import test from 'node:test';
import assert from 'node:assert/strict';
import { RadioChannelClient } from '../radio-channel.js';

function connection(status, suffix = '6 6 1 13 2 2') {
  return { async exchange(command, accept) {
    const id = command.split(' ')[2];
    assert.equal(accept(`KMSCHANNEL ${Number(id) + 1} OK ${suffix}`), undefined);
    return accept(`KMSCHANNEL ${id} ${status} ${suffix}`);
  } };
}
test('radio status correlates IDs and exposes receiver upgrade readiness', async () => {
  const client = new RadioChannelClient(connection('OK'));
  assert.deepEqual(await client.get(), { current: 6, pending: 6, first: 1, count: 13, paired: 2, supported: 2 });
});
test('changing channel returns current and pending values', async () => {
  const client = new RadioChannelClient(connection('OK', '6 11 1 13 0 0'));
  assert.equal((await client.set(11)).pending, 11);
  assert.throws(() => client.set(14), /valid/);
  assert.throws(() => client.set(1.5), /valid/);
});
test('legacy receivers and active transfers block channel changes with useful messages', async () => {
  await assert.rejects(new RadioChannelClient(connection('UPGRADE_SLAVES')).set(11), /OTA-update/);
  await assert.rejects(new RadioChannelClient(connection('BUSY')).set(11), /test mode/);
  await assert.rejects(new RadioChannelClient(connection('STORAGE')).set(11), /save/);
});
