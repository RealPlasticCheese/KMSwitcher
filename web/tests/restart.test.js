import test from 'node:test';
import assert from 'node:assert/strict';
import { Controller, restartTargets } from '../management.js';
import { OP, STATUS, CAP_RESTART, RESTART_KEY, ManagementError, decodeFrame, encodeFrame, hex, unhex, words, word } from '../protocol.js';

function fixture({ masterCap = CAP_RESTART, receiverCap = CAP_RESTART, lostAck = false,
  status = STATUS.OK, returns = true, confirmed = true, changedImage = false } = {}) {
  const requests = [];
  const connection = { connected: true, async exchange(command, accept) {
    const parts = command.split(' '), local = parts[0] === 'master';
    const q = decodeFrame(unhex(parts[local ? 1 : 2])); requests.push(q);
    const restarted = requests.some(row => row.op === OP.RESTART);
    let sender = local ? 11 : restarted && returns ? 23 : 22, data = new Uint8Array(), result = STATUS.OK;
    if (q.op === OP.INFO) {
      data = new Uint8Array(120);
      data.set(new TextEncoder().encode('revision')); data.set(new TextEncoder().encode('2026-10-29T00:00:00Z'), 48);
      data.fill(changedImage && restarted && !local ? 2 : 1, 72, 104);
      data.set(words(256, local ? 9 : 2, (local ? masterCap : receiverCap) | (local || !restarted || confirmed ? 4 : 0), 1), 104);
    } else if (q.op === OP.RESTART) {
      assert.equal(q.target, 22); assert.equal(word(q.data), RESTART_KEY);
      if (lostAck) throw new Error('Lost reply');
      sender = 22; result = status;
    }
    const response = hex(encodeFrame({ ...q, sender, flags: 1, status: result, data }));
    return accept(local ? `KMSMASTER ${response}` : `KMSM ${q.slot} ${response}`);
  } };
  let now = 0;
  return { controller: new Controller(connection), requests,
    options: { timeout: 500, sleep: async ms => { now += ms; }, now: () => now } };
}

test('restart and lost ACK both verify a new boot with a single restart command', async () => {
  for (const lostAck of [false, true]) {
    const { controller, requests, options } = fixture({ lostAck });
    assert.deepEqual(await controller.restart(2, options), { slot: 2, result: 'restarted', before: 22, after: 23 });
    assert.equal(requests.filter(row => row.op === OP.RESTART).length, 1);
  }
});
test('old S3 or S2 firmware reports unsupported before sending a restart', async () => {
  for (const caps of [{ masterCap: 0 }, { receiverCap: 0 }]) {
    const { controller, requests, options } = fixture(caps);
    await assert.rejects(controller.restart(2, options), error => error.code === STATUS.UNSUPPORTED);
    assert.ok(!requests.some(row => row.op === OP.RESTART));
  }
});
test('busy reply is definitive; same boot and unconfirmed boot never count as success', async () => {
  for (const changes of [{ status: STATUS.BUSY }, { returns: false }, { confirmed: false }]) {
    const { controller, requests, options } = fixture(changes);
    await assert.rejects(controller.restart(2, options), error => error.code === (changes.status ?? STATUS.TIMEOUT));
    assert.equal(requests.filter(row => row.op === OP.RESTART).length, 1);
  }
});
test('different firmware after restart is reported as a failure', async () => {
  const { controller, options } = fixture({ changedImage: true });
  await assert.rejects(controller.restart(2, options), /different firmware/);
});
test('batch restarts selected receiver last, deduplicates and continues after offline receivers', async () => {
  const order = [], routes = [];
  const controller = { peers: async () => ({ slots: [1, 2, 3], selected: 2 }), route: async paused => routes.push(paused),
    async restart(slot, options) {
      order.push(slot); await options.beforeRestart();
      if (slot === 3) throw new ManagementError(STATUS.TIMEOUT);
      return { slot, result: 'restarted' };
    } };
  const report = await restartTargets(controller, [2, 1, 3, 1]);
  assert.deepEqual(order, [1, 3, 2]); assert.deepEqual(routes, [true, false]);
  assert.deepEqual(report.results.map(row => row.result), ['restarted', 'unreachable', 'restarted']);
  assert.equal(report.success, false);
});
test('manual route changes and an already paused route are preserved', async () => {
  for (const current of [0, 3]) {
    let calls = 0;
    const controller = { peers: async () => ({ slots: [1, 2, 3], selected: ++calls === 1 ? 2 : current }),
      route: async () => assert.fail('Route should be preserved'),
      async restart(slot, options) { await options.beforeRestart(); return { slot, result: 'restarted' }; } };
    assert.equal((await restartTargets(controller, [2])).success, true);
  }
});
test('failed pause still resumes; restoration failures and invalid selections are reported', async () => {
  const routes = [];
  const controller = { peers: async () => ({ slots: [2], selected: 2 }),
    route: async paused => { routes.push(paused); throw new Error('Lost route reply'); },
    restart: async (slot, options) => options.beforeRestart() };
  const report = await restartTargets(controller, [2]);
  assert.deepEqual(routes, [true, false]); assert.match(report.warnings[0], /restore input/); assert.equal(report.success, false);
  for (const targets of [[], [0], [3]]) await assert.rejects(restartTargets(controller, targets), /paired receiver/);
});
