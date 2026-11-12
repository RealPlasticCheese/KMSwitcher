const states = new Set(['probing', 'active', 'off', 'channel-limit', 'unsupported', 'error', 'removing', 'ignored', 'standby']);
const uint = (value, max) => /^\d+$/.test(value) && Number(value) <= max;
export function usbInventory(connection, timeout = 6000) {
  let snapshot;
  return connection.exchange('usb-list', line => {
    if (!line.startsWith('KMSUSB ')) return;
    const parts = line.trim().split(/\s+/), kind = parts[1];
    if (kind === 'BEGIN') {
      if (parts.length !== 7 || !parts.slice(2).every(value => uint(value, 255)) || Number(parts[2]) > 1 || Number(parts[6]) !== 8)
        throw new Error('Invalid USB inventory header');
      snapshot = { automatic: parts[2] === '1', controls: Number(parts[3]), hubs: Number(parts[4]),
        inputs: Number(parts[5]), limit: 8, devices: [], interfaces: [] };
    } else if (kind === 'DEVICE' && snapshot) {
      const [, , address, vid, pid, serial, parent, port, speed, name] = parts;
      if (parts.length !== 10 || ![address, parent, port, speed].every(value => uint(value, 127)) ||
          !/^[\da-f]{4}$/i.test(vid) || !/^[\da-f]{4}$/i.test(pid) || !/^[\da-f]{8}$/i.test(serial) ||
          !(name === '-' || /^(?:[\da-f]{2}){1,47}$/i.test(name)) || snapshot.devices.length >= 8 ||
          snapshot.devices.some(device => device.address === Number(address))) throw new Error('Invalid USB device');
      const label = name === '-' ? '' : String.fromCharCode(...name.match(/../g).map(byte => parseInt(byte, 16)));
      snapshot.devices.push({ address: Number(address), vid, pid, serial, parent: Number(parent), port: Number(port), speed: Number(speed), name: label });
    } else if (kind === 'INTERFACE' && snapshot) {
      const [, , address, number, deviceClass, endpoints, kinds, wanted, state, bytes, reports, decoded] = parts;
      if (parts.length !== 12 || !uint(address, 127) || !uint(number, 255) || !/^[\da-f]{2}$/i.test(deviceClass) ||
          !uint(endpoints, 255) || !uint(kinds, 31) || !uint(wanted, 1) || !states.has(state) || !uint(bytes, 65535) ||
          !uint(reports, 0xffffffff) || !uint(decoded, 0xffffffff) || snapshot.interfaces.length >= 24 ||
          snapshot.interfaces.some(row => row.address === Number(address) && row.number === Number(number))) throw new Error('Invalid USB interface');
      snapshot.interfaces.push({ address: Number(address), number: Number(number), deviceClass: parseInt(deviceClass, 16),
        endpoints: Number(endpoints), kinds: Number(kinds), wanted: wanted === '1', state,
        descriptorBytes: Number(bytes), reports: Number(reports), decoded: Number(decoded) });
    } else if (kind === 'END' && snapshot) {
      if (snapshot.interfaces.some(row => !snapshot.devices.some(device => device.address === row.address)))
        throw new Error('USB inventory changed; refresh and try again');
      snapshot.used = snapshot.controls + snapshot.hubs + snapshot.inputs;
      return snapshot;
    }
  }, timeout);
}
export function usbCommand(connection, command) {
  if (!/^usb-(?:rescan|auto (?:on|off)|input \d{1,3} \d{1,3} (?:on|off))$/.test(command))
    throw new Error('Invalid USB selection command');
  return connection.exchange(command, line => {
    if (line === 'KMSUSB RESULT ok') return true;
    if (line === 'KMSUSB RESULT error') throw new Error('USB choice rejected. Refresh the list; the device may have disconnected or the saved-choice limit may be full.');
  });
}
export function usbKinds(bits) {
  const labels = [[1, 'Keyboard'], [2, 'Mouse'], [4, 'Media'], [8, 'System'], [16, 'Lock LEDs']];
  return labels.filter(([bit]) => bits & bit).map(([, label]) => label).join(' + ') || 'Identifying HID';
}
export class UsbInputs {
  constructor(connection, run, message) {
    this.connection = connection; this.run = run; this.message = message;
    this.$ = id => document.getElementById(id);
    this.$('usb-refresh').addEventListener('click', () => run('Reading USB inputs…', () => this.refresh()));
    this.$('usb-retry').addEventListener('click', () => run('Retrying USB descriptors…', async () => {
      await usbCommand(connection, 'usb-rescan'); await this.refresh();
      message('Descriptor retry requested. Unplug and reconnect devices missing from the list.');
    }));
    this.$('usb-mode').addEventListener('change', () => run('Changing USB input selection…', async () => {
      try { await usbCommand(connection, `usb-auto ${this.$('usb-mode').value}`); }
      finally { await this.refresh(); }
      message('USB selection mode applied.', 'success');
    }));
    this.reset();
  }
  reset() {
    this.snapshot = null;
    this.$('usb-budget').textContent = 'Channel usage appears after refresh';
    this.$('usb-note').textContent = 'Connect the master, then refresh. This requires USB-selection firmware.';
    this.$('usb-mode').value = 'on';
    this.$('usb-rows').replaceChildren();
  }
  async refresh() {
    try { this.snapshot = await usbInventory(this.connection); }
    catch (error) {
      this.reset(); this.$('usb-note').textContent = 'USB inventory unavailable. Check the connection and master firmware, then refresh.';
      throw error;
    }
    this.render();
  }
  render() {
    const s = this.snapshot;
    this.$('usb-mode').value = s.automatic ? 'on' : 'off';
    this.$('usb-budget').textContent = `${s.used}/${s.limit} channels · ${s.controls} device controls + ${s.hubs} hub status + ${s.inputs} selected endpoints · up to 2 input devices`;
    this.$('usb-note').textContent = 'Up to two physical input devices are active. Each device’s supported input interfaces stay together so an unused keyboard cannot stall its mouse. Allow selects the whole device; standby means its complete inputs do not fit or another device has priority. Hubs and device controls still use channels. Reconnect missing devices after freeing capacity. Choices follow device IDs and serial numbers; identical devices without serial numbers share choices.';
    const rows = [];
    for (const i of s.interfaces.filter(row => row.deviceClass === 3)) {
      const d = s.devices.find(device => device.address === i.address);
      const row = document.createElement('tr');
      const cell = text => { const td = document.createElement('td'); td.textContent = text; row.append(td); return td; };
      cell(d.name || `${d.vid}:${d.pid}`);
      cell(`${d.vid}:${d.pid} · ${i.address}/${i.number}`);
      cell(usbKinds(i.kinds)); cell(String(i.endpoints));
      const status = cell(`${i.state} · ${i.decoded}/${i.reports} reports decoded`);
      status.className = ['error', 'channel-limit'].includes(i.state) ? 'error' : '';
      const control = cell('');
      const first = s.interfaces.find(other => other.address === i.address && other.deviceClass === 3 && other.state !== 'unsupported');
      if (first !== i) { control.textContent = first ? 'Same device' : 'Unavailable'; rows.push(row); continue; }
      const label = document.createElement('label'), checkbox = document.createElement('input');
      checkbox.type = 'checkbox'; checkbox.checked = i.wanted; checkbox.dataset.device = '';
      checkbox.disabled = !this.connection.connected || ['unsupported', 'removing'].includes(i.state);
      checkbox.setAttribute('aria-label', `Use ${d.name || d.vid + ':' + d.pid}`);
      label.className = 'checkbox'; label.append(checkbox, ' Allow device'); control.append(label);
      checkbox.addEventListener('change', () => this.run('Changing USB input…', async () => {
        try { await usbCommand(this.connection, `usb-input ${i.address} ${i.number} ${checkbox.checked ? 'on' : 'off'}`); }
        finally { await this.refresh(); }
        this.message('USB input choice applied. Refresh to check activation and decoded reports.', 'success');
      }));
      if (['unsupported', 'removing'].includes(i.state)) checkbox.dataset.usbBlocked = '';
      rows.push(row);
    }
    if (!rows.length) {
      const row = document.createElement('tr'), cell = document.createElement('td');
      cell.colSpan = 6; cell.className = 'empty'; cell.textContent = 'No enumerated HID interfaces. Check power and the USB cable, then reconnect the input device.';
      row.append(cell); rows.push(row);
    }
    this.$('usb-rows').replaceChildren(...rows);
  }
}
