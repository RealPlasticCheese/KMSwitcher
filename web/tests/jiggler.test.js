import test from 'node:test';
import assert from 'node:assert/strict';
import { JigglerClient } from '../jiggler.js';

function connection(replies) {
  const commands = [];
  return { commands, async exchange(command, accept) {
    commands.push(command);
    const [, , id, slot] = command.split(' '), reply = replies.shift();
    assert.equal(accept(`KMSJIGGLER ${Number(id) + 1} OK ${slot} 0 30 0 100 0 1 0 1`), undefined);
    assert.equal(accept(`KMSJIGGLER ${id} OK ${Number(slot) + 1} 0 30 0 100 0 1 0 1`), undefined);
    return accept(`KMSJIGGLER ${id} ${reply.status ?? 'OK'} ${slot} ${reply.enabled ?? 0} 30 ${reply.pending ?? 0} ${reply.boot ?? 100} ${reply.sequence ?? 1} ${reply.distance ?? 1} ${reply.mode ?? 0} ${reply.modesSupported ?? 1}`);
  } };
}
test('jiggler sets an inactive receiver explicitly and waits for its confirmation', async () => {
  const serial = connection([{ enabled: 1, pending: 1 }, { enabled: 1, pending: 1 }, { enabled: 1 }]);
  const client = new JigglerClient(serial, { pollMs: 1 }), updates = [];
  const value = await client.set(3, true, state => updates.push(state.pending));
  assert.deepEqual(updates, [true, true, false]);
  assert.equal(value.enabled, true);
  assert.match(serial.commands[0], /^jiggler set \d+ 3 on$/);
  assert.ok(serial.commands.slice(1).every(command => /^jiggler get \d+ 3$/.test(command)));
  await new JigglerClient(connection([{}])).set(2, false);
});
test('mode changes preserve disabled state and wait for exact mode confirmation', async () => {
  for (const [mode, name] of [[0, 'left-right'], [1, 'four-way'], [2, 'random']]) {
    const serial = connection([{ mode, pending: 1 }, { mode }]);
    const client = new JigglerClient(serial, { pollMs: 1 });
    const result = await client.setMode(2, mode);
    assert.equal(result.mode, mode); assert.equal(result.enabled, false); assert.equal(result.pending, false);
    assert.match(serial.commands[0], new RegExp(`^jiggler mode \\d+ 2 ${name}$`));
  }
  await assert.rejects(new JigglerClient(connection([])).setMode(1, 3), /valid jiggle mode/);
  await assert.rejects(new JigglerClient(connection([{ mode: 1, pending: 1 }, { mode: 2 }]), { pollMs: 1 }).setMode(1, 1), /another control/);
  const legacy = await new JigglerClient(connection([{ modesSupported: 0 }])).get(1);
  assert.equal(legacy.modesSupported, false); assert.equal(legacy.mode, 0);
});
test('restart or a newer shortcut request cannot be mistaken for confirmation', async () => {
  await assert.rejects(new JigglerClient(connection([{ enabled: 1, pending: 1 }, { boot: 101 }]), { pollMs: 1 }).set(1, true), /restarted/);
  await assert.rejects(new JigglerClient(connection([{ enabled: 1, pending: 1 }, { sequence: 2 }]), { pollMs: 1 }).set(1, true), /another control/);
});
test('offline, unsupported and busy receivers surface errors without claiming success', async () => {
  for (const [status, error] of [['OFFLINE', /offline/], ['UNSUPPORTED', /Update/], ['UPGRADE_RECEIVER', /distance/], ['BUSY', /Finish/], ['INVALID', /paired/]])
    await assert.rejects(new JigglerClient(connection([{ status }])).set(4, true), error);
  const client = new JigglerClient(connection([{ enabled: 1, pending: 1 }]), { timeoutMs: 0 });
  await assert.rejects(client.set(4, true), /awaiting receiver confirmation/);
  assert.throws(() => client.get(0), /paired/);
});
