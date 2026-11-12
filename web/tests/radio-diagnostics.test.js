import test from 'node:test';
import assert from 'node:assert/strict';
import { OP, encodeFrame, decodeFrame, decodeRadio, words } from '../protocol.js';
import { SerialConnection } from '../serial.js';
import { Controller } from '../management.js';
import { DemoPort } from '../demo.js';

test('radio status decodes delivery failures and timing without changing old summaries', () => {
  const sample = decodeRadio(words(1, 22, ...Array.from({ length: 18 }, (_, i) => i + 2)));
  assert.equal(sample.txFailed, 5); assert.equal(sample.maxRxGapMs, 13); assert.equal(sample.minHeap, 19);
  for (const bytes of [new Uint8Array(), new Uint8Array(80), new Uint8Array(84)]) assert.throws(() => decodeRadio(bytes));
  const frame = { op: OP.RADIO_STATUS, slot: 2, id: 123, target: 22 };
  assert.equal(decodeFrame(encodeFrame(frame)).op, 21);
});
test('demo exercises the real browser radio diagnostic request and detail object', async t => {
  const connection = new SerialConnection(); await connection.open(new DemoPort());
  t.after(() => connection.close());
  const status = await new Controller(connection).status(2);
  assert.equal(status.radio.boot, status.identity.boot);
  assert.equal(status.radio.txFailed, 0); assert.equal(status.radio.usbReady, 1);
});
