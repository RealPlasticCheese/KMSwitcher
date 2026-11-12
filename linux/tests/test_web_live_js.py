"""Run live rendering with selection and poll races, without opening hardware."""
from pathlib import Path
import shutil
import subprocess
import unittest


class LiveBrowserTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "Node is optional for browser-handler tests")
    def test_live_progress_preserves_selection_and_catches_up_after_copying(self):
        script = r"""
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
let selected = false, selectedWrites = 0, reportReply;
function element() {
  return {value:'', checked:false, children:[], handlers:{}, text:'', writes:0,
    scrollTop:0, scrollHeight:100, clientHeight:100,
    classList:{toggle() {}},
    get textContent() { return this.text; },
    set textContent(value) { this.text = value; this.writes++; if (selected) selectedWrites++; },
    append(...items) { this.children.push(...items); },
    replaceChildren() { this.children = []; if (selected) selectedWrites++; },
    setAttribute() {}, addEventListener(kind, fn) { this.handlers[kind] = fn; }};
}
const nodes = new Map();
const node = id => { if (!nodes.has(id)) nodes.set(id, element()); return nodes.get(id); };
const context = vm.createContext({document:{getElementById:node, createElement:element,
    querySelectorAll:()=>[], getSelection:()=>({isCollapsed:!selected})}, structuredClone,
  setTimeout:()=>0, clearTimeout:()=>{},
  fetch:()=>reportReply ? reportReply : new Promise(()=>{})});
vm.runInContext(fs.readFileSync(process.argv[1], 'utf8'), context);
const initial = {
  version:'0.3.17', revision:1, busy:true, operation:'run', profile:'/profile.json',
  config:{master:{identity:'', uart:''}, masters:[], generators:[], receivers:[],
    preset:'all-tests', rates:[10], seconds:30, soak_seconds:3600, seed:1,
    max_log_mb:1024, output:'/results', stop_on_failure:false},
  messages:['[2026-10-26 14:47:38+10:00] First line'], report:null, workload_text:'All tests',
  progress:{cycle:1, test:'usb-stall', slot:1, remaining_seconds:724, counts:{FAIL:3, PASS:65}}
};
context.value = initial;
vm.runInContext('applyState(value)', context);
assert.equal(node('run-summary').hidden, false);
assert.equal(node('run-summary').textContent, '0h 12m 4s remaining · FAIL: 3 · PASS: 65');
assert.match(node('suite-progress').textContent, /Cycle 1 · usb-stall · receiver 1/);
const logWrites = node('log').writes, statusWrites = node('status').writes;
vm.runInContext('applyState(value)', context);
assert.equal(node('log').writes, logWrites);
assert.equal(node('status').writes, statusWrites);

selected = true;
const next = structuredClone(initial);
next.messages.push('[2026-10-26 14:47:40+10:00] New line <plain text>');
next.progress.remaining_seconds = 722; next.progress.counts.PASS = 66;
context.value = next;
vm.runInContext('applyState(value)', context);
assert.equal(selectedWrites, 0);
assert.equal(node('log').textContent, initial.messages[0]);
assert.equal(node('run-summary').textContent, '0h 12m 4s remaining · FAIL: 3 · PASS: 65');
assert.equal(vm.runInContext('state.messages.length', context), 2);
selected = false;
vm.runInContext('applyState(state)', context);
assert.equal(node('log').textContent, next.messages.join('\n'));
assert.equal(node('run-summary').textContent, '0h 12m 2s remaining · FAIL: 3 · PASS: 66');

// Selection starts while the reports request is in flight.
let resolve;
reportReply = new Promise(done => { resolve = done; });
const refresh = vm.runInContext('refreshReports()', context);
selected = true;
resolve({ok:true, json:async()=>({root:'/results', reports:[]})});
(async () => {
  await refresh;
  assert.equal(selectedWrites, 0);
  selected = false;
  await vm.runInContext('refreshReports()', context);
  assert.equal(node('report-root').textContent, 'Server results directory: /results');

  // A forced saved-profile refresh is deferred, not lost, while text is selected.
  vm.runInContext('dirty = true', context);
  context.value = structuredClone(next); context.value.config.seed = 7;
  selected = true;
  vm.runInContext('applyState(value, true)', context);
  assert.equal(selectedWrites, 0);
  selected = false;
  vm.runInContext('applyState(state)', context);
  assert.equal(node('seed').value, 7);

  // Every preset can render progress without suite-only cycle fields.
  for (const preset of ['baseline', 'quick', 'everyday', 'routing', 'overlap', 'resilience',
      'background', 'media', 'limit', 'overload', 'soak', 'pointer', 'full', 'usb-stall', 'all-tests']) {
    context.value = {...next, progress:{test:preset, slot:preset === 'baseline' ? 'direct' : 1,
      phase:'running', remaining_seconds:10800, counts:{PASS:2},
      estimated:!['soak', 'all-tests'].includes(preset)}};
    vm.runInContext('applyState(value)', context);
    assert.equal(node('run-summary').hidden, false);
    assert.match(node('run-summary').textContent, /3h 0m 0s remaining/);
    assert.equal(node('run-summary').textContent.startsWith('Estimated '), context.value.progress.estimated);
    assert.doesNotMatch(node('suite-progress').textContent, /undefined|NaN|Infinity|Cycle/);
    if (preset === 'baseline') assert.match(node('suite-progress').textContent, /direct USB/);
  }
  context.value.progress.phase = 'preparing';
  vm.runInContext('applyState(value)', context);
  assert.match(node('run-summary').textContent, /Preparing hardware/);
  context.value.progress.phase = 'cleanup';
  vm.runInContext('applyState(value)', context);
  assert.match(node('run-summary').textContent, /Finishing capture and cleanup/);
  assert.doesNotMatch(node('run-summary').textContent, /remaining/);
  context.value.progress.phase = 'finished'; context.value.progress.status = 'CANCELLED';
  vm.runInContext('applyState(value)', context);
  assert.equal(node('run-summary').textContent, 'CANCELLED · PASS: 2');

  context.value = {...next, progress:null};
  vm.runInContext('applyState(value)', context);
  assert.equal(node('run-summary').hidden, true);
})().catch(error => { console.error(error); process.exitCode = 1; });
"""
        path = Path(__file__).parents[1] / "kms_stress/web_assets/app.js"
        result = subprocess.run([shutil.which("node"), "-e", script, str(path)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
