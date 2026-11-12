// Management v1: matches kms_management.h and manage-slaves.py.
export const OP = Object.freeze({ INFO: 1, STATS: 2, HISTORY_BEGIN: 3, HISTORY_READ: 4,
  CLEAR: 5, BEGIN: 6, WRITE: 7, END: 8, ACTIVATE: 9, ABORT: 10,
  CACHE_INFO: 13, CACHE_BEGIN: 14, CACHE_WRITE: 15, CACHE_END: 16,
  CACHE_ABORT: 17, CACHE_INSTALL: 18, CACHE_PROGRESS: 19, RESTART: 20, RADIO_STATUS: 21 });
export const CAP_RESTART = 16, RESTART_KEY = 0x52535431;
export const CAP_RADIO_DIAGNOSTICS = 32;
export const STATUS = Object.freeze({ OK: 0, BUSY: 2, WRONG_BOOT: 3, INTEGRITY: 5,
  UNSUPPORTED: 7, CURRENT: 8, EXPIRED: 9, TIMEOUT: 11 });
export const RESULTS = ['ok', 'bad request', 'busy', 'receiver restarted', 'unexpected offset',
  'integrity failure', 'storage failure', 'unsupported', 'already current', 'expired session',
  'incompatible image', 'timeout/unreachable'];
export class ManagementError extends Error {
  constructor(code, message = RESULTS[code]) { super(message); this.code = code; }
}
export const hex = bytes => Array.from(bytes, value => value.toString(16).padStart(2, '0')).join('');
export function unhex(text) {
  if (!/^(?:[\da-f]{2})*$/i.test(text)) throw new Error('Invalid hexadecimal data');
  return Uint8Array.from(text.match(/../g) ?? [], value => parseInt(value, 16));
}
export function words(...values) {
  const bytes = new Uint8Array(values.length * 4), view = new DataView(bytes.buffer);
  values.forEach((value, index) => view.setUint32(index * 4, value, true));
  return bytes;
}
export const word = (bytes, at = 0) => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(at, true);
export function concat(...parts) {
  const result = new Uint8Array(parts.reduce((size, part) => size + part.length, 0));
  let at = 0;
  for (const part of parts) { result.set(part, at); at += part.length; }
  return result;
}
export function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return (crc ^ 0xffffffff) >>> 0;
}
export function encodeFrame({ op, slot, id, sender = 0, target = 0, offset = 0,
  flags = 0, status = 0, data = new Uint8Array() }) {
  const uint = value => Number.isInteger(value) && value >= 0 && value <= 0xffffffff;
  if (!Number.isInteger(op) || op < 1 || op > OP.RADIO_STATUS || !Number.isInteger(slot) || slot < 1 || slot > 7 ||
      !uint(id) || !id || ![sender, target, offset].every(uint) || !Number.isInteger(flags) || flags < 0 || flags > 7 ||
      !Number.isInteger(status) || status < 0 || status > 11 || !(data instanceof Uint8Array) || data.length > 192) {
    throw new Error('Invalid management frame');
  }
  const bytes = new Uint8Array(34 + data.length), view = new DataView(bytes.buffer);
  bytes.set([75, 77, 83, 77, 1, op, flags, status, slot]);
  [id, sender, target, offset].forEach((value, index) => view.setUint32(12 + index * 4, value, true));
  view.setUint16(28, data.length, true);
  bytes.set(data, 30);
  view.setUint32(bytes.length - 4, crc32(bytes.subarray(0, -4)), true);
  return bytes;
}
export function decodeFrame(bytes) {
  if (bytes.length < 34 || bytes.length > 226) throw new Error('Invalid management length');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (hex(bytes.subarray(0, 5)) !== '4b4d534d01' || bytes[9] || bytes[10] || bytes[11] ||
      view.getUint16(28, true) + 34 !== bytes.length || crc32(bytes.subarray(0, -4)) !== word(bytes, bytes.length - 4)) {
    throw new Error('Invalid management header or CRC');
  }
  const frame = { op: bytes[5], flags: bytes[6], status: bytes[7], slot: bytes[8], id: word(bytes, 12),
    sender: word(bytes, 16), target: word(bytes, 20), offset: word(bytes, 24), data: bytes.slice(30, -4) };
  encodeFrame(frame);
  return frame;
}
export function decodeIdentity(data) {
  const text = (start, end) => {
    const bytes = data.subarray(start, end), nul = bytes.indexOf(0);
    if (nul < 0 || bytes.subarray(0, nul).some(value => value > 127)) throw new Error('Invalid firmware identity text');
    return new TextDecoder().decode(bytes.subarray(0, nul));
  };
  if (data.length !== 120 || word(data, 116) !== 1) throw new Error('Invalid firmware identity');
  const capabilities = word(data, 112);
  return { git: text(0, 48), utc: text(48, 72), sha256: hex(data.subarray(72, 104)),
    size: word(data, 104), chip: word(data, 108), capabilities, confirmed: Boolean(capabilities & 4) };
}
export function decodeRadio(data) {
  if (data.length !== 80 || word(data) !== 1) throw new Error('Invalid radio diagnostic status');
  const names = ['schema', 'boot', 'sampleMs', 'txSubmitted', 'txCompleted', 'txFailed', 'txSlow', 'txRejected',
    'maxTxMs', 'lastTxMs', 'txPendingMs', 'rxPackets', 'lastRxAgeMs', 'maxRxGapMs', 'maxDispatchMs',
    'lastDispatchMs', 'maxTickMs', 'rxQueue', 'usbReady', 'minHeap'];
  return Object.fromEntries(names.map((name, index) => [name, word(data, index * 4)]));
}
export function decodeSummary(data) {
  if (data.length !== 168) throw new Error('Invalid diagnostic summary');
  const values = Array.from({ length: 42 }, (_, i) => word(data, i * 4));
  return { boot: values[0].toString(16).padStart(8, '0'), resetReason: values[1],
    uptimeMs: values[2] + values[3] * 0x100000000,
    metrics: Object.fromEntries(['radioRx', 'radioTx', 'duplicates', 'rejected', 'accepted', 'usbCompleted',
      'peakRxQueue', 'peakUsbQueue', 'maxUsbMs', 'minHeap', 'usbFlags'].map((name, i) => [name, values[i + 4]])),
    totals: Object.fromEntries(['boot', 'usbMount', 'usbSuspend', 'usbResume', 'usbUnmount', 'usbStall',
      'usbOverflow', 'radioOverflow', 'leaseExpired', 'outputClear', 'radioError', 'updateBegin',
      'updateVerified', 'updateAbort', 'updateConfirmed', 'storageError'].map((name, i) => [name, values[i + 15]])),
    rssi: values[31] | 0, meanRssi: values[32] | 0, lastRadioMs: values[33],
    events: { retained: values[34], capacity: values[35], overwritten: values[36] },
    checkpoints: { retained: values[37], capacity: values[38], overwritten: values[39] },
    oldest: { boot: values[40], uptimeMs: values[41] } };
}
