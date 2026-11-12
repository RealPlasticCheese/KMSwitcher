const storageKey = 'kmswitcher-theme';
const themes = new Set(['light', 'dark', 'system']);

export function initializeTheme(win) {
  const selector = win.document.getElementById('theme');
  const system = win.matchMedia?.('(prefers-color-scheme: dark)');
  let preference = 'system';
  try {
    const saved = win.localStorage.getItem(storageKey);
    if (themes.has(saved)) preference = saved;
  } catch { /* Storage can be unavailable for local files or browser policy. */ }

  function apply() {
    win.document.documentElement.dataset.theme = preference === 'system'
      ? (system?.matches ? 'dark' : 'light') : preference;
  }

  selector.value = preference;
  selector.addEventListener('change', () => {
    preference = themes.has(selector.value) ? selector.value : 'system';
    selector.value = preference;
    apply();
    try { win.localStorage.setItem(storageKey, preference); }
    catch { /* The selected theme still works for this page. */ }
  });
  system?.addEventListener('change', apply);
  apply();
}
