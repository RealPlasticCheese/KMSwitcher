// Explicitly opt-in simulated serial port. It never requests access to hardware.
import { OP, STATUS, CAP_RESTART, CAP_RADIO_DIAGNOSTICS, RESTART_KEY, encodeFrame, decodeFrame, hex, unhex, words, word, concat, crc32 } from './protocol.js';
import { encodePreferences, decodePreferences } from './preferences.js';
const defaults = () => ({ bootSlot: 0, jigglerSeconds: 30, jigglerDistance: 1, trigger: 1, doubleMs: 350, selectMs: 3000, normalLed: 0, brightness: 8, keyboardLeds: 1, slotColors: [1, 2, 3, 4, 5, 6, 7] });
const text = value => new TextEncoder().encode(value);
const digest = async image => new Uint8Array(await crypto.subtle.digest('SHA-256', image));
export async function demoFirmware(role = 'receiver') {
  const content = new Uint8Array(2048);
  content[0] = 0xe9; content[12] = role === 'controller' ? 9 : 2; content[23] = 1;
  content.set(text('KMSwitcher demonstration image'), 64);
  const image = concat(content, await digest(content));
  return { image, metadata: { schema: 1, role, chip: role === 'controller' ? 9 : 2, management_protocol: 1,
    input_protocol: 2, ota_schema: 1, git: role === 'controller' ? 'demo-master-update' : 'demo-update', utc: '2026-10-17T00:00:00Z',
    size: image.length, sha256: hex(await digest(image)) } };
}
export class DemoPort {
  constructor() {
    this.preferences = defaults(); this.prefsRevision = 10001;
    this.master = { boot: 1, sha256: '11'.repeat(32), git: 'demo-master' };
    this.selected = 1;
    this.radioChannel = 6;
    this.usbAutomatic = true;
    this.usbWanted = [true, true, true, true];
    this.savedSlot = 1;
    this.jigglers = new Map();
    this.receivers = new Map([1, 2, 3, 4].map(slot => [slot, { boot: 100 + slot,
      sha256: '11'.repeat(32), git: 'demo-a71c029', history: `KMSwitcher simulated receiver ${slot}\nBoot ${100 + slot}: USB mounted; encrypted radio ready\nNo input values are recorded.\n` }]));
  }
  async open({ baudRate = 115200 } = {}) {
    this.baudRate = baudRate;
    this.readable = new ReadableStream({ start: controller => { this.output = controller; }, cancel: () => { this.output = null; } });
    this.writable = new WritableStream({ write: async bytes => {
      await new Promise(resolve => setTimeout(resolve, 12));
      await this.command(new TextDecoder().decode(bytes).trim());
    } });
  }
  async setSignals() {}
  async close() { this.output = null; }
  line(value) { this.output?.enqueue(text(value + '\n')); }
  async command(command) {
    const [name, arg, wire] = command.split(' '), slot = Number(arg);
    if (name === 'baud') {
      const parts = command.split(' '), id = Number(parts[2]);
      this.line(`KMSBAUD ${id} ${parts[1] === 'set' && Number(parts[3]) !== this.baudRate ? 'SWITCH' : 'OK'} ${parts[1] === 'set' ? Number(parts[3]) : this.baudRate} ${this.master.boot}`);
    } else if (name === 'jiggler') {
      const parts = command.split(' '), id = Number(parts[2]), target = Number(parts[3]);
      const receiver = this.receivers.get(target);
      const value = this.jigglers.get(target) ?? { enabled: false, sequence: 0, mode: 0 };
      if (parts[1] === 'set' && receiver) {
        value.enabled = parts[4] === 'on'; value.sequence++; this.jigglers.set(target, value);
      } else if (parts[1] === 'mode' && receiver) {
        value.mode = ['left-right', 'four-way', 'random'].indexOf(parts[4]); value.sequence++; this.jigglers.set(target, value);
      }
      this.line(`KMSJIGGLER ${id} ${receiver ? 'OK' : 'INVALID'} ${target} ${+value.enabled} ${this.preferences.jigglerSeconds} 0 ${receiver?.boot ?? 0} ${value.sequence} ${this.preferences.jigglerDistance} ${value.mode} 1`);
    } else if (name === 'channel') {
      const parts = command.split(' '), id = Number(parts[2]);
      if (parts[1] === 'set') { this.radioChannel = Number(parts[3]); this.selected = 0; }
      this.line(`KMSCHANNEL ${id} OK ${this.radioChannel} ${this.radioChannel} 1 13 4 4`);
    } else if (name === 'usb-list') {
      const rows = [[2, 0, 1, 900, 900], [2, 1, 12, 15, 15], [3, 0, 1, 100, 100], [3, 1, 2, 600, 600]];
      this.line(`KMSUSB BEGIN ${+this.usbAutomatic} 3 1 ${this.usbWanted.filter(Boolean).length} 8`);
      this.line('KMSUSB DEVICE 1 2109 2817 00000000 0 1 1 506f77657265642055534220687562');
      this.line('KMSUSB DEVICE 2 1241 1203 00000000 1 1 0 4e756d65726963206b6579706164');
      this.line('KMSUSB DEVICE 3 046d c534 00000000 1 2 1 4c6f676974656368207265636569766572');
      this.line('KMSUSB INTERFACE 1 0 09 1 0 0 ignored 0 0 0');
      rows.forEach(([address, number, kinds, reports, decoded], index) => this.line(`KMSUSB INTERFACE ${address} ${number} 03 1 ${kinds} ${+this.usbWanted[index]} ${this.usbWanted[index] ? 'active' : 'off'} 63 ${reports} ${decoded}`));
      this.line('KMSUSB END');
    } else if (name === 'usb-input') {
      const parts = command.split(' '), number = Number(parts[2]);
      const index = slot === 2 && number < 2 ? number : slot === 3 && number < 2 ? 2 + number : -1;
      if (index >= 0) this.usbWanted.fill(parts[3] === 'on', slot === 2 ? 0 : 2, slot === 2 ? 2 : 4);
      this.line(`KMSUSB RESULT ${index >= 0 ? 'ok' : 'error'}`);
    } else if (name === 'usb-auto') {
      this.usbAutomatic = arg === 'on'; if (this.usbAutomatic) this.usbWanted.fill(true); this.line('KMSUSB RESULT ok');
    } else if (name === 'usb-rescan') this.line('KMSUSB RESULT ok');
    else if (name === 'remote' || name === 'master') {
      const local = name === 'master';
      const request = decodeFrame(unhex(local ? arg : wire)), receiver = local ? this.master : this.receivers.get(slot);
      const response = { ...request, flags: 1, sender: receiver?.boot ?? 0, target: 1, status: 0, data: new Uint8Array() };
      if (!receiver) response.status = STATUS.UNSUPPORTED;
      else if (request.target && request.target !== receiver.boot) response.status = STATUS.WRONG_BOOT;
      else if (request.op === OP.INFO) {
        const data = new Uint8Array(120);
        data.set(text(receiver.git)); data.set(text('2026-10-17T00:00:00Z'), 48);
        data.set(unhex(receiver.sha256), 72); data.set(words(2080, local ? 9 : 2, 7 | CAP_RESTART | CAP_RADIO_DIAGNOSTICS, 1), 104);
        response.data = data;
      } else if (request.op === OP.RADIO_STATUS) {
        response.data = words(1, receiver.boot, 10000, 1000, 1000, 0, 0, 0, 18, 3, 0,
          1100, 0, 200, 2, 1, 4, 0, 1, 31000);
      } else if (request.op === OP.RESTART) {
        if (local || !request.target || request.flags || request.offset || request.data.length !== 4 || word(request.data) !== RESTART_KEY) response.status = 1;
        else { receiver.boot++; this.jigglers.delete(slot); }
      } else if (request.op === OP.STATS) {
        const data = Array(42).fill(0);
        data[0] = receiver.boot; data[2] = 8640000 + slot * 340000;
        data[4] = 234090; data[5] = 42001; data[8] = 90211; data[9] = 103000;
        data[13] = 104320; data[14] = 1; data[15] = 1; data[16] = 1;
        data[31] = -39 - slot * 3; data[32] = -41; data[34] = receiver.history ? 12 : 0;
        data[35] = 32; data[37] = 3; data[38] = 8;
        response.data = words(...data);
      } else if (request.op === OP.HISTORY_BEGIN) {
        receiver.snapshot = text(receiver.history || 'History is empty.\n');
        receiver.snapshotToken = request.id;
        response.data = words(request.id, receiver.snapshot.length, crc32(receiver.snapshot));
      } else if (request.op === OP.HISTORY_READ) {
        if (word(request.data) !== receiver.snapshotToken) response.status = 9;
        else if (request.offset === receiver.snapshot.length) receiver.snapshot = null;
        else response.data = receiver.snapshot.slice(request.offset, request.offset + 192);
      } else if (request.op === OP.CLEAR) receiver.history = '';
      else if (request.op === OP.BEGIN) {
        receiver.upload = []; receiver.uploadSize = 0; receiver.uploadToken = word(request.data);
        receiver.uploadDigest = hex(request.data.subarray(8, 40));
        response.data = words(0);
      } else if (request.op === OP.WRITE) {
        if (word(request.data) !== receiver.uploadToken || request.offset !== receiver.uploadSize) response.status = 4;
        else {
          receiver.upload.push(request.data.slice(4)); receiver.uploadSize += request.data.length - 4;
          response.data = words(receiver.uploadSize);
        }
      } else if (request.op === OP.END) {
        if (hex(await digest(concat(...receiver.upload))) !== receiver.uploadDigest) response.status = STATUS.INTEGRITY;
      } else if (request.op === OP.ACTIVATE) {
        receiver.sha256 = receiver.uploadDigest; receiver.git = local ? 'demo-master-update' : 'demo-update'; receiver.boot++;
        if (local) { this.selected = 0; this.savedSlot = 0; this.prefsRevision++; }
        else this.jigglers.delete(slot);
      } else if (request.op === OP.ABORT) receiver.upload = [];
      this.line(local ? `KMSMASTER ${hex(encodeFrame(response))}` : `KMSM ${slot} ${hex(encodeFrame(response))}`);
    } else if (name.startsWith('prefs-')) {
      const parts = command.split(' '), id = Number(parts[1]);
      let status = 'OK';
      if (name !== 'prefs-get') {
        if (Number(parts[2]) !== this.prefsRevision) status = 'CONFLICT';
        else {
          try { this.preferences = name === 'prefs-default' ? defaults() : decodePreferences(unhex(parts[3])); this.prefsRevision++; }
          catch { status = 'INVALID'; }
        }
      }
      this.line(`KMSPREFS ${id} ${status} ${this.prefsRevision} ${hex(encodePreferences(this.preferences))}`);
    } else if (name === 'slaves') {
      for (const [id, receiver] of this.receivers) this.line(`KMSPEER ${id} ${receiver.boot.toString(16).padStart(8, '0')}`);
      this.line(`KMSROUTE CURRENT ${this.selected}`); this.line('KMSPEERS END');
    } else if (name === 'unpair') {
      if (!this.receivers.delete(slot)) this.line(`KMSUNPAIR ${slot} NOT_PAIRED`);
      else {
        this.jigglers.delete(slot);
        if (this.selected === slot) this.selected = 0;
        if (this.savedSlot === slot) this.savedSlot = 0;
        this.line(`KMSUNPAIR ${slot} OK`);
      }
    } else if (name === 'pause') {
      if (this.selected) this.savedSlot = this.selected;
      this.selected = 0; this.line('KMSROUTE PAUSED');
    } else if (name === 'resume') {
      this.selected ||= this.savedSlot; this.line(`KMSROUTE RESUMED ${this.selected}`);
    } else if (name === 'select' && this.receivers.has(slot)) {
      this.selected = slot; this.line(`KMSROUTE SELECTED ${slot}`);
    } else if (name === 'pair') this.line(`I (12400) pairing: Enrollment open for slot ${slot}; hold receiver button for 3 seconds`);
    else if (name === 'about') this.line('I (12400) console: KMSwitcher ESP32-S3 · DEMO firmware · input v2 / management v1');
    else if (name === 'status') this.line(`I (12400) status: selected=${this.selected} paired=${this.receivers.size} route=${this.selected ? 'active' : 'paused'} buffer=0 retries=0`);
    else if (name === 'history') this.line('I (12400) history: DEMO boot=1 event=ready; no faults; checkpoints=3');
    else if (name === 'stats-save') this.line('I (12400) history: Statistics checkpoint queued (demo)');
    else if (name === 'help') this.line('help | about | status | history | stats-save | slaves | slave-status N | slave-history N | slave-history-clear N | pause | resume | pair N | unpair N | select N');
    else this.line('I (12400) console: Command not recognized in demo. Type help.');
  }
}
