import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { Script } from 'node:vm';

test('standalone build is deterministic, self-contained and allowed by its CSP', async () => {
  const build = fileURLToPath(new URL('../build.js', import.meta.url));
  const output = new URL('../dist/KMSwitcher.html', import.meta.url);
  execFileSync(process.execPath, [build]);
  const html = await readFile(output, 'utf8');
  execFileSync(process.execPath, [build]);
  assert.equal(await readFile(output, 'utf8'), html);

  assert.doesNotMatch(html, /<script[^>]+src=|<link[^>]+rel="stylesheet"|type="module"/);
  assert.ok(html.includes('Copyright (c) 2023 Arjun Barrett'));
  assert.doesNotMatch(html, /href="\.\/"|unsafe-inline|unsafe-eval/);
  const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)];
  const styles = [...html.matchAll(/<style>([\s\S]*?)<\/style>/g)];
  assert.equal(scripts.length, 1);
  assert.equal(styles.length, 1);
  const policy = html.match(/http-equiv="Content-Security-Policy" content="([^"]+)"/)[1];
  for (const [directive, content] of [['script-src', scripts[0][1]], ['style-src', styles[0][1]]]) {
    const hash = createHash('sha256').update(content).digest('base64');
    assert.ok(policy.includes(`${directive} 'sha256-${hash}'`));
  }
  new Script(scripts[0][1]); // Must parse as a classic script without module imports.
  assert.ok(html.indexOf('<script>') > html.indexOf('id="command-form"'));
});
