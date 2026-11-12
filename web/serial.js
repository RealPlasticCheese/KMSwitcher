export const CONSOLE_BAUD_RATES = [115200, 230400, 460800, 921600, 1000000, 1500000, 2000000, 3000000, 4000000];

// One reader feeds both the terminal and protocol requests.
export class SerialConnection {
  constructor({ onLine = () => {}, onClose = () => {}, onBaudRateChange = () => {} } = {}) {
    this.onLine = onLine;
    this.onClose = onClose;
    this.onBaudRateChange = onBaudRateChange;
    this.listeners = new Set();
    this.waiters = new Set();
    this.connected = false;
  }
  async open(port, baudRate = 115200) {
    if (this.port) throw new Error('A serial connection is already open');
    this.port = port;
    try {
      // Do not toggle DTR/RTS: Chromium clears DTR first, pulsing the board's EN reset.
      await port.open({ baudRate, flowControl: 'none' });
      this.baudRate = baudRate;
      this.onBaudRateChange(baudRate);
      this.reader = port.readable.getReader();
      this.writer = port.writable.getWriter();
      this.connected = true;
      this.readTask = this.readLoop();
    } catch (error) {
      try { await port.close(); } catch { /* It may never have opened. */ }
      this.port = null;
      throw error;
    }
  }
  async readLoop() {
    const decoder = new TextDecoder();
    let partial = '', failure;
    try {
      while (this.connected) {
        const { value, done } = await this.reader.read();
        if (done) break;
        partial += decoder.decode(value, { stream: true });
        let newline;
        while ((newline = partial.indexOf('\n')) >= 0) {
          const line = partial.slice(0, newline).replace(/\r$/, '').replace(/\x1b\[[0-9;]*m/g, '');
          partial = partial.slice(newline + 1);
          if (line.length <= 16384) this.deliver(line);
        }
        if (partial.length > 16384) partial = '';
      }
    } catch (error) { failure = error; }
    finally {
      this.connected = false;
      if (partial) this.deliver(partial);
      for (const reject of [...this.waiters]) reject(new Error('Serial connection closed'));
      this.reader.releaseLock();
      try { await this.writer.abort(); } catch { /* Disconnected hardware. */ }
      this.writer.releaseLock();
      try { await this.port.close(); } catch { /* Port may already be gone. */ }
      this.port = null;
      this.stopKeepAlive();
      if (!this.reconfiguring) this.onClose(failure);
    }
  }
  deliver(line) {
    this.onLine(line);
    for (const listener of [...this.listeners]) listener(line);
  }
  async close({ restore = true } = {}) {
    this.stopKeepAlive();
    if (restore && this.connected && this.baudRate !== 115200 && !this.reconfiguring) {
      // The worker returns to the base rate even after this handle closes.
      try { await this.baudRequest('set', 115200); } catch { /* Lease expiry also restores it. */ }
    }
    this.connected = false;
    try { await this.reader?.cancel(); } catch { /* Already disconnected. */ }
    await this.readTask;
  }
  async reopen(baudRate) {
    const port = this.port;
    this.reconfiguring = true;
    try {
      await this.close();
      await this.open(port, baudRate);
    } finally { this.reconfiguring = false; }
  }
  async changeBaud(baudRate, before = null) {
    before ??= await this.baudRequest('get');
    const switching = await this.baudRequest('set', baudRate);
    if (switching.status !== 'SWITCH' || switching.baudRate !== baudRate || switching.boot !== before.boot) {
      throw new Error('Invalid console speed switch acknowledgement');
    }
    await this.reopen(baudRate);
    let confirmed;
    for (let attempt = 0; attempt < 4 && !confirmed; attempt++) {
      try { confirmed = await this.baudRequest('get'); }
      catch (error) { if (!this.connected || !/timed out/.test(error.message) || attempt === 3) throw error; }
    }
    if (confirmed.status !== 'OK' || confirmed.baudRate !== baudRate || confirmed.boot !== before.boot) {
      throw new Error('Console restarted or the requested speed was not confirmed');
    }
  }
  stopKeepAlive() {
    clearInterval(this.keepAlive);
    this.keepAliveGeneration = (this.keepAliveGeneration ?? 0) + 1;
  }
  startKeepAlive(baudRate, boot) {
    this.stopKeepAlive();
    this.baudSession = { baudRate, boot };
    const generation = this.keepAliveGeneration;
    this.keepAlive = setInterval(() => {
      if (this.keepAliveTask) return;
      const task = this.baudRequest('get').then(reply => {
        if (generation !== this.keepAliveGeneration) return;
        if (reply.boot !== boot || reply.baudRate !== baudRate) throw new Error('Console speed session changed');
      }).catch(error => {
        if (generation !== this.keepAliveGeneration) return;
        this.onLine(`Console speed connection lost: ${error.message}. Reconnect at 115200.`);
        void this.close();
      }).finally(() => { if (this.keepAliveTask === task) this.keepAliveTask = null; });
      this.keepAliveTask = task;
    }, 2000);
  }
  async prepareForMasterReboot() {
    this.stopKeepAlive();
    // Settle an already queued probe before activation; its old-boot result
    // must not close a deliberately rebooting connection.
    await this.keepAliveTask;
  }
  cancelMasterReboot() {
    if (this.connected && this.baudRate !== 115200 && this.baudSession) {
      this.startKeepAlive(this.baudSession.baudRate, this.baudSession.boot);
    }
  }
  async reopenForMasterReboot() {
    // The master starts at the base rate. Change only the PC's port here:
    // negotiating with the old firmware after ACTIVATE could miss the reboot.
    if (this.baudRate !== 115200) await this.reopen(115200);
    this.baudSession = null;
  }
  async returnToBase() {
    if (this.baudRate === 115200) return;
    this.stopKeepAlive();
    try { await this.changeBaud(115200); }
    catch (error) { await this.close(); throw error; }
  }
  baudRequest(action, baudRate, timeout = 1000) {
    const id = this.baudId = (((this.baudId ?? 0) + 1) >>> 0) || 1;
    return this.exchange(`baud ${action} ${id}${baudRate ? ` ${baudRate}` : ''}`, line => {
      const match = /^KMSBAUD (\d+) (OK|SWITCH|INVALID|BUSY|ERROR) (\d+) (\d+)$/.exec(line);
      if (!match || Number(match[1]) !== id) return;
      if (!['OK', 'SWITCH'].includes(match[2])) throw new Error(`Console baud negotiation: ${match[2]}`);
      return { status: match[2], baudRate: Number(match[3]), boot: Number(match[4]) };
    }, timeout);
  }
  async negotiate(baudRate) {
    if (!CONSOLE_BAUD_RATES.includes(baudRate)) throw new Error('Unsupported console baud rate');
    if (baudRate === 115200) {
      const deadline = Date.now() + 12000;
      while (true) {
        try {
          await this.exchange('about', line => /KMSwitcher.*ESP32-S3/.test(line) ? true : undefined, 1000);
          return;
        } catch (error) {
          if (!this.connected || !/timed out/.test(error.message) || Date.now() >= deadline) {
            await this.close();
            throw error;
          }
        }
      }
    }
    try {
      // A vanished browser's high-speed lease expires after 10 seconds.
      let current;
      const deadline = Date.now() + 12000;
      while (!current) {
        try { current = await this.baudRequest('get'); }
        catch (error) {
          if (!this.connected || !/timed out/.test(error.message) || Date.now() >= deadline) throw error;
        }
      }
      await this.changeBaud(baudRate, current);
      this.startKeepAlive(baudRate, current.boot);
    } catch (error) {
      await this.close();
      throw new Error(`Could not connect at ${baudRate} baud: ${error.message}. Use 115200 or update the master firmware. The console returns to 115200 within 10 seconds.`);
    }
  }
  async send(command) {
    if (!this.connected) throw new Error('Connect the controller first');
    if (!command || command.length > 511 || /[^\x20-\x7e]/.test(command)) {
      throw new Error('Commands must be one ASCII line of 1–511 characters');
    }
    let timer;
    try {
      await Promise.race([this.writer.write(new TextEncoder().encode(command + '\n')),
        new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('Serial write timed out')), 5000); })]);
    } catch (error) {
      void this.close({ restore: false });
      throw error;
    } finally { clearTimeout(timer); }
  }
  exchange(command, accept, timeout = 6000) {
    return new Promise((resolve, reject) => {
      let timer, finished = false;
      const finish = (error, result) => {
        if (finished) return;
        finished = true;
        clearTimeout(timer);
        this.listeners.delete(listener);
        this.waiters.delete(fail);
        if (error) reject(error); else resolve(result);
      };
      const fail = error => finish(error);
      const listener = line => {
        try {
          const result = accept(line);
          if (result !== undefined) finish(null, result);
        } catch (error) { fail(error); }
      };
      this.listeners.add(listener);
      this.waiters.add(fail);
      timer = setTimeout(() => fail(new Error('Controller response timed out')), timeout);
      this.send(command).catch(fail);
    });
  }
}
