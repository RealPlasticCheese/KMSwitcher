import { initializeTheme } from './theme.js';
import { createBuildUI } from './build-ui.js';
import { SerialConnection, CONSOLE_BAUD_RATES } from './serial.js';
import { DemoPort } from './demo.js';

const $ = id => document.getElementById(id);
initializeTheme(window);
const demo = new URLSearchParams(location.search).get('demo') === '1';
const state = { busy: false, lines: [], chars: 0, discarded: 0, commands: [], commandIndex: 0 };
let renderPending = false;
function message(text, kind = '') { $('message').textContent = text; $('message').className = `message ${kind}`; }
function log(text, kind = '') {
  const entry = { text, kind, time: new Date().toLocaleTimeString([], { hour12: false }), protocol: /^(KMSM |KMSMASTER |KMSBAUD |remote |master )/.test(text) };
  state.lines.push(entry); state.chars += text.length;
  while (state.lines.length > 10000 || state.chars > 4 * 1024 * 1024) {
    state.chars -= state.lines.shift().text.length; state.discarded++;
  }
  scheduleConsole();
}
function scheduleConsole() {
  if (renderPending) return;
  renderPending = true;
  requestAnimationFrame(() => {
    renderPending = false;
    const output = $('console-output'), fragment = document.createDocumentFragment();
    for (const entry of state.lines.filter(line => $('show-protocol').checked || !line.protocol).slice(-700)) {
      const span = document.createElement('span'); span.className = entry.kind;
      span.textContent = `${entry.time}  ${entry.kind === 'sent' ? '❯ ' : ''}${entry.text}\n`; fragment.append(span);
    }
    output.replaceChildren(fragment);
    $('line-count').textContent = `${state.lines.length.toLocaleString()} lines${state.discarded ? ` · ${state.discarded.toLocaleString()} older lines discarded` : ''} · showing latest 700`;
    if ($('autoscroll').checked) output.scrollTop = output.scrollHeight;
  });
}
const connection = new SerialConnection({
  onLine: line => log(line),
  onBaudRateChange: rate => { $('controller-info').textContent = `ESP32-S3 · ${rate.toLocaleString('en-US')} baud`; },
  onClose: error => {
    $('controller-info').textContent = 'ESP32-S3 · COM/UART';
    log(error ? `Connection lost: ${error.message}` : 'Serial connection closed.', error ? 'failure' : 'system');
    message(error ? `Connection lost: ${error.message}. Reconnect to continue.` : 'Disconnected. Console capture is still available to download.', error ? 'error' : '');
    syncControls();
  }
});
function syncControls() {
  for (const control of document.querySelectorAll('[data-device]')) control.disabled = !connection.connected || state.busy;
  $('connect').hidden = connection.connected;
  $('connect').disabled = state.busy || (!demo && (!('serial' in navigator) || !isSecureContext));
  $('baud-rate').disabled = connection.connected || state.busy;
  $('disconnect').hidden = !connection.connected;
  $('disconnect').disabled = state.busy;
  const badge = $('connection-state');
  badge.className = `badge${connection.connected ? ' active' : ''}`;
  const dot = document.createElement('span'); dot.className = 'dot';
  badge.replaceChildren(dot, connection.connected ? (demo ? 'Demo connected' : 'Controller connected') : 'Disconnected');
}
async function run(label, action) {
  if (state.busy) return;
  state.busy = true; syncControls(); message(label);
  try { await action(); }
  catch (error) { message(error.message, 'error'); log(error.message, 'failure'); }
  finally { state.busy = false; syncControls(); }
}
function download(name, content, type = 'text/plain') {
  const url = URL.createObjectURL(new Blob([content], { type })), anchor = document.createElement('a');
  anchor.href = url; anchor.download = name; anchor.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
const stamp = () => new Date().toISOString().replace(/[:.]/g, '-');

async function send(command) { log(command, 'sent'); await connection.send(command); }
async function connectPort() {
  let port;
  try { port = demo ? new DemoPort() : await navigator.serial.requestPort(); }
  catch (error) {
    if (error.name === 'NotFoundError') { message('Connection cancelled.'); return; }
    throw error;
  }
  await connection.open(port);
  await connection.negotiate(Number($('baud-rate').value));
  log(demo ? 'Demo connected. All output is simulated.' : `Connected at ${connection.baudRate} baud. Serial reset lines left untouched.`, 'system');
  await send('about');
  message(demo ? 'Connected to the simulated controller.' : 'Connected. Use the shortcuts or enter a command.', 'success');
}
for (const rate of CONSOLE_BAUD_RATES) $('baud-rate').add(new Option(`${rate.toLocaleString('en-US')} baud`, String(rate)));
$('baud-rate').value = '115200';
$('connect').addEventListener('click', () => void run('Choose the controller’s COM/UART port…', connectPort));
$('disconnect').addEventListener('click', () => void run('Disconnecting…', () => connection.close()));
for (const button of document.querySelectorAll('[data-command]')) {
  button.addEventListener('click', () => void run(`Sending ${button.dataset.command}…`, async () => {
    await send(button.dataset.command);
    message('Command sent. Output appears in the console.');
  }));
}
$('command-form').addEventListener('submit', event => {
  event.preventDefault();
  const command = $('command').value.trim(); if (!command) return;
  void run('Sending console command…', async () => {
    await send(command); state.commands.push(command); if (state.commands.length > 100) state.commands.shift();
    state.commandIndex = state.commands.length; $('command').value = ''; message('Command sent. Output appears in the console.');
  }).then(() => $('command').focus());
});
$('command').addEventListener('keydown', event => {
  if (!['ArrowUp', 'ArrowDown'].includes(event.key)) return;
  event.preventDefault();
  state.commandIndex = Math.max(0, Math.min(state.commands.length, state.commandIndex + (event.key === 'ArrowUp' ? -1 : 1)));
  $('command').value = state.commands[state.commandIndex] ?? '';
});
$('download-console').addEventListener('click', () => download(`kmswitcher-console-${stamp()}.log`,
  `${state.discarded ? `[${state.discarded} older lines discarded by the capture limit]\n` : ''}` + state.lines.map(entry => `${entry.time} ${entry.kind === 'sent' ? '> ' : ''}${entry.text}`).join('\n') + '\n'));
$('clear-console').addEventListener('click', () => { state.lines = []; state.chars = 0; state.discarded = 0; scheduleConsole(); });
function maximiseConsole(value) {
  $('console-panel').classList.toggle('maximised', value); document.body.classList.toggle('console-maximised', value);
  document.querySelector('.topbar').inert = value;
  for (const sibling of document.querySelector('main').children) if (sibling !== $('console-panel')) sibling.inert = value;
  $('maximise-console').textContent = value ? 'Restore ↙' : 'Maximise ↗';
  $('maximise-console').setAttribute('aria-pressed', String(value));
  if (value) $('console-output').focus(); else $('maximise-console').focus();
}
$('maximise-console').addEventListener('click', () => maximiseConsole(!$('console-panel').classList.contains('maximised')));
document.addEventListener('keydown', event => {
  if (event.key === 'Escape' && $('console-panel').classList.contains('maximised')) maximiseConsole(false);
});

createBuildUI({ demo, download });
$('show-protocol').addEventListener('change', scheduleConsole);
if (!demo && (!isSecureContext || !('serial' in navigator))) {
  $('support').hidden = false;
  $('support').textContent = 'Device access requires a browser with Web Serial, such as desktop Chrome or Edge. Open the generated HTML from disk or use HTTPS.';
}
$('demo-banner').hidden = !demo; $('demo-link').hidden = demo;
log('KMSwitcher Engineering Console. Connect the S3’s COM/UART connector and close other serial tools first.', 'system');
syncControls();
if (demo) void run('Connecting simulated controller…', connectPort);
