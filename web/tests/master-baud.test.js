import test from 'node:test';
import assert from 'node:assert/strict';
import { SerialConnection } from '../serial.js';
import { Controller, updateMaster, updateTargets } from '../management.js';
import { DemoPort, demoFirmware } from '../demo.js';
import { OP, STATUS, decodeFrame, unhex, encodeFrame, hex } from '../protocol.js';

// Host and firmware rates are independent. A master reboot resets only the
// firmware rate, so confirmation cannot succeed until the client reopens at base.
class UpdatePort extends DemoPort {
  constructor(options = {}) {
    super(); Object.assign(this, options);
    this.deviceRate = 115200; this.calls = []; this.opens = [];
  }
  async open(options) { this.opens.push(options.baudRate); await super.open(options); }
  async setSignals() { throw new Error('Must not manipulate reset signals'); }
  async command(command) {
    this.calls.push({ command, rate: this.baudRate });
    if (this.baudRate !== this.deviceRate) return;
    const parts = command.split(' ');
    if (parts[0] === 'baud') {
      const rate = parts[1] === 'set' ? Number(parts[3]) : this.deviceRate;
      if (this.refuseRestoration && this.master.boot > 1 && rate > 115200) {
        this.line(`KMSBAUD ${parts[2]} INVALID ${this.deviceRate} ${this.master.boot}`); return;
      }
      this.line(`KMSBAUD ${parts[2]} ${rate !== this.deviceRate ? 'SWITCH' : 'OK'} ${rate} ${this.master.boot}`);
      this.deviceRate = rate; return;
    }
    if (parts[0] === 'master') {
      const request = decodeFrame(unhex(parts[1]));
      const status = request.op === OP.ACTIVATE && this.rejectActivation ? STATUS.INTEGRITY :
        request.op === OP.END && this.rejectEnd ? STATUS.INTEGRITY : null;
      if (status !== null) {
        this.line(`KMSMASTER ${hex(encodeFrame({ ...request, flags: 1, sender: this.master.boot, target: 1, status, data: new Uint8Array() }))}`);
        return;
      }
      if (request.op === OP.ACTIVATE) {
        this.onActivate?.();
        await super.command(command);
        this.deviceRate = 115200;
        return;
      }
    }
    await super.command(command);
  }
  line(value) {
    if (this.dropActivationReply && value.startsWith('KMSMASTER ') && decodeFrame(unhex(value.split(' ')[1])).op === OP.ACTIVATE) return;
    super.line(value);
  }
  frames() { return this.calls.filter(call => call.command.startsWith('master ')).map(call => ({ ...call, frame: decodeFrame(unhex(call.command.split(' ')[1])) })); }
}
async function setup(t, options, baudRate = 4000000) {
  const port = new UpdatePort(options), rates = [], closed = [];
  const connection = new SerialConnection({ onBaudRateChange: rate => rates.push(rate), onClose: error => closed.push(error) });
  await connection.open(port); await connection.negotiate(baudRate);
  const controller = new Controller(connection, { timeout: 100, attempts: 2 });
  await controller.select(3);
  t.after(() => connection.close());
  return { port, connection, controller, rates, closed };
}

