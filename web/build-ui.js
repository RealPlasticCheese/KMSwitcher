const targets = ['controller', 'receiver', 'all'];

export function repositoryFolder(page) {
  const pageURL = new URL(page), directory = new URL('.', pageURL);
  if (pageURL.protocol !== 'file:' || !/\/web\/(?:dist\/)?$/i.test(directory.pathname)) return '';
  const url = new URL(/\/dist\/$/i.test(directory.pathname) ? '../../' : '../', pageURL);
  const path = decodeURIComponent(url.pathname).replace(/^\/([a-z]:\/)/i, '$1').replaceAll('/', '\\').replace(/\\$/, '');
  return url.hostname ? `\\\\${url.hostname}${path}` : path;
}

export function buildPlan(folder, target, resetConfig = false) {
  const root = folder.trim().replace(/\\+$/, '');
  if (!/^(?:[a-z]:\\|\\\\[^\\]+\\[^\\]+)/i.test(root) || /[%!"^&|<>\x00-\x1f]/.test(root)) {
    throw new Error('Enter an absolute Windows repository folder without shell characters (% ! " ^ & | < >).');
  }
  if (!targets.includes(target)) throw new Error('Choose controller, receivers or both.');
  const script = `${root}\\web\\build-firmware.ps1`;
  const options = `-Target ${target}${resetConfig ? ' -ResetConfig' : ''}`;
  const prefix = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File';
  return {
    command: `${prefix} '${script.replaceAll("'", "''")}' ${options}`,
    launcher: ['@echo off', 'chcp 65001 >nul', `${prefix} "${script}" ${options}`,
      'set "KMS_BUILD_EXIT=%errorlevel%"', 'echo.', 'pause', 'exit /b %KMS_BUILD_EXIT%', ''].join('\r\n'),
    outputs: (target === 'all' ? targets.slice(0, 2) : [target]).map(role => ({ role,
      image: `${root}\\build\\${role}\\kmswitcher_${role}.bin`,
      manifest: `${root}\\build\\${role}\\kmswitcher_${role}.json` }))
  };
}

export function createBuildUI({ demo, download }) {
  const $ = id => document.getElementById(id);
  let plan;
  $('build-root').value = repositoryFolder(location.href);
  function render() {
    try {
      plan = buildPlan($('build-root').value, $('build-target').value, $('build-reset').checked);
      $('build-command').textContent = plan.command;
      $('build-download').disabled = demo; $('build-copy').disabled = demo;
      $('build-results').replaceChildren();
      for (const output of plan.outputs) {
        const row = document.createElement('div'); row.className = 'build-result';
        const title = document.createElement('strong'); title.textContent = `${output.role === 'controller' ? 'Controller' : 'Receivers'} build files`;
        const paths = document.createElement('pre'); paths.textContent = `${output.image}\n${output.manifest}`;
        row.append(title, paths); $('build-results').append(row);
      }
      $('build-status').textContent = demo ? 'Demo mode: build launchers are disabled. Exit demo to prepare a real firmware build.' :
        'Open the downloaded .cmd file to start the build. The browser cannot run scripts or observe build completion.';
    } catch (error) {
      plan = null; $('build-command').textContent = ''; $('build-results').replaceChildren();
      $('build-download').disabled = true; $('build-copy').disabled = true;
      $('build-status').textContent = error.message;
    }
  }
  for (const id of ['build-root', 'build-target', 'build-reset']) $(id).addEventListener('input', render);
  $('build-download').addEventListener('click', () => {
    if (!plan || demo) return;
    download(`kmswitcher-build-${$('build-target').value}.cmd`, plan.launcher, 'application/octet-stream');
    $('build-status').textContent = 'Launcher downloaded. Open it to build, wait for the success message, then choose the generated .bin and .json below.';
  });
  $('build-copy').addEventListener('click', async () => {
    if (!plan || demo) return;
    try {
      await navigator.clipboard.writeText(plan.command);
      $('build-status').textContent = 'Command copied. Paste it into PowerShell, then choose the generated files after the build succeeds.';
    } catch {
      const selection = getSelection(), range = document.createRange(); range.selectNodeContents($('build-command'));
      selection.removeAllRanges(); selection.addRange(range);
      $('build-status').textContent = 'Clipboard access is unavailable. The command is selected; press Ctrl+C and paste it into PowerShell.';
    }
  });
  render();
}
