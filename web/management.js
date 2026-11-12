import { OP, STATUS, CAP_RESTART, CAP_RADIO_DIAGNOSTICS, RESTART_KEY, ManagementError, encodeFrame, decodeFrame, decodeIdentity, decodeSummary, decodeRadio,
  hex, unhex, words, word, concat, crc32 } from './protocol.js';

export const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const randomId = () => crypto.getRandomValues(new Uint32Array(1))[0] || 1;
export class Controller {
  constructor(connection, { timeout = 6000, attempts = 3, writeAttempts = 8 } = {}) {
    this.connection = connection;
    this.timeout = timeout;
    this.attempts = attempts;
    this.writeAttempts = writeAttempts;
    this.sequence = randomId();
    this.boots = new Map();
  }
  async request(slot, op, data = new Uint8Array(), { offset = 0, force = false, discover = false, allow = [], timeout = this.timeout, attempts: requestedAttempts } = {}) {
    this.sequence = ((this.sequence + 1) >>> 0) || 1;
    const local = slot === 0;
    const request = { slot: local ? 1 : slot, op, id: this.sequence, target: discover ? 0 : (this.boots.get(slot) ?? 0),
      offset, flags: force ? 4 : 0, data };
    const command = local ? `master ${hex(encodeFrame(request))}` : `remote ${slot} ${hex(encodeFrame(request))}`;
    // Large images need thousands of writes; tolerate bursts of lost replies
    // without restarting a healthy upload. Every retry keeps the same ID/data.
    const attempts = requestedAttempts ?? ([OP.WRITE, OP.CACHE_WRITE].includes(op) ? Math.max(this.attempts, this.writeAttempts) : this.attempts);
    for (let attempt = 0; attempt < attempts; attempt++) {
      let response;
      try {
        response = await this.connection.exchange(command, line => {
          const match = line.match(local ? /^KMSMASTER ([\da-f]+)$/i : /^KMSM (\d+) ([\da-f]+)$/i);
          if (!match || (!local && Number(match[1]) !== slot)) return;
          let frame;
          try { frame = decodeFrame(unhex(match[local ? 1 : 2])); } catch { return; }
          if (frame.id === request.id && frame.slot === request.slot && frame.op === op && frame.offset === offset &&
              (frame.flags & 1) && !(frame.flags & 2) &&
              (discover || !request.target || frame.sender === request.target || frame.status === STATUS.WRONG_BOOT)) return frame;
        }, timeout);
      } catch (error) {
        if (!this.connection.connected) throw error;
        continue;
      }
      if (response.status === STATUS.TIMEOUT && !allow.includes(STATUS.TIMEOUT)) continue;
      if (response.status !== STATUS.OK && !allow.includes(response.status)) throw new ManagementError(response.status);
      return response;
    }
    throw new ManagementError(STATUS.TIMEOUT, [OP.WRITE, OP.CACHE_WRITE].includes(op)
      ? `Upload to ${local ? "master" : `receiver ${slot}`} timed out at byte ${offset}; retry the update` : undefined);
  }
  async identity(slot) {
    const response = await this.request(slot, OP.INFO, undefined, { discover: true });
    const identity = decodeIdentity(response.data);
    this.boots.set(slot, response.sender);
    return { ...identity, boot: response.sender };
  }
  async status(slot) {
    const identity = await this.identity(slot);
    const diagnostics = decodeSummary((await this.request(slot, OP.STATS)).data);
    const result = { identity, diagnostics };
    if (identity.capabilities & CAP_RADIO_DIAGNOSTICS) {
      try {
        result.radio = decodeRadio((await this.request(slot, OP.RADIO_STATUS)).data);
        if (result.radio.boot !== identity.boot) throw new Error('Radio status boot does not match identity');
      } catch (error) { delete result.radio; result.radioError = error.message; }
    }
    return result;
  }
  async restart(slot, { beforeRestart = async () => {}, timeout = 20000, sleep = delay, now = () => performance.now() } = {}) {
    if (!Number.isInteger(slot) || slot < 1 || slot > 7) throw new Error('Choose a receiver from 1 to 7');
    if (!((await this.identity(0)).capabilities & CAP_RESTART)) throw new ManagementError(STATUS.UNSUPPORTED,
      'Update the S3 firmware to restart dongles through the controller');
    const before = await this.identity(slot);
    if (!(before.capabilities & CAP_RESTART)) throw new ManagementError(STATUS.UNSUPPORTED,
      `Receiver ${slot} needs updated firmware for remote restart`);
    await beforeRestart();
    try { await this.request(slot, OP.RESTART, words(RESTART_KEY), { attempts: 1 }); }
    catch (error) {
      // A missing reply can mean the receiver has already restarted. Probe its
      // boot identity rather than submitting another restart command.
      if (!(error instanceof ManagementError) || ![STATUS.TIMEOUT, STATUS.WRONG_BOOT].includes(error.code)) throw error;
    }
    const deadline = now() + timeout;
    while (now() < deadline) {
      await sleep(200);
      try {
        const response = await this.request(slot, OP.INFO, undefined, { discover: true, attempts: 1 });
        const after = { ...decodeIdentity(response.data), boot: response.sender };
        this.boots.set(slot, response.sender);
        if (after.boot !== before.boot && after.confirmed) {
          if (after.sha256 !== before.sha256) throw new Error(`Receiver ${slot} returned with different firmware after restarting`);
          return { slot, result: 'restarted', before: before.boot, after: after.boot };
        }
      } catch (error) {
        if (!(error instanceof ManagementError) || ![STATUS.TIMEOUT, STATUS.WRONG_BOOT].includes(error.code)) throw error;
      }
    }
    throw new ManagementError(STATUS.TIMEOUT, `Receiver ${slot} did not return with a new confirmed boot after restarting`);
  }
  async peers() {
    const slots = [];
    let selected = 0;
    return this.connection.exchange('slaves', line => {
      const peer = line.match(/^KMSPEER ([1-7]) [\da-f]{8}$/i);
      if (peer && !slots.includes(Number(peer[1]))) slots.push(Number(peer[1]));
      const route = line.match(/^KMSROUTE CURRENT ([0-7])$/);
      if (route) selected = Number(route[1]);
      if (line === 'KMSPEERS END') return { slots, selected };
    }, this.timeout);
  }
  async unpair(slot) {
    if (!Number.isInteger(slot) || slot < 1 || slot > 7) throw new Error('Choose a valid receiver');
    await this.connection.exchange(`unpair ${slot}`, line => {
      const match = line.match(/^KMSUNPAIR (\d+) (\w+)$/);
      if (!match || Number(match[1]) !== slot) return;
      if (match[2] !== 'OK') throw new Error(`Could not unpair receiver ${slot}: ${match[2]}`);
      return true;
    }, this.timeout);
    this.boots.delete(slot);
  }
  async select(slot) {
    return this.connection.exchange(`select ${slot}`, line => {
      if (line.startsWith('KMSROUTE ERROR')) throw new Error(`Receiver ${slot} is unavailable; select it when it returns`);
      if (line === `KMSROUTE SELECTED ${slot}`) return true;
    }, this.timeout);
  }
  async route(paused) {
    return this.connection.exchange(paused ? 'pause' : 'resume', line => {
      if (line.includes('KMSROUTE ERROR')) throw new Error('Previous route is unavailable; select a receiver when it returns');
      if (line.includes(paused ? 'KMSROUTE PAUSED' : 'KMSROUTE RESUMED')) return true;
    }, this.timeout);
  }
  async history(slot, progress = () => {}) {
    await this.identity(slot);
    const begin = await this.request(slot, OP.HISTORY_BEGIN);
    if (begin.data.length !== 12) throw new Error('Invalid history snapshot header');
    const token = word(begin.data), size = word(begin.data, 4), checksum = word(begin.data, 8);
    if (!size || size > 98304) throw new Error('History snapshot exceeds the transfer limit');
    const result = new Uint8Array(size);
    for (let offset = 0; offset < size;) {
      const response = await this.request(slot, OP.HISTORY_READ, words(token), { offset });
      if (!response.data.length || response.data.length > size - offset) throw new Error('Invalid history chunk length');
      result.set(response.data, offset);
      offset += response.data.length;
      progress(offset, size);
    }
    if (crc32(result) !== checksum) throw new ManagementError(STATUS.INTEGRITY, 'Downloaded history checksum mismatch');
    try { await this.request(slot, OP.HISTORY_READ, words(token), { offset: size }); }
    catch { /* Older firmware expires snapshots instead of accepting EOF release. */ }
    return result;
  }
  async clearHistory(slot) {
    await this.identity(slot);
    await this.request(slot, OP.CLEAR);
  }
  async cacheIdentity() {
    const master = await this.identity(0);
    if (!(master.capabilities & 8)) throw new ManagementError(STATUS.UNSUPPORTED,
      'Receiver cache requires the one-time wired S3 partition-table installation');
    const response = await this.request(0, OP.CACHE_INFO, undefined, { allow: [STATUS.EXPIRED] });
    return response.status === STATUS.EXPIRED ? null : decodeIdentity(response.data);
  }
}

