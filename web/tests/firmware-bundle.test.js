import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { zipSync, strToU8 } from 'fflate';
import { readFirmwareBundle } from '../firmware-bundle.js';

const hash = data => createHash('sha256').update(data).digest('hex');
function fixture() {
  const files = {}, roles = {}, images = {};
  for (const [role, chip, capacity] of [['receiver', 2, 0x400000], ['controller', 9, 0x1000000]]) {
    const image = new Uint8Array(544);
    image[0] = 0xe9; image[12] = chip; image[23] = 1;
    image.set(createHash('sha256').update(image.subarray(0, -32)).digest(), image.length - 32);
    const metadata = { schema: 1, role, chip, git: 'a'.repeat(40), utc: '2026-10-29T00:00:00Z',
      size: image.length, sha256: hash(image), management_protocol: 1, input_protocol: 2, ota_schema: 1 };
    images[role] = { image, metadata };
    if (role === 'controller') metadata.receiver_firmware = { metadata: images.receiver.metadata, image_base64: Buffer.from(images.receiver.image).toString('base64') };
    const entries = [];
    function add(name, data, offset) {
      files[`${role}/${name}`] = data;
      entries.push({ path: `${role}/${name}`, size: data.length, sha256: hash(data), offset });
    }
    add(`kmswitcher_${role}.bin`, image, role === 'controller' ? 0x10000 : 0x20000);
    add('boot.bin', new Uint8Array(32), role === 'controller' ? 0 : 0x1000);
    add('table.bin', new Uint8Array(32), 0x8000);
    add('ota.bin', new Uint8Array(32), role === 'controller' ? 0x310000 : 0x10000);
    if (role === 'controller') {
      const cache = new Uint8Array(8192).fill(255), view = new DataView(cache.buffer);
      cache.fill(0, 0, 144); cache.set(strToU8('KMSC')); view.setUint32(4, 1, true); view.setUint32(8, 1, true); view.setUint32(12, 4096, true);
      cache.set(strToU8(images.receiver.metadata.git), 16); cache.set(strToU8(images.receiver.metadata.utc), 64);
      cache.set(Buffer.from(images.receiver.metadata.sha256, 'hex'), 88);
      for (const [offset, value] of [[120, 544], [124, 2], [128, 3], [132, 1]]) view.setUint32(offset, value, true);
      let crc = 0xffffffff;
      for (const byte of cache.subarray(0, 136)) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
      }
      view.setUint32(136, (crc ^ 0xffffffff) >>> 0, true); cache.set(strToU8('DONE'), 140);
      cache.set(images.receiver.image, 4096);
      add('receiver-cache.bin', cache, 0x960000); add('receiver-cache-empty.bin', new Uint8Array(4096).fill(255), 0xb60000);
    }
    roles[role] = { chip: role === 'controller' ? 'esp32s3' : 'esp32s2', capacity, files: entries };
    files[`${role}/kmswitcher_${role}.json`] = strToU8(JSON.stringify(metadata));
  }
  const bundle = { schema: 1, version: '1.2.3', commit: 'a'.repeat(40), roles };
  const pack = () => zipSync({ ...files, 'bundle.json': strToU8(JSON.stringify(bundle)) });
  return { files, bundle, pack };
}

test('one ZIP supplies both roles and the exact slave cached on the master', async () => {
  const f = fixture();
  assert.equal((await readFirmwareBundle(f.pack(), 'controller')).receiver.metadata.sha256,
    (await readFirmwareBundle(f.pack(), 'receiver')).metadata.sha256);
});

test('ZIP import rejects corruption, mixed revisions, unsafe paths and a mismatched master cache', async () => {
  let f = fixture(); f.files['receiver/kmswitcher_receiver.bin'][55] ^= 1;
  await assert.rejects(readFirmwareBundle(f.pack(), 'receiver'), /manifest/);
  f = fixture(); f.bundle.commit = 'b'.repeat(40);
  await assert.rejects(readFirmwareBundle(f.pack(), 'controller'), /revisions/);
  f = fixture(); f.files['../outside'] = new Uint8Array(1);
  await assert.rejects(readFirmwareBundle(f.pack(), 'controller'), /Unsafe/);
  f = fixture(); f.files['controller/receiver-cache.bin'][4096] ^= 1;
  f.bundle.roles.controller.files.find(row => row.path.endsWith('/receiver-cache.bin')).sha256 = hash(f.files['controller/receiver-cache.bin']);
  await assert.rejects(readFirmwareBundle(f.pack(), 'controller'), /cache/);
});
