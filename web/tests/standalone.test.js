import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createHash, webcrypto } from 'node:crypto';
import { JSDOM, VirtualConsole } from 'jsdom';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { DemoPort } from '../demo.js';

execFileSync(process.execPath, [fileURLToPath(new URL('../build.js', import.meta.url))]);
const outputURL = new URL('../dist/KMSwitcher.html', import.meta.url);
const html = await readFile(outputURL, 'utf8');

async function openOffline(t, search = '', pageURL = outputURL.href) {
  const errors = [], console = new VirtualConsole(); console.on('jsdomError', error => errors.push(error));
  // No resource loader or browser process: scripts run only in this offline DOM.
  const dom = new JSDOM(html, { url: `${pageURL}${search}`, runScripts: 'outside-only', pretendToBeVisual: true, virtualConsole: console });
  t.after(() => dom.window.close());
  const win = dom.window;
  Object.defineProperty(win, 'crypto', { value: webcrypto });
  Object.assign(win, { TextEncoder, TextDecoder, ReadableStream, WritableStream, TransformStream,
    isSecureContext: true });
  win.HTMLElement.prototype.scrollIntoView = () => {};
  win.eval(win.document.querySelector('script').textContent);
  const $ = id => win.document.getElementById(id);
  return { win, $, errors };
}

test('generated HTML is self-contained and protected by exact CSP hashes', () => {
  const dom = new JSDOM(html);
  try {
    const doc = dom.window.document, csp = doc.querySelector('[http-equiv="Content-Security-Policy"]').content;
    for (const tag of ['script', 'style']) {
      const digest = createHash('sha256').update(doc.querySelector(tag).textContent).digest('base64');
      assert.ok(csp.includes(`'sha256-${digest}'`), `${tag} CSP hash must match the emitted contents`);
    }
    assert.equal(doc.querySelectorAll('script[src], link[rel="stylesheet"]').length, 0);
    assert.ok(csp.includes("connect-src 'none'"));
    assert.equal(doc.querySelector('.brand').getAttribute('href'), '?');
  } finally { dom.window.close(); }
});

test('file URL console initializes offline and prepares the chosen build launcher', async t => {
  const { win, $, errors } = await openOffline(t, '', 'file:///C:/My%20repo/web/dist/KMSwitcher.html');
  assert.equal($('build-root').value, 'C:\\My repo');
  assert.equal($('build-download').disabled, false);
  $('build-target').value = 'controller'; $('build-target').dispatchEvent(new win.Event('input'));
  $('build-reset').checked = true; $('build-reset').dispatchEvent(new win.Event('input'));
  assert.match($('build-command').textContent, /-Target controller -ResetConfig$/);
  assert.match($('build-results').textContent, /kmswitcher_controller.bin/);
  assert.doesNotMatch($('build-results').textContent, /kmswitcher_receiver/);
  let downloaded;
  win.URL.createObjectURL = blob => { downloaded = blob; return 'blob:offline-test'; };
  win.URL.revokeObjectURL = () => {};
  win.HTMLAnchorElement.prototype.click = () => {};
  $('build-download').click();
  assert.ok(downloaded); assert.match($('build-status').textContent, /Launcher downloaded/);
  assert.equal($('baud-rate').value, '115200');
  assert.equal($('baud-rate').options.length, 9);
  assert.equal($('baud-rate').options[8].value, '4000000');
  assert.equal($('connection-state').textContent, 'Disconnected');
  assert.equal($('theme').value, 'system');
  $('theme').value = 'dark'; $('theme').dispatchEvent(new win.Event('change'));
  assert.equal(win.document.documentElement.dataset.theme, 'dark');
  $('theme').value = 'light'; $('theme').dispatchEvent(new win.Event('change'));
  assert.equal(win.document.documentElement.dataset.theme, 'light');
  assert.deepEqual(errors, []);
});

test('a Linux file URL requires a Windows repository folder before preparing a launcher', async t => {
  const { win, $, errors } = await openOffline(t, '', 'file:///home/builder/KMSwitcher/web/dist/KMSwitcher.html');
  assert.equal($('build-download').disabled, true);
  assert.match($('build-status').textContent, /absolute Windows repository folder/);
  $('build-root').value = 'D:\\KMSwitcher';
  $('build-root').dispatchEvent(new win.Event('input'));
  assert.equal($('build-download').disabled, false);
  assert.match($('build-command').textContent, /D:\\KMSwitcher\\web\\build-firmware.ps1/);
  assert.deepEqual(errors, []);
});


test('standalone demo exposes a working engineering terminal without device configuration panels', async t => {
  const { win, $, errors } = await openOffline(t, '?demo=1');
  const until = async predicate => {
    const deadline = Date.now() + 3000;
    while (!predicate() && Date.now() < deadline) await new Promise(resolve => setTimeout(resolve, 10));
    assert.ok(predicate(), 'expected console state before timeout');
  };
  await until(() => $('connection-state').textContent === 'Demo connected' && !$('command').disabled);
  assert.equal($('build-download').disabled, true);
  for (const id of ['firmware-form', 'preferences-form', 'usb-rows', 'receivers']) assert.equal($(id), null);
  $('command').value = 'status';
  $('command-form').dispatchEvent(new win.Event('submit', { cancelable: true }));
  await until(() => $('console-output').textContent.includes('status') && $('command').value === '');
  $('command').dispatchEvent(new win.KeyboardEvent('keydown', { key: 'ArrowUp' }));
  assert.equal($('command').value, 'status');
  $('maximise-console').click();
  assert.equal($('maximise-console').getAttribute('aria-pressed'), 'true');
  win.document.dispatchEvent(new win.KeyboardEvent('keydown', { key: 'Escape' }));
  assert.equal($('maximise-console').getAttribute('aria-pressed'), 'false');
  assert.deepEqual(errors, []);
});

test('speed dropdown connects at the selected rate and unlocks after disconnect', async t => {
  const { win, $, errors } = await openOffline(t);
  const port = new DemoPort();
  Object.defineProperty(win.navigator, 'serial', { value: { requestPort: async () => port } });
  $('baud-rate').value = '4000000';
  $('connect').disabled = false; $('connect').click();
  let deadline = Date.now() + 3000;
  while ($('connection-state').textContent !== 'Controller connected' && Date.now() < deadline) await new Promise(resolve => setTimeout(resolve, 10));
  assert.equal($('connection-state').textContent, 'Controller connected');
  assert.match($('controller-info').textContent, /4,000,000 baud/);
  assert.equal($('baud-rate').disabled, true);
  while ($('disconnect').disabled && Date.now() < deadline) await new Promise(resolve => setTimeout(resolve, 10));
  $('disconnect').click();
  deadline = Date.now() + 3000;
  while ($('connection-state').textContent !== 'Disconnected' && Date.now() < deadline) await new Promise(resolve => setTimeout(resolve, 10));
  assert.equal($('connection-state').textContent, 'Disconnected');
  assert.equal($('baud-rate').disabled, false);
  assert.deepEqual(errors, []);
});