export async function restartTargets(controller, targets, options = {}) {
  const { slots, selected } = await controller.peers();
  if (!targets.length || targets.some(slot => !Number.isInteger(slot) || !slots.includes(slot))) {
    throw new Error('Choose at least one paired receiver');
  }
  const ordered = [...new Set(targets.filter(slot => slot !== selected))].concat(targets.includes(selected) ? [selected] : []);
  const results = [], warnings = [];
  let paused = false;
  try {
    for (const slot of ordered) {
      options.onProgress?.(slot);
      let row;
      try {
        row = await controller.restart(slot, { ...options, beforeRestart: async () => {
          if ((await controller.peers()).selected === slot) { paused = true; await controller.route(true); }
        } });
      } catch (error) {
        row = { slot, result: error instanceof ManagementError && error.code === STATUS.TIMEOUT ? 'unreachable' :
          error instanceof ManagementError && error.code === STATUS.UNSUPPORTED ? 'unsupported' : 'failed', detail: error.message };
      }
      results.push(row); options.onResult?.(row);
    }
  } finally {
    if (paused) {
      try { await controller.route(false); }
      catch (error) { warnings.push(`Could not restore input after restarting: ${error.message}`); }
    }
  }
  return { results, warnings, success: results.every(row => row.result === 'restarted') && !warnings.length };
}

