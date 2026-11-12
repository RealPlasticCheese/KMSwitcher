import { PreferencesClient } from './preferences.js';

export function createPreferencesUI({ connection, run, message, confirmAction }) {
  const $ = id => document.getElementById(id), client = new PreferencesClient(connection);
  let loaded = null, dirty = false;
  const palette = ['Off', 'Red', 'Green', 'Yellow', 'Blue', 'Magenta', 'Cyan', 'White'];
  for (let slot = 1; slot <= 7; slot++) {
    const label = document.createElement('label'); label.textContent = `Computer ${slot}`;
    const select = document.createElement('select'); select.id = `pref-color-${slot}`; select.dataset.preference = '';
    palette.forEach((name, color) => select.add(new Option(name, String(color))));
    label.append(select); $('slot-colors').append(label);
  }
  function sync(busy) {
    for (const input of document.querySelectorAll('[data-preference]')) input.disabled = busy || !connection.connected || !loaded;
  }
  function fill(snapshot) {
    loaded = snapshot; dirty = false;
    const p = snapshot.values;
    $('pref-boot-slot').value = p.bootSlot; $('pref-trigger').value = p.trigger; $('pref-double').value = p.doubleMs;
    $('pref-select').value = p.selectMs; $('pref-brightness').value = p.brightness;
    $('pref-jiggler').value = p.jigglerSeconds;
    $('pref-jiggler-distance').value = p.jigglerDistance;
    $('pref-normal-led').checked = Boolean(p.normalLed); $('pref-keyboard-leds').checked = Boolean(p.keyboardLeds);
    p.slotColors.forEach((value, i) => { $(`pref-color-${i + 1}`).value = value; });
    $('preferences-info').textContent = 'Loaded from the controller. Save changes to apply them and keep them across restarts.';
  }
  async function load() { fill(await client.get()); }
  $('load-preferences').addEventListener('click', () => run('Loading controller preferences…', async () => {
    if (dirty && !await confirmAction('Discard unsaved preference changes?', 'Reload the saved values from the controller?')) { message('Reload cancelled.'); return; }
    try { await load(); message('Controller preferences loaded.', 'success'); }
    catch (error) { throw new Error(`Could not load preferences: ${error.message}. The master needs preferences-capable firmware.`); }
  }));
  $('preferences-form').addEventListener('input', () => {
    dirty = Boolean(loaded); $('preferences-info').textContent = 'Unsaved changes. Save preferences to apply them.';
  });
  $('preferences-form').addEventListener('submit', event => {
    event.preventDefault();
    void run('Saving controller preferences…', async () => {
      if (!loaded) throw new Error('Load preferences from the controller first');
      const values = { bootSlot: Number($('pref-boot-slot').value), trigger: Number($('pref-trigger').value), doubleMs: Number($('pref-double').value),
        selectMs: Number($('pref-select').value), brightness: Number($('pref-brightness').value),
        jigglerSeconds: Number($('pref-jiggler').value),
        jigglerDistance: Number($('pref-jiggler-distance').value),
        normalLed: Number($('pref-normal-led').checked), keyboardLeds: Number($('pref-keyboard-leds').checked),
        slotColors: Array.from({ length: 7 }, (_, i) => Number($(`pref-color-${i + 1}`).value)) };
      fill(await client.save(values, loaded.revision)); message('Preferences saved on the controller and applied. Pairings retained.', 'success');
    });
  });
  $('reset-preferences').addEventListener('click', () => run('Preparing preference reset…', async () => {
    if (!loaded) throw new Error('Load preferences from the controller first');
    if (!await confirmAction('Restore everyday preferences to defaults?', 'Reset the boot dongle, shortcut key/timings, mouse jiggler interval/distance, normal LED indication, brightness, computer colours and keyboard LED feedback. Computer pairings and diagnostic history are kept.')) { message('Preference reset cancelled.'); return; }
    fill(await client.reset(loaded.revision)); message('Default preferences restored. Pairings and history retained.', 'success');
  }));
  function disconnect() {
    loaded = null; dirty = false; $('preferences-info').textContent = 'Load the controller’s saved preferences before editing.'; sync(false);
  }
  window.addEventListener('beforeunload', event => { if (dirty) { event.preventDefault(); event.returnValue = ''; } });
  sync(false);
  return { sync, disconnect, load, isDirty: () => dirty };
}
