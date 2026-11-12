import test from 'node:test';
import assert from 'node:assert/strict';
import { JSDOM } from 'jsdom';
import { initializeTheme } from '../theme.js';

function setup(t, { saved, dark = false, blocked = false } = {}) {
  const dom = new JSDOM('<select id="theme"><option>light</option><option>dark</option><option>system</option></select>', { url: 'https://console.example' });
  t.after(() => dom.window.close());
  const win = dom.window;
  const media = new win.EventTarget();
  media.matches = dark;
  win.matchMedia = () => media;
  if (saved) win.localStorage.setItem('kmswitcher-theme', saved);
  if (blocked) Object.defineProperty(win, 'localStorage', { get() { throw new Error('Storage blocked'); } });
  initializeTheme(win);
  const selector = win.document.getElementById('theme');
  return {
    win, selector,
    theme: () => win.document.documentElement.dataset.theme,
    choose(value) { selector.value = value; selector.dispatchEvent(new win.Event('change')); },
    system(value) { media.matches = value; media.dispatchEvent(new win.Event('change')); },
  };
}

test('system is the default and tracks live appearance changes', t => {
  const app = setup(t, { dark: true });
  assert.equal(app.selector.value, 'system');
  assert.equal(app.theme(), 'dark');
  app.system(false);
  assert.equal(app.theme(), 'light');
});

test('explicit themes persist and override system until system is selected again', t => {
  const app = setup(t, { saved: 'light', dark: true });
  assert.equal(app.selector.value, 'light');
  assert.equal(app.theme(), 'light');
  app.choose('dark');
  assert.equal(app.win.localStorage.getItem('kmswitcher-theme'), 'dark');
  app.system(false);
  assert.equal(app.theme(), 'dark');
  app.choose('system');
  assert.equal(app.theme(), 'light');
  assert.equal(app.win.localStorage.getItem('kmswitcher-theme'), 'system');
  app.system(true);
  assert.equal(app.theme(), 'dark');
});

test('invalid storage falls back to system and blocked storage leaves switching usable', t => {
  assert.equal(setup(t, { saved: 'invalid', dark: true }).theme(), 'dark');
  const app = setup(t, { blocked: true });
  assert.equal(app.selector.value, 'system');
  app.choose('dark');
  assert.equal(app.theme(), 'dark');
});