async function sha256(bytes) { return hex(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes))); }
export async function validateFirmware(image, metadata) {
  const target = metadata?.role === 'controller' ? { chip: 9, capacity: 0x300000 } :
    metadata?.role === 'receiver' ? { chip: 2, capacity: 0x1e0000 } : null;
  if (!target || metadata?.schema !== 1 || metadata.chip !== target.chip ||
      metadata.management_protocol !== 1 || metadata.input_protocol !== 2 || metadata.ota_schema !== 1) {
    throw new Error('Choose a controller or receiver .bin and its generated OTA-capable JSON manifest');
  }
  if (image.length < 56 || image.length > target.capacity || image[0] !== 0xe9 || image[12] !== target.chip || image[13] !== 0) {
    throw new Error('Image is incompatible with the selected role’s chip or OTA slots');
  }
  if (metadata.size !== image.length || metadata.sha256 !== await sha256(image)) {
    throw new ManagementError(STATUS.INTEGRITY, 'Firmware does not match its manifest');
  }
  if (image[23] !== 1 || await sha256(image.subarray(0, -32)) !== hex(image.subarray(-32))) {
    throw new ManagementError(STATUS.INTEGRITY, 'ESP image appended SHA-256 is missing or invalid');
  }
  const firmware = { image, metadata };
  if (metadata.receiver_firmware) {
    if (metadata.role !== 'controller') throw new Error('Only controller manifests can bundle receiver firmware');
    const bundle = metadata.receiver_firmware;
    if (bundle.metadata?.role !== 'receiver' || bundle.metadata.receiver_firmware ||
        typeof bundle.image_base64 !== 'string' || bundle.image_base64.length > 0x280000) throw new Error('Invalid bundled receiver firmware');
    const bytes = Uint8Array.from(atob(bundle.image_base64), char => char.charCodeAt(0));
    firmware.receiver = await validateFirmware(bytes, bundle.metadata);
    if (bundle.metadata.git !== metadata.git) throw new Error('Bundled receiver firmware comes from another source revision');
  }
  return firmware;
}

