import test from 'node:test';
import assert from 'node:assert/strict';
import { SerialConnection, CONSOLE_BAUD_RATES } from '../serial.js';

// The firmware and PC have independent rates. Bytes at a mismatched rate are lost.
class BaudPort {
  constructor() { this.deviceRate = 115200; this.boot = 42; this.opens = []; this.commands = []; }
  async open({ baudRate }) {
    this.hostRate = baudRate; this.opens.push(baudRate);
    this.readable = new ReadableStream({ start: output => { this.output = output; } });
    this.writable = new WritableStream({ write: bytes => {
      const command = new TextDecoder().decode(bytes).trim(); this.commands.push(command);
      if (this.deviceRate !== this.hostRate) return;
      if (command === 'about') { this.emit('KMSwitcher ESP32-S3 master'); return; }
      const [, action, id, rate] = command.split(' ');
      if (action === 'get') this.emit(`KMSBAUD ${id} OK ${this.deviceRate} ${this.boot}`);
      if (action === 'set') {
        this.emit(`KMSBAUD ${id} SWITCH ${rate} ${this.boot}`);
        this.deviceRate = Number(rate);
      }
    } });
  }
  async setSignals() { throw new Error('Reset control lines must remain untouched'); }
  emit(line) { this.output.enqueue(new TextEncoder().encode(line + '\n')); }
  async close() {}
}

test('all offered speeds negotiate, reopen and confirm the same boot without disconnect callbacks', async () => {
  for (const rate of CONSOLE_BAUD_RATES) {
    let closed = 0;
    const port = new BaudPort(), connection = new SerialConnection({ onClose: () => closed++ });
    try {
      await connection.open(port); await connection.negotiate(rate);
      assert.equal(connection.baudRate, rate);
      assert.equal(port.deviceRate, rate);
      assert.equal(closed, 0);
      assert.deepEqual(port.opens, rate === 115200 ? [115200] : [115200, rate]);
      await connection.close();
      assert.equal(port.deviceRate, 115200);
      assert.equal(closed, 1);
    } finally { await connection.close(); }
  }
});

test('a reset during reopen fails negotiation instead of enabling a different boot', async () => {
  const port = new BaudPort(), open = port.open.bind(port);
  port.open = async options => { if (port.opens.length) port.boot++; await open(options); };
  const connection = new SerialConnection(); await connection.open(port);
  await assert.rejects(connection.negotiate(4000000), /restarted/);
  assert.equal(connection.connected, false);
  assert.equal(port.deviceRate, 115200);
});

test('a driver rejection closes the port and does not report a negotiated rate', async () => {
  const port = new BaudPort(), open = port.open.bind(port);
  port.open = async options => { if (options.baudRate !== 115200) throw new Error('Unsupported driver rate'); await open(options); };
  const connection = new SerialConnection(); await connection.open(port);
  await assert.rejects(connection.negotiate(4000000), /Unsupported driver rate/);
  assert.equal(connection.connected, false);
});

test('default speed remains compatible with firmware that only supports about', async () => {
  const port = new BaudPort(), connection = new SerialConnection();
  await connection.open(port); await connection.negotiate(115200);
  assert.deepEqual(port.commands, ['about']);
  await connection.close();
});

test('unsupported selections are rejected before issuing a speed switch', async () => {
  const port = new BaudPort(), connection = new SerialConnection(); await connection.open(port);
  for (const rate of [5000000, 6000000]) await assert.rejects(connection.negotiate(rate), /Unsupported/);
  assert.deepEqual(port.commands, []);
  await connection.close();
});

test('master update can return to base speed without resetting or closing the UI session', async () => {
  const port = new BaudPort(); let closed = 0;
  const connection = new SerialConnection({ onClose: () => closed++ });
  await connection.open(port); await connection.negotiate(4000000);
  await connection.returnToBase();
  assert.equal(connection.connected, true);
  assert.equal(connection.baudRate, 115200);
  assert.equal(closed, 0);
  assert.equal((await connection.baudRequest('get')).boot, 42);
  await connection.close();
});

test('an in-flight old-boot heartbeat failure cannot close a planned master reboot', async t => {
  t.mock.timers.enable({ apis: ['setInterval'] });
  const port = new BaudPort(), closed = [], connection = new SerialConnection({ onClose: error => closed.push(error) });
  await connection.open(port); await connection.negotiate(4000000);
  const request = connection.baudRequest.bind(connection);
  let failProbe;
  connection.baudRequest = () => new Promise((resolve, reject) => { failProbe = reject; });
  t.mock.timers.tick(2000);
  const prepared = connection.prepareForMasterReboot();
  failProbe(new Error('Old boot no longer answers'));
  await prepared;
  assert.equal(connection.connected, true); assert.deepEqual(closed, []);
  connection.baudRequest = request;
  connection.cancelMasterReboot(); t.mock.timers.tick(2000);
  await connection.keepAliveTask;
  assert.equal(connection.connected, true);
  await connection.close();
});
