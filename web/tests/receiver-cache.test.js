import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { OP, STATUS, words, word, concat, hex, unhex, encodeFrame, decodeFrame } from '../protocol.js';
import { validateFirmware, stageCache, updateCachedOne, updateTargets } from '../management.js';

function firmware(role = 'receiver') {
  const image = new Uint8Array(544), chip = role === 'receiver' ? 2 : 9;
  image[0] = 0xe9; image[12] = chip; image[23] = 1;
  image.set([0x32, 0x54, 0xcd, 0xab], 32);
  image.set(new TextEncoder().encode(`kmswitcher_${role}\0`), 80);
  image.set(createHash('sha256').update(image.subarray(0, -32)).digest(), image.length - 32);
  return { image, metadata: { schema: 1, role, chip, git: 'a'.repeat(40), utc: '2026-10-28T00:00:00Z',
    size: image.length, sha256: createHash('sha256').update(image).digest('hex'),
    management_protocol: 1, input_protocol: 2, ota_schema: 1 } };
}

class CacheController {
  constructor(fw) {
    this.firmware = fw; this.current = null; this.calls = []; this.received = []; this.routes = [];
    this.now = 0; this.connection = { connected: true }; this.states = [];
  }
  async identity() { return { capabilities: 15, sha256: '0'.repeat(64) }; }
  async cacheIdentity() { return this.current; }
  async route(paused) { this.routes.push(paused); }
  async request(slot, op, data, options = {}) {
    this.calls.push({ slot, op, data, ...options });
    let reply = new Uint8Array();
    if (op === OP.CACHE_BEGIN) reply = words(0);
    else if (op === OP.CACHE_WRITE) {
      assert.equal(options.offset, this.received.length);
      this.received.push(...data.subarray(4)); reply = words(this.received.length + (this.wrongOffset ? 1 : 0));
    } else if (op === OP.CACHE_END) {
      assert.equal(hex(Uint8Array.from(this.received)), hex(this.firmware.image)); this.current = this.firmware.metadata;
    } else if (op === OP.CACHE_INSTALL) {
      this.slot = word(data); this.states = [3, 6, 7]; reply = words(123);
    } else if (op === OP.CACHE_PROGRESS) {
      const state = this.fail ? 9 : this.states.shift();
      reply = words(123, state, this.fail ? STATUS.INTEGRITY : 0,
        this.firmware.image.length, this.firmware.image.length, this.slot);
    }
    return { status: STATUS.OK, data: reply };
  }
  options() { return { sleep: async ms => { this.now += ms; }, now: () => this.now }; }
}

test('controller manifest verifies its complete bundled receiver before any transfer', async () => {
  const receiver = firmware(), master = firmware('controller');
  master.metadata.receiver_firmware = { metadata: receiver.metadata, image_base64: Buffer.from(receiver.image).toString('base64') };
  assert.equal((await validateFirmware(master.image, master.metadata)).receiver.metadata.sha256, receiver.metadata.sha256);
  master.metadata.receiver_firmware.image_base64 = Buffer.from(new Uint8Array(544)).toString('base64');
  await assert.rejects(validateFirmware(master.image, master.metadata));
});

test('cache upload commits one image, skips matching content, and aborts a wrong offset', async () => {
  const fw = firmware(), controller = new CacheController(fw);
  assert.equal(await stageCache(controller, fw), 'updated');
  const count = controller.calls.length;
  assert.equal(await stageCache(controller, fw), 'skipped');
  assert.equal(controller.calls.length, count);
  controller.current = null; controller.received = []; controller.wrongOffset = true;
  await assert.rejects(stageCache(controller, fw));
  assert.equal(controller.calls.at(-1).op, OP.CACHE_ABORT);
});

test('cached receiver update polls S3 progress through confirmation without transferring firmware bytes', async () => {
  const fw = firmware(), controller = new CacheController(fw);
  assert.equal(await updateCachedOne(controller, 3, fw, controller.options()), 'updated');
  assert.equal(controller.calls.filter(call => call.op === OP.CACHE_PROGRESS).length, 3);
  assert.ok(controller.calls.every(call => call.slot === 0));
  assert.ok(controller.calls.every(call => ![OP.WRITE, OP.CACHE_WRITE].includes(call.op)));
  controller.fail = true;
  await assert.rejects(updateCachedOne(controller, 3, fw, controller.options()), /integrity/);
});

test('receiver batch stages once, updates selected receiver last, and restores routing', async () => {
  const fw = firmware(), controller = new CacheController(fw);
  const result = await updateTargets(controller, [3, 1], 3, fw, { useCache: true, ...controller.options() });
  assert.deepEqual(result.results, [{ slot: 1, result: 'updated' }, { slot: 3, result: 'updated' }]);
  assert.equal(controller.calls.filter(call => call.op === OP.CACHE_BEGIN).length, 1);
  assert.equal(controller.received.length, fw.image.length);
  assert.deepEqual(controller.routes, [true, false]);
  assert.ok(!controller.calls.some(call => call.op === OP.WRITE));
});

test('cache wire operations retain the bounded management envelope', () => {
  for (let op = OP.CACHE_INFO; op <= OP.CACHE_PROGRESS; ++op) {
    const frame = { op, slot: 1, id: 123, data: concat(words(123), unhex('aa'.repeat(188))) };
    assert.equal(encodeFrame(frame).length, 226);
    assert.equal(decodeFrame(encodeFrame(frame)).op, op);
  }
});