function cacheIdentityBytes(metadata) {
  const bytes = new Uint8Array(120), encoder = new TextEncoder();
  const git = encoder.encode(metadata.git), utc = encoder.encode(metadata.utc);
  if (git.length >= 48 || utc.length >= 24) throw new Error('Firmware identity is too long');
  bytes.set(git); bytes.set(utc, 48); bytes.set(unhex(metadata.sha256), 72);
  bytes.set(words(metadata.size, 2, 3, 1), 104);
  return bytes;
}

export async function stageCache(controller, firmware, { progress = () => {} } = {}) {
  const { image, metadata } = firmware;
  if (metadata.role !== 'receiver') throw new Error('Choose S2 receiver firmware to refresh the cache');
  const current = await controller.cacheIdentity();
  if (current?.sha256 === metadata.sha256) return 'skipped';
  const token = randomId();
  let started = false;
  try {
    let response = await controller.request(0, OP.CACHE_BEGIN, concat(words(token), cacheIdentityBytes(metadata)), { allow: [STATUS.CURRENT] });
    if (response.status === STATUS.CURRENT) return 'skipped';
    started = true;
    if (response.data.length !== 4 || word(response.data) !== 0) throw new Error('Invalid initial cache offset');
    for (let offset = 0; offset < image.length; offset += 188) {
      const chunk = image.subarray(offset, offset + 188);
      response = await controller.request(0, OP.CACHE_WRITE, concat(words(token), chunk), { offset });
      if (response.data.length !== 4 || word(response.data) !== offset + chunk.length) throw new ManagementError(4);
      progress(offset + chunk.length, image.length, 'Saving receiver firmware on S3');
    }
    await controller.request(0, OP.CACHE_END, words(token));
    if ((await controller.cacheIdentity())?.sha256 !== metadata.sha256) throw new ManagementError(STATUS.INTEGRITY);
    return 'updated';
  } catch (error) {
    if (started) { try { await controller.request(0, OP.CACHE_ABORT, words(token)); } catch { /* Inactive cache uploads expire. */ } }
    throw error;
  }
}

export async function updateCachedOne(controller, slot, firmware, { force = false, beforeWrite = async () => {},
  progress = () => {}, sleep = delay, now = () => performance.now() } = {}) {
  const { metadata } = firmware, before = await controller.identity(slot);
  if (before.sha256 === metadata.sha256 && !force) return 'skipped';
  await beforeWrite();
  const begin = await controller.request(0, OP.CACHE_INSTALL, concat(words(slot), unhex(metadata.sha256)), { force });
  if (begin.data.length !== 4) throw new Error('Invalid cached-update token');
  const token = word(begin.data);
  let last = '', deadline = now() + 120000;
  const stages = ['', 'Checking receiver', 'Starting update', 'Sending cached image', 'Verifying receiver image',
    'Restarting receiver', 'Confirming receiver boot', 'Updated', 'Already current', 'Failed'];
  while (now() < deadline) {
    await sleep(250);
    const response = await controller.request(0, OP.CACHE_PROGRESS, words(token)), data = response.data;
    if (data.length !== 24 || word(data) !== token || word(data, 20) !== slot ||
        word(data, 16) !== metadata.size || word(data, 12) > metadata.size) throw new Error('Invalid cached-update progress');
    const state = word(data, 4), offset = word(data, 12), result = word(data, 8);
    if (last !== `${state}:${offset}`) { last = `${state}:${offset}`; deadline = now() + 120000; }
    progress(offset, metadata.size, stages[state] ?? 'Updating receiver');
    if (state === 7 || state === 8) return state === 7 ? 'updated' : 'skipped';
    if (state === 9) throw new ManagementError(result);
  }
  throw new ManagementError(STATUS.TIMEOUT, 'Cached update stopped making progress');
}