test('master upload and verification stay at 4 Mbaud; reboot confirms at base and restores selected speed', async t => {
  const { port, connection, controller, closed } = await setup(t);
  const heartbeat = connection.keepAliveGeneration;
  let activationGeneration;
  port.onActivate = () => { activationGeneration = connection.keepAliveGeneration; };
  const firmware = await demoFirmware('controller');
  const report = await updateMaster(controller, firmware, { progress: (done, total, stage) => {
    if (['Uploading', 'Verifying image'].includes(stage)) {
      assert.equal(connection.baudRate, 4000000);
      assert.equal(connection.keepAliveGeneration, heartbeat, 'Lease remains active during upload and verification');
    }
  } });
  assert.equal(report.result, 'updated'); assert.deepEqual(report.warnings, []);
  const frames = port.frames();
  for (const op of [OP.BEGIN, OP.WRITE, OP.END, OP.ACTIVATE]) {
    const calls = frames.filter(call => call.frame.op === op); assert.ok(calls.length);
    assert.ok(calls.every(call => call.rate === 4000000));
  }
  const activation = frames.findIndex(call => call.frame.op === OP.ACTIVATE);
  assert.ok(frames.slice(activation + 1).some(call => call.frame.op === OP.INFO && call.rate === 115200));
  assert.ok(activationGeneration > heartbeat, 'Old-boot heartbeat is suspended for activation');
  assert.deepEqual(port.opens, [115200, 4000000, 115200, 4000000]);
  assert.ok(!port.calls.some(call => /^baud set \d+ 115200$/.test(call.command)), 'No downgrade command is sent to the uploading master');
  assert.equal(connection.baudRate, 4000000); assert.equal(port.selected, 3); assert.deepEqual(closed, []);
  const opens = [...port.opens];
  assert.equal((await updateMaster(controller, firmware)).result, 'skipped');
  assert.deepEqual(port.opens, opens, 'Matching firmware does not reopen or downgrade the connection');
});

test('a lost master activation ACK is not retried at the old speed and still confirms the reboot', async t => {
  const { port, controller } = await setup(t, { dropActivationReply: true });
  const report = await updateMaster(controller, await demoFirmware('controller'));
  assert.equal(report.result, 'updated'); assert.deepEqual(report.warnings, []);
  assert.equal(port.frames().filter(call => call.frame.op === OP.ACTIVATE).length, 1);
  assert.deepEqual(port.opens, [115200, 4000000, 115200, 4000000]);
  assert.equal(port.selected, 3);
});

test('definitive activation rejection aborts and resumes the original high-speed heartbeat', async t => {
  const { port, connection, controller } = await setup(t, { rejectActivation: true });
  const generation = connection.keepAliveGeneration;
  await assert.rejects(updateMaster(controller, await demoFirmware('controller')), /integrity/);
  assert.ok(port.frames().some(call => call.frame.op === OP.ABORT && call.rate === 4000000));
  assert.deepEqual(port.opens, [115200, 4000000]);
  assert.equal(connection.baudRate, 4000000); assert.equal(connection.connected, true);
  assert.ok(connection.keepAliveGeneration > generation);
  assert.equal(port.selected, 3);
});

test('image verification failure never hands the connection to the reboot rate', async t => {
  const { port, connection, controller } = await setup(t, { rejectEnd: true });
  const generation = connection.keepAliveGeneration;
  await assert.rejects(updateMaster(controller, await demoFirmware('controller')), /integrity/);
  assert.deepEqual(port.opens, [115200, 4000000]);
  assert.equal(connection.keepAliveGeneration, generation);
  assert.ok(!port.frames().some(call => call.frame.op === OP.ACTIVATE));
  assert.equal(port.selected, 3);
});

test('base-rate master updates need no serial reopen', async t => {
  const { port, controller, connection } = await setup(t, {}, 115200);
  assert.equal((await updateMaster(controller, await demoFirmware('controller'))).result, 'updated');
  assert.deepEqual(port.opens, [115200]); assert.equal(connection.baudRate, 115200);
});

test('receiver updates keep the master connection and heartbeat at the selected high speed', async t => {
  const { port, controller, connection } = await setup(t);
  const generation = connection.keepAliveGeneration;
  const report = await updateTargets(controller, [1], 3, await demoFirmware());
  assert.equal(report.results[0].result, 'updated');
  assert.deepEqual(port.opens, [115200, 4000000]);
  assert.equal(connection.keepAliveGeneration, generation);
  assert.ok(port.calls.filter(call => call.command.startsWith('remote ')).every(call => call.rate === 4000000));
});

test('confirmed master update remains successful if restoring the selected console speed fails', async t => {
  const { controller } = await setup(t, { refuseRestoration: true });
  let saved;
  const report = await updateMaster(controller, await demoFirmware('controller'), { onComplete: result => { saved = result; } });
  assert.equal(report.result, 'updated'); assert.equal(saved.result, 'updated');
  assert.ok(report.warnings.some(warning => /could not restore 4000000 baud/.test(warning)));
});
