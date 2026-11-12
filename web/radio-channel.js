let sequence = 1;
export class RadioChannelClient {
  constructor(connection) { this.connection = connection; }
  async request(channel) {
    const id = sequence++;
    return this.connection.exchange(channel === undefined ? `channel get ${id}` : `channel set ${id} ${channel}`, line => {
      const match = /^KMSCHANNEL (\d+) (\w+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)$/.exec(line.trim());
      if (!match || Number(match[1]) !== id) return;
      const errors = {
        UPGRADE_SLAVES: 'Pair and OTA-update every slave on the current channel first. Wait for its channel support to be detected.',
        BUSY: 'Finish pairing, firmware updates or test mode before changing channels.',
        STORAGE: 'The controller could not save its radio channel.',
        INVALID: 'That radio channel is not allowed by the controller’s country setting.',
      };
      if (match[2] !== 'OK') throw new Error(errors[match[2]] ?? `Channel change failed: ${match[2]}`);
      const [current, pending, first, count, paired, supported] = match.slice(3).map(Number);
      return { current, pending, first, count, paired, supported };
    });
  }
  get() { return this.request(); }
  set(channel) {
    if (!Number.isInteger(channel) || channel < 1 || channel > 13) throw new Error('Choose a valid radio channel');
    return this.request(channel);
  }
}

export function createRadioChannelUI({ connection, run, message, confirmAction }) {
  const $ = id => document.getElementById(id), client = new RadioChannelClient(connection);
  let loaded = null;
  function fill(value) {
    loaded = value;
    $('radio-channel').replaceChildren();
    for (let channel = value.first; channel < value.first + value.count; channel++) {
      $('radio-channel').add(new Option(String(channel), String(channel)));
    }
    $('radio-channel').value = String(value.pending);
    $('radio-info').textContent = `Current channel ${value.current}. ${value.supported}/${value.paired} paired slaves support channel discovery.` +
      (value.current !== value.pending ? ` Switching to ${value.pending}; routing is paused.` : '');
  }
  $('load-radio-channel').addEventListener('click', () => run('Reading radio channel…', async () => {
    fill(await client.get()); message('Radio channel loaded.', 'success');
  }));
  $('save-radio-channel').addEventListener('click', () => run('Changing radio channel…', async () => {
    const channel = Number($('radio-channel').value);
    if (!loaded) throw new Error('Load the radio channel first');
    if (channel !== loaded.current && !await confirmAction('Change radio channel?',
      `Move this master and its upgraded slaves to channel ${channel}? Input pauses. Offline slaves search when reconnected. Resume routing when ready.`)) return;
    fill(await client.set(channel));
    if (loaded.pending !== loaded.current) {
      await new Promise(resolve => setTimeout(resolve, 2200));
      fill(await client.get());
      if (loaded.current !== channel) throw new Error('Channel change did not complete; reload the radio status');
      message(`Channel ${channel} saved. Allow slaves time to reconnect, then resume routing.`, 'success');
    } else message(`Channel ${channel} saved.`, 'success');
  }));
  return {
    sync(busy) {
      $('load-radio-channel').disabled = busy || !connection.connected;
      $('radio-channel').disabled = busy || !connection.connected || !loaded;
      $('save-radio-channel').disabled = busy || !connection.connected || !loaded || loaded.supported < loaded.paired;
    },
    disconnect() { loaded = null; $('radio-info').textContent = 'Load the radio channel. Upgrade paired slaves before changing it.'; },
  };
}
