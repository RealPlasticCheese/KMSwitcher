"""Run the shipped browser handlers with a small DOM adapter; no server/devices."""
from pathlib import Path
import shutil
import subprocess
import unittest


class RecipeBrowserTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "Node is optional for browser-handler tests")
    def test_restart_buttons_target_devices_and_stop_while_busy(self):
        script = r"""
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
function element() { return {value:'', checked:false, children:[], handlers:{},
  append(...items) { this.children.push(...items); }, replaceChildren() { this.children = []; },
  setAttribute() {}, addEventListener(kind, fn) { this.handlers[kind] = fn; }}; }
const nodes = new Map(), calls = [];
const node = id => { if (!nodes.has(id)) nodes.set(id, element()); return nodes.get(id); };
const context = vm.createContext({document:{getElementById:node, createElement:element, querySelectorAll:()=>[]},
  structuredClone, setTimeout:()=>0, clearTimeout:()=>{}, fetch:()=>new Promise(()=>{})});
vm.runInContext(fs.readFileSync(process.argv[1], 'utf8'), context);
vm.runInContext(`renderConfig({master:{identity:'112233445566', name:'Master', uart:'one', enabled:true},
  generators:[], receivers:[{identity:'AABBCCDDEE01', name:'Office receiver', slot:1, enabled:true}],
  preset:'quick', rates:[10], seconds:1, soak_seconds:60, seed:1, max_log_mb:16, output:'/results',
  stop_on_failure:true, restart_before_run:true});
  state = {busy:false}; applyState = value => {state = value};`, context);
context.fetch = async (path, options) => {
  calls.push({path, body:JSON.parse(options.body)});
  return {ok:true, json:async()=>({busy:true})};
};
(async () => {
  const row = node('devices').children[1];
  const button = row.children[5].children[0].children.find(x => x.textContent === 'Restart');
  await button.handlers.click();
  assert.equal(calls[0].path, '/api/restart');
  assert.deepEqual(calls[0].body.target, {role:'receiver', identity:'AABBCCDDEE01'});
  assert.equal(calls[0].body.config.restart_before_run, true);
  assert.equal(node('editor').disabled, true);
  await node('restart-all').handlers.click();
  assert.equal(calls.length, 1);
  vm.runInContext('state.busy = false', context);
  await node('restart-all').handlers.click();
  assert.equal(calls.length, 2); assert.equal(calls[1].body.target, null);
})().catch(error => { console.error(error); process.exitCode = 1; });
"""
        path = Path(__file__).parents[1] / "kms_stress/web_assets/app.js"
        result = subprocess.run([shutil.which("node"), "-e", script, str(path)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which("node"), "Node is optional for browser-handler tests")
    def test_import_clear_and_oversize_recipe_handlers(self):
        script = r"""
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const nodes = new Map();
function node(id) {
  if (!nodes.has(id)) nodes.set(id, {value:'', checked:false, textContent:'', handlers:{},
    addEventListener(kind, fn) { this.handlers[kind] = fn; }});
  return nodes.get(id);
}
const context = vm.createContext({document:{getElementById:node}, structuredClone,
  setTimeout:()=>0, clearTimeout:()=>{}, fetch:()=>new Promise(()=>{})});
vm.runInContext(fs.readFileSync(process.argv[1], 'utf8'), context);
vm.runInContext(`renderDevices = () => {};
  renderConfig({master:{}, generators:[], receivers:[], preset:'quick', rates:[10], seconds:1,
    soak_seconds:60, seed:1, max_log_mb:16, output:'/results', stop_on_failure:true});`, context);
(async () => {
  const text = JSON.stringify({version:1, case:{slot:6}, fixture:{master:'known'}, case_id:'saved'});
  const target = {value:'recipe.json', files:[{size:text.length, text:async()=>text}]};
  await node('campaign_recipe_file').handlers.change({target});
  assert.equal(node('error').textContent, '');
  let result = vm.runInContext('readConfig(false)', context);
  assert.equal(result.campaign_recipe, text); assert.equal(result.preset, 'usb-stall');
  assert.equal(result.campaign_repeats, 3); assert.equal(target.value, '');
  node('clear_campaign_recipe').handlers.click();
  assert.equal(vm.runInContext('readConfig(false).campaign_recipe', context), '');
  node('preset').value = 'all-tests'; node('preset').handlers.change();
  assert.equal(node('stop_on_failure').checked, false);
  assert.equal(node('loop-note').hidden, false);
  assert.equal(vm.runInContext('readConfig(false).recovery_attempts', context), 3);
  await node('campaign_recipe_file').handlers.change({target:{files:[{size:16385}]}});
  assert.match(node('error').textContent, /16 KiB/);
})().catch(error => { console.error(error); process.exitCode = 1; });
"""
        path = Path(__file__).parents[1] / "kms_stress/web_assets/app.js"
        result = subprocess.run([shutil.which("node"), "-e", script, str(path)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which("node"), "Node is optional for browser-handler tests")
    def test_master_remove_select_and_run_validation(self):
        script = r"""
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
function element() { return {value:'', checked:false, textContent:'', children:[], handlers:{},
  append(...items) { this.children.push(...items); }, replaceChildren() { this.children = []; },
  setAttribute() {}, addEventListener(kind, fn) { this.handlers[kind] = fn; }}; }
const nodes = new Map();
const node = id => { if (!nodes.has(id)) nodes.set(id, element()); return nodes.get(id); };
node('configuration').reportValidity = () => true;
const context = vm.createContext({document:{getElementById:node, createElement:element, querySelectorAll:()=>[]}, structuredClone,
  setTimeout:()=>0, clearTimeout:()=>{}, fetch:()=>new Promise(()=>{})});
vm.runInContext(fs.readFileSync(process.argv[1], 'utf8'), context);
vm.runInContext(`renderConfig({master:{identity:'112233445566', name:'Master', uart:'one', enabled:true},
  generators:[], receivers:[], preset:'quick', rates:[10], seconds:1, soak_seconds:60,
  seed:1, max_log_mb:16, output:'/results', stop_on_failure:true});`, context);
let row = node('devices').children[0];
assert.equal(row.children[0].children[0].checked, true);
row.children[5].children[0].children.find(button => button.textContent === 'Remove').handlers.click();
assert.equal(node('devices').children.length, 0);
assert.equal(vm.runInContext('readConfig(false).masters.length', context), 0);
(async () => {
  await vm.runInContext('action("/api/run")', context);
  assert.match(node('error').textContent, /exactly one master/);
  vm.runInContext(`config.masters.push({identity:'AABBCCDDEEFF', name:'Other', uart:'two', enabled:false}); renderDevices();`, context);
  const checkbox = node('devices').children[0].children[0].children[0];
  checkbox.checked = true; checkbox.handlers.input();
  assert.equal(vm.runInContext('readConfig(false).master.uart', context), 'two');
})().catch(error => { console.error(error); process.exitCode = 1; });
"""
        path = Path(__file__).parents[1] / "kms_stress/web_assets/app.js"
        result = subprocess.run([shutil.which("node"), "-e", script, str(path)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
