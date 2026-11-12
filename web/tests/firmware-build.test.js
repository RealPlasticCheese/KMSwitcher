import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile, mkdtemp, mkdir, copyFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve, relative } from 'node:path';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { buildPlan, repositoryFolder } from '../build-ui.js';

test('local build plans infer checkout paths and generate build-only launchers for the chosen role', () => {
  assert.equal(repositoryFolder('file:///C:/src/KMSwitcher/web/index.html'), 'C:\\src\\KMSwitcher');
  assert.equal(repositoryFolder('file:///C:/src/KMSwitcher/web/dist/KMSwitcher.html'), 'C:\\src\\KMSwitcher');
  assert.equal(repositoryFolder('file:///D:/Downloads/MyConsole.html'), '');
  assert.equal(repositoryFolder('file:///D:/My%20repo/web/index.html?demo=1'), 'D:\\My repo');
  assert.equal(repositoryFolder('https://example.test/index.html'), '');
  assert.equal(repositoryFolder('file://server/share/repo/web/index.html'), '\\\\server\\share\\repo');
  const both = buildPlan('C:\\My repo\\', 'all', true);
  assert.match(both.command, /-Target all -ResetConfig$/);
  assert.match(both.launcher, /-File "C:\\My repo\\web\\build-firmware.ps1"/);
  assert.match(both.launcher, /exit \/b %KMS_BUILD_EXIT%/);
  assert.equal(both.outputs.length, 2);
  assert.equal(buildPlan('C:\\repo', 'receiver').outputs[0].image, 'C:\\repo\\build\\receiver\\kmswitcher_receiver.bin');
  assert.equal(buildPlan('C:\\repo', 'controller').outputs.length, 1);
  assert.match(buildPlan("C:\\Pat's repo", 'controller').command, /Pat''s repo/);
  for (const folder of ['relative', 'C:\\bad%PATH%', 'C:\\bad" & calc', 'C:\\bad\ncalc', 'C:\\bad!var!', 'C:\\bad|calc']) {
    assert.throws(() => buildPlan(folder, 'all'), /absolute Windows/);
  }
  assert.throws(() => buildPlan('C:\\repo', 'all & calc'), /Choose/);
});

async function buildFixture(t, mode) {
  const root = await mkdtemp(join(tmpdir(), 'kmswitcher-build-test-'));
  t.after(async () => {
    // Only remove the directory created by this fixture, within the temp directory.
    const location = relative(resolve(tmpdir()), resolve(root));
    assert.ok(location.startsWith('kmswitcher-build-test-') && !location.includes('..'));
    await rm(root, { recursive: true, force: true });
  });
  await mkdir(join(root, 'web'));
  await mkdir(join(root, 'scripts'));
  await copyFile(new URL('../build-firmware.ps1', import.meta.url), join(root, 'web/build-firmware.ps1'));
  await writeFile(join(root, 'scripts/idf.ps1'), `
param([string]$Project, [string]$Action, [switch]$ResetConfig)
$ErrorActionPreference = 'Stop'
if ($Action -ne 'build') { throw 'Builds must never flash a device' }
$root = Split-Path -Parent $PSScriptRoot
Add-Content (Join-Path $root 'calls.txt') "$Project $Action $ResetConfig"
if ('${mode}' -eq 'fail' -and $Project -eq 'controller') { throw 'fixture compile failure' }
$directory = Join-Path $root "build\\$Project"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$image = Join-Path $directory "kmswitcher_$Project.bin"
[IO.File]::WriteAllBytes($image, [byte[]](1, 2, 3))
$hash = '${createHash('sha256').update(Buffer.from([1, 2, 3])).digest('hex')}'
if ('${mode}' -eq 'hash') { $hash = 'incorrect' }
@{ role = $Project; size = 3; sha256 = $hash } | ConvertTo-Json | Set-Content (Join-Path $directory "kmswitcher_$Project.json")
`);
  return root;
}

test('repo build script builds both roles with reset defaults and reports matching artifacts', { skip: process.platform !== 'win32' }, async t => {
  const root = await buildFixture(t, 'ok');
  const result = spawnSync('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', join(root, 'web/build-firmware.ps1'), '-Target', 'all', '-ResetConfig'], { encoding: 'utf8', windowsHide: true });
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /SUCCESS: controller/); assert.match(result.stdout, /SUCCESS: receiver/);
  assert.match(await readFile(join(root, 'calls.txt'), 'utf8'), /receiver build True[\r\n]+controller build True/);
});

test('a failed controller build still attempts receivers and fails the overall build', { skip: process.platform !== 'win32' }, async t => {
  const root = await buildFixture(t, 'fail');
  const result = spawnSync('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', join(root, 'web/build-firmware.ps1'), '-Target', 'all'], { encoding: 'utf8', windowsHide: true });
  assert.equal(result.status, 1);
  assert.match(result.stdout, /FAILED: controller/); assert.match(result.stdout, /SUCCESS: receiver/);
  assert.match(result.stdout, /do not use old files/);
});

test('a stale or mismatched manifest fails instead of announcing a usable build', { skip: process.platform !== 'win32' }, async t => {
  const root = await buildFixture(t, 'hash');
  const result = spawnSync('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', join(root, 'web/build-firmware.ps1'), '-Target', 'receiver'], { encoding: 'utf8', windowsHide: true });
  assert.equal(result.status, 1); assert.match(result.stdout, /does not match its build manifest/);
  assert.doesNotMatch(result.stdout, /SUCCESS:/);
});
