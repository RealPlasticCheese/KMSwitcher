import { unzipSync } from 'fflate';
import { validateFirmware } from './management.js';

const LIMIT = 128 * 1024 * 1024;
const decoder = new TextDecoder('utf-8', { fatal: true });
const digest = async bytes => Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)),
  byte => byte.toString(16).padStart(2, '0')).join('');

// Read the same bundle.json ZIP used by Firmware Setup. No archive paths are
// written to disk, and expansion limits are checked before decompression.
export async function readFirmwareBundle(bytes, role) {
  if (!['controller', 'receiver'].includes(role) || bytes.length > LIMIT) throw new Error('Invalid firmware archive or role');
  let total = 0, count = 0;
  const names = new Set();
  const files = unzipSync(bytes, { filter: entry => {
    total += entry.originalSize; count++;
    const name = entry.name;
    if (count > 256 || total > LIMIT || !Number.isSafeInteger(total) || name.startsWith('/') ||
        name.includes('\\') || name.includes(':') || name.split('/').includes('..') || names.has(name.toLowerCase())) {
      throw new Error('Unsafe or oversized firmware archive');
    }
    names.add(name.toLowerCase()); return true;
  } });
  const manifests = Object.keys(files).filter(name => name === 'bundle.json' || name.endsWith('/bundle.json'));
  if (manifests.length !== 1) throw new Error('Choose a ZIP containing exactly one firmware bundle');
  const manifest = manifests[0], prefix = manifest.slice(0, -'bundle.json'.length);
  const bundle = JSON.parse(decoder.decode(files[manifest]));
  if (bundle.schema !== 1 || !bundle.roles?.controller || !bundle.roles?.receiver ||
      !['regular', undefined].includes(bundle.product)) throw new Error('Choose a regular controller/receiver firmware bundle');
  const images = {};
  let cache;
  for (const [name, chip, capacity] of [['controller', 'esp32s3', 0x1000000], ['receiver', 'esp32s2', 0x400000]]) {
    const description = bundle.roles[name];
    if (description.chip !== chip || description.capacity !== capacity || !Array.isArray(description.files)) throw new Error('Firmware board mismatch');
    let application;
    const offsets = new Set();
    const ranges = [];
    for (const entry of description.files) {
      if (typeof entry.path !== 'string' || entry.path.includes('\\') || entry.path.includes(':') ||
          entry.path.startsWith('/') || entry.path.split('/').includes('..')) throw new Error('Unsafe firmware image path');
      const data = files[prefix + entry.path];
      if (!data || data.length !== entry.size || await digest(data) !== entry.sha256 ||
          !Number.isSafeInteger(entry.offset) || entry.offset < 0 || entry.offset % 4096 ||
          entry.offset + data.length > capacity || offsets.has(entry.offset)) throw new Error('Firmware file or flash range does not match its manifest');
      offsets.add(entry.offset);
      if (!data.length || (entry.offset < 0x10000 && Math.ceil((entry.offset + data.length) / 4096) * 4096 > 0x9000) ||
          ranges.some(([start, end]) => entry.offset < end && start < entry.offset + data.length)) throw new Error('Firmware overlaps protected storage or another image');
      ranges.push([entry.offset, entry.offset + data.length]);
      if (name === 'controller' && entry.offset === 0x960000) cache = data;
      if (name === 'controller' && entry.offset === 0xb60000 &&
          (data.length !== 4096 || data.some(byte => byte !== 255))) throw new Error('Second slave cache must be empty');
      if (entry.path === `${name}/kmswitcher_${name}.bin`) application = data;
    }
    const metadataBytes = files[`${prefix}${name}/kmswitcher_${name}.json`];
    const expectedOffsets = name === 'controller' ? [0, 0x8000, 0x10000, 0x310000, 0x960000, 0xb60000] : [0x1000, 0x8000, 0x10000, 0x20000];
    if (offsets.size !== expectedOffsets.length || expectedOffsets.some(offset => !offsets.has(offset))) throw new Error('Unsupported firmware flash layout');
    if (!application || !metadataBytes) throw new Error('Firmware application or OTA metadata is missing');
    const metadata = JSON.parse(decoder.decode(metadataBytes));
    if (metadata.git !== bundle.commit || metadata.role !== name) throw new Error('Mixed firmware source revisions');
    images[name] = await validateFirmware(application, metadata);
  }
  const embedded = images.controller.receiver;
  if (!embedded || embedded.metadata.sha256 !== images.receiver.metadata.sha256) throw new Error('Master and slave firmware do not form a matched pair');
  const receiver = images.receiver;
  if (!cache || cache.length < 4096 + receiver.image.length || cache.length > 0x200000 || cache.length % 4096) throw new Error('Master slave-cache image is missing or invalid');
  const view = new DataView(cache.buffer, cache.byteOffset, cache.byteLength);
  let crc = 0xffffffff;
  for (const byte of cache.subarray(0, 136)) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  const identity = decoder.decode(cache.subarray(16, 64)).split('\0')[0];
  const storedHash = Array.from(cache.subarray(88, 120), byte => byte.toString(16).padStart(2, '0')).join('');
  if (decoder.decode(cache.subarray(0, 4)) !== 'KMSC' || view.getUint32(4, true) !== 1 ||
      view.getUint32(12, true) !== 4096 || decoder.decode(cache.subarray(140, 144)) !== 'DONE' ||
      view.getUint32(136, true) !== ((crc ^ 0xffffffff) >>> 0) || identity !== receiver.metadata.git ||
      storedHash !== receiver.metadata.sha256 || view.getUint32(120, true) !== receiver.image.length ||
      view.getUint32(124, true) !== 2 || view.getUint32(128, true) !== 3 || view.getUint32(132, true) !== 1 ||
      await digest(cache.subarray(4096, 4096 + receiver.image.length)) !== receiver.metadata.sha256 ||
      cache.subarray(144, 4096).some(byte => byte !== 255) ||
      cache.subarray(4096 + receiver.image.length).some(byte => byte !== 255)) throw new Error('Master cache does not contain its matched slave firmware');
  return images[role];
}