export async function updateOne(controller, slot, firmware, { force = false, beforeWrite = async () => {},
  progress = () => {}, sleep = delay, now = () => performance.now() } = {}) {
  const { image, metadata } = firmware, master = slot === 0;
  const chip = master ? 9 : 2, role = master ? 'controller' : 'receiver';
  if (metadata.role !== role || metadata.chip !== chip) throw new Error('Firmware role does not match the update target');
  const before = await controller.identity(slot);
  if (before.chip !== chip || !(before.capabilities & 2)) {
    throw new ManagementError(STATUS.UNSUPPORTED, 'Device needs the one-time wired OTA installation');
  }
  if (before.sha256 === metadata.sha256 && !force) return 'skipped';
  if (!before.confirmed) throw new ManagementError(STATUS.BUSY, 'Device has not confirmed its current firmware');
  await beforeWrite();
  const token = randomId(), begin = concat(words(token, image.length), unhex(metadata.sha256), words(chip));
  let started = false, activating = false;
  try {
    let response;
    for (let attempt = 0; attempt < 8; attempt++) {
      response = await controller.request(slot, OP.BEGIN, begin, { force, allow: [STATUS.CURRENT, STATUS.BUSY] });
      if (response.status !== STATUS.BUSY) break;
      await sleep(500);
    }
    if (response.status === STATUS.CURRENT) return 'skipped';
    if (response.status === STATUS.BUSY) throw new ManagementError(STATUS.BUSY);
    started = true;
    if (hex(response.data) !== hex(words(0))) throw new Error('Unexpected initial upload offset');
    for (let offset = 0; offset < image.length; offset += 188) {
      const chunk = image.subarray(offset, offset + 188);
      response = await controller.request(slot, OP.WRITE, concat(words(token), chunk), { offset });
      if (hex(response.data) !== hex(words(offset + chunk.length))) throw new ManagementError(4);
      progress(offset + chunk.length, image.length, 'Uploading');
    }
    progress(image.length, image.length, 'Verifying image');
    await controller.request(slot, OP.END, words(token));
    if (master) await controller.connection.prepareForMasterReboot?.();
    activating = true;
    progress(image.length, image.length, 'Waiting for confirmed reboot');
    try {
      // A lost master ACK must not retry at the old rate while the new firmware
      // is already starting at 115200. Receiver activation keeps its retry policy.
      await controller.request(slot, OP.ACTIVATE, words(token), master ? { attempts: 1, timeout: 1000 } : {});
    }
    catch (error) {
      if (!(error instanceof ManagementError) || ![STATUS.TIMEOUT, STATUS.WRONG_BOOT].includes(error.code)) {
        // A negative activation reply is definitive: release the verified upload.
        // A missing reply/restarted target remains ambiguous and is probed below.
        activating = false;
        if (master) controller.connection.cancelMasterReboot?.();
        throw error;
      }
    }
    if (master) {
      await controller.connection.reopenForMasterReboot?.();
      progress(image.length, image.length, 'Confirming reboot at 115200 baud');
    }
    const deadline = now() + 60000;
    while (now() < deadline) {
      await sleep(500);
      let after;
      try { after = await controller.identity(slot); }
      catch (error) {
        if (error instanceof ManagementError && [STATUS.TIMEOUT, STATUS.WRONG_BOOT].includes(error.code)) continue;
        throw error;
      }
      if (after.boot !== before.boot && after.sha256 === metadata.sha256 && after.confirmed) return 'updated';
      if (metadata.sha256 !== before.sha256 && after.boot !== before.boot && after.sha256 === before.sha256) {
        throw new ManagementError(STATUS.INTEGRITY, 'Device rolled back to its previous firmware');
      }
    }
    throw new ManagementError(STATUS.TIMEOUT, 'Expected firmware was not confirmed after reboot');
  } catch (error) {
    if (started && !activating) {
      try { await controller.request(slot, OP.ABORT, words(token)); } catch { /* Receiver also expires abandoned transfers. */ }
    }
    throw error;
  }
}

