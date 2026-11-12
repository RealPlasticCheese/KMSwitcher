import { hex, unhex, words, word, crc32 } from './protocol.js';

export function validatePreferences(p) {
  const integer = (value, min, max) => Number.isInteger(value) && value >= min && value <= max;
  if (!p || !integer(p.bootSlot, 0, 7) || !integer(p.trigger, 1, 7) || !integer(p.doubleMs, 100, 1000) ||
      !integer(p.selectMs, 500, 15000) || !integer(p.jigglerSeconds, 1, 65535) || !integer(p.jigglerDistance, 1, 127) || !integer(p.normalLed, 0, 1) ||
      !integer(p.brightness, 1, 255) || !integer(p.keyboardLeds, 0, 1) ||
      !Array.isArray(p.slotColors) || p.slotColors.length !== 7 || !p.slotColors.every(value => integer(value, 0, 7))) {
    throw new Error('Invalid preferences. Check timing bounds, brightness and colour selections.');
  }
  return p;
}
export function encodePreferences(p) {
  validatePreferences(p);
  const data = new Uint8Array(32);
  data.set([75, 77, 80, 49, 4, p.trigger, p.normalLed, p.brightness, p.keyboardLeds, p.bootSlot]);
  new DataView(data.buffer).setUint16(10, p.jigglerSeconds, true);
  data.set(words(p.doubleMs, p.selectMs), 12); data.set(p.slotColors, 20);
  data[27] = p.jigglerDistance;
  data.set(words(crc32(data.subarray(0, 28))), 28); return data;
}
export function decodePreferences(data) {
  if (data.length !== 32 || hex(data.subarray(0, 4)) !== '4b4d5031' || ![1, 2, 3, 4].includes(data[4]) ||
      (data[4] === 1 && data[9]) || (data[4] < 3 && (data[10] || data[11])) ||
      (data[4] < 4 && data[27]) || word(data, 28) !== crc32(data.subarray(0, 28))) {
    throw new Error('Invalid preferences response or checksum');
  }
  return validatePreferences({ bootSlot: data[4] >= 2 ? data[9] : 0, trigger: data[5], normalLed: data[6], brightness: data[7], keyboardLeds: data[8],
    jigglerSeconds: data[4] >= 3 ? data[10] | (data[11] << 8) : 30, jigglerDistance: data[4] >= 4 ? data[27] : 1,
    doubleMs: word(data, 12), selectMs: word(data, 16), slotColors: Array.from(data.subarray(20, 27)) });
}
export class PreferencesClient {
  constructor(connection, timeout = 6000) {
    this.connection = connection; this.timeout = timeout;
    this.sequence = crypto.getRandomValues(new Uint32Array(1))[0] || 1;
  }
  request(operation, revision, preferences) {
    this.sequence = ((this.sequence + 1) >>> 0) || 1;
    const id = this.sequence;
    const suffix = operation === 'get' ? '' : ` ${revision}${operation === 'set' ? ` ${hex(encodePreferences(preferences))}` : ''}`;
    return this.connection.exchange(`prefs-${operation} ${id}${suffix}`, line => {
      const match = line.match(/^KMSPREFS (\d+) (OK|CONFLICT|INVALID|STORAGE|BUSY) (\d+) ([\da-f]+)$/i);
      if (!match || Number(match[1]) !== id) return;
      const errors = { CONFLICT: 'Preferences changed on the device. Reload them before saving.',
        INVALID: 'The controller rejected the preference values.', STORAGE: 'Preferences could not be saved. Current settings are unchanged.',
        BUSY: 'Preferences cannot change during a master update.' };
      if (match[2] !== 'OK') throw new Error(errors[match[2]]);
      return { revision: Number(match[3]), values: decodePreferences(unhex(match[4])) };
    }, this.timeout);
  }
  get() { return this.request('get'); }
  save(values, revision) { return this.request('set', revision, values); }
  reset(revision) { return this.request('default', revision); }
}
