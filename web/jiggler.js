let sequence = 1;
export const JIGGLER_MODES = [
  { value: 'left-right', label: 'Left / right' },
  { value: 'four-way', label: 'Up / down / right / left' },
  { value: 'random', label: 'Random strokes' },
];
export class JigglerClient {
  constructor(connection, { pollMs = 500, timeoutMs = 6000 } = {}) {
    this.connection = connection; this.pollMs = pollMs; this.timeoutMs = timeoutMs;
  }
  request(slot, enabled, mode) {
    if (!Number.isInteger(slot) || slot < 1 || slot > 7) throw new Error('Choose a paired receiver');
    const id = sequence++;
    const command = mode !== undefined ? `jiggler mode ${id} ${slot} ${JIGGLER_MODES[mode].value}` :
      enabled === undefined ? `jiggler get ${id} ${slot}` : `jiggler set ${id} ${slot} ${enabled ? 'on' : 'off'}`;
    return this.connection.exchange(command, line => {
      const match = /^KMSJIGGLER (\d+) (\w+) (\d+) ([01]) (\d+) ([01]) (\d+) (\d+) (\d+) ([012]) ([01])$/.exec(line.trim());
      if (!match || Number(match[1]) !== id || Number(match[3]) !== slot) return;
      const errors = { OFFLINE: 'Receiver is offline.', UNSUPPORTED: 'Update this receiver to use the mouse jiggler.',
        UPGRADE_RECEIVER: 'Update this receiver to use the selected jiggle mode or distance.',
        BUSY: 'Finish pairing or firmware/channel changes before changing the jiggler.', INVALID: 'Receiver is not paired.' };
      if (match[2] !== 'OK') throw new Error(errors[match[2]] ?? `Jiggler request failed: ${match[2]}`);
      return { enabled: match[4] === '1', seconds: Number(match[5]), pending: match[6] === '1',
        boot: Number(match[7]), sequence: Number(match[8]), distance: Number(match[9]),
        mode: Number(match[10]), modesSupported: match[11] === '1' };
    });
  }
  get(slot) { return this.request(slot); }
  async set(slot, enabled, update = () => {}) {
    return this.confirm(slot, await this.request(slot, enabled), update);
  }
  async setMode(slot, mode, update = () => {}) {
    if (!Number.isInteger(mode) || mode < 0 || mode >= JIGGLER_MODES.length) throw new Error('Choose a valid jiggle mode');
    return this.confirm(slot, await this.request(slot, undefined, mode), update);
  }
  async confirm(slot, requested, update) {
    const deadline = Date.now() + this.timeoutMs;
    let value = requested; update(value);
    while (value.pending && Date.now() < deadline) {
      await new Promise(resolve => setTimeout(resolve, this.pollMs));
      value = await this.get(slot); update(value);
      if (value.boot !== requested.boot) throw new Error('Receiver restarted; its jiggler starts disabled. Refresh before trying again.');
      if (value.sequence !== requested.sequence || value.enabled !== requested.enabled || value.mode !== requested.mode)
        throw new Error('Jiggler state changed from another control. Its latest state is shown.');
    }
    if (value.pending) throw new Error('Jiggler request is awaiting receiver confirmation. Refresh to check its state.');
    return value;
  }
}