export async function updateTargets(controller, targets, selected, firmware, options = {}) {
  const results = [], warnings = [];
  let paused = false;
  let cached = options.cached ?? false;
  if (options.useCache && !cached && (await controller.identity(0)).capabilities & 8) {
    await stageCache(controller, firmware, { progress: (done, total, stage) => options.progress?.(0, done, total, stage) });
    cached = true;
  }
  const ordered = targets.filter(slot => slot !== selected).concat(targets.includes(selected) ? [selected] : []);
  try {
    for (const slot of ordered) {
      if (!controller.connection.connected) {
        const record = { slot, result: 'not attempted', detail: 'Serial connection closed' };
        results.push(record); options.onResult?.(record); continue;
      }
      let record;
      try {
        const result = await (cached ? updateCachedOne : updateOne)(controller, slot, firmware, { ...options,
          beforeWrite: async () => { if (slot === selected && !paused) { paused = true; await controller.route(true); } },
          progress: (done, total, stage) => options.progress?.(slot, done, total, stage) });
        record = { slot, result };
      } catch (error) {
        record = { slot, result: error instanceof ManagementError && error.code === STATUS.TIMEOUT ? 'unreachable' : 'failed', detail: error.message };
      }
      results.push(record);
      options.onResult?.(record);
    }
  } finally {
    if (paused) {
      try { await controller.route(false); }
      catch (error) { warnings.push(`Could not confirm route restoration: ${error.message}`); }
    }
    options.onComplete?.({ results, warnings });
  }
  return { results, warnings };
}

// The UART bridge stays enumerated while the master restarts. The first new OTA
// boot holds routing paused until confirmation and this explicit restoration.
export async function updateMaster(controller, firmware, options = {}) {
  const warnings = [];
  const uploadBaudRate = controller.connection?.baudRate ?? 115200;
  let previous, paused = false, result, detail;
  try {
    previous = await controller.peers();
    result = await updateOne(controller, 0, firmware, { ...options,
      beforeWrite: async () => { paused = true; await controller.route(true); } });
    if (result === 'updated' && uploadBaudRate > 115200 && controller.connection.baudRate !== uploadBaudRate) {
      options.progress?.(firmware.image.length, firmware.image.length, 'Restoring console speed');
      try { await controller.connection.negotiate(uploadBaudRate); }
      catch (error) { warnings.push(`Master updated, but could not restore ${uploadBaudRate} baud: ${error.message}`); }
    }
    if (firmware.receiver) {
      try { await stageCache(controller, firmware.receiver, options); }
      catch (error) { warnings.push(`Master application checked, but receiver cache could not be refreshed: ${error.message}`); }
    }
    return { result, warnings };
  } catch (error) {
    detail = error.message; throw error;
  } finally {
    if (paused) {
      try {
        if (previous.selected) await controller.select(previous.selected);
        else await controller.route(true);
      } catch (error) { warnings.push(`Could not restore the previous input destination: ${error.message}`); }
    }
    options.onComplete?.({ result, detail, warnings });
  }
}
