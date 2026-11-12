import { build, transform } from 'esbuild';
import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

const root = new URL('.', import.meta.url);
const [template, scripts, styles] = await Promise.all([
  readFile(new URL('index.html', root), 'utf8'),
  build({
    absWorkingDir: fileURLToPath(root),
    entryPoints: ['app.js'],
    bundle: true,
    format: 'iife',
    platform: 'browser',
    target: ['chrome100', 'edge100'],
    charset: 'utf8',
    write: false,
  }),
  readFile(new URL('styles.css', root), 'utf8').then(css =>
    transform(css, { loader: 'css', charset: 'utf8' })),
]);

// Hash exactly the inline contents so CSP stays restrictive without unsafe-inline.
const license = await readFile(new URL('fflate-LICENSE.txt', root), 'utf8');
const script = '/*!\n' + license + '\n*/\n' + scripts.outputFiles[0].text;
const style = styles.code;
const hash = text => `'sha256-${createHash('sha256').update(text).digest('base64')}'`;
const html = template
  .replace("script-src 'self'", `script-src ${hash(script)}`)
  .replace("style-src 'self'", `style-src ${hash(style)}`)
  .replace('<link rel="stylesheet" href="styles.css">', () => `<style>${style}</style>`)
  .replace('<script type="module" src="app.js"></script>', '')
  // The original module was deferred; run the bundled app after the DOM exists.
  .replace('</body>', () => `<script>${script}</script>\n</body>`);

const destination = new URL('dist/KMSwitcher.html', root);
await mkdir(new URL('dist/', root), { recursive: true });
await writeFile(destination, html, 'utf8');
console.log(`Built ${fileURLToPath(destination)} (${Buffer.byteLength(html)} bytes)`);
