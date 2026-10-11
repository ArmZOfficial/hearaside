// Keeping an open tab on the current build. Every deploy replaces the hashed files in /assets, so a tab
// opened before it can't load the next page's code any more (a blank page). We reload instead:
// - when a page's code fails to load (the build changed under us, or the network dropped a request);
// - when /version.json names a newer build: checked on every page change (then loaded at the new address),
//   and when the tab is shown again (reloaded then unless something has been typed into it).

declare const __BUILD_ID__: string;

const RELOAD_KEY = 'hs-reloaded-at';
let newer = false;

/** reload, but not twice within 30 s (a build that really is broken would loop) */
export function reloadOnce(): boolean {
  try {
    const last = Number(sessionStorage.getItem(RELOAD_KEY) || 0);
    if (Date.now() - last < 30_000) return false;
    sessionStorage.setItem(RELOAD_KEY, String(Date.now()));
  } catch { /* storage blocked: reload anyway */ }
  window.location.reload();
  return true;
}

async function checkVersion() {
  try {
    const r = await fetch('/version.json', { cache: 'no-store' });
    if (!r.ok) return;
    const { id } = await r.json() as { id?: string };
    if (id && id !== __BUILD_ID__) newer = true;
  } catch { /* offline: try again later */ }
}

function typedSomething() {
  return [...document.querySelectorAll<HTMLInputElement | HTMLTextAreaElement>('input, textarea')]
    .some(el => !['hidden', 'checkbox', 'radio', 'submit', 'button'].includes(el.type) && el.value !== '');
}

export function watchForUpdates() {
  // a dynamic import() Vite couldn't load
  window.addEventListener('vite:preloadError', e => { if (reloadOnce()) e.preventDefault(); });
  const look = async () => {
    await checkVersion();
    if (newer && document.visibilityState === 'visible' && !typedSomething()) reloadOnce();
  };
  document.addEventListener('visibilitychange', look);
  window.addEventListener('pageshow', e => { if (e.persisted) void look(); });   // back from Google / Discord
  window.setInterval(checkVersion, 5 * 60_000);
}

/** called when the route changes: if a newer build is live, load it now, at the new address */
export async function reloadIfNewer() {
  if (!newer) await checkVersion();
  if (newer) reloadOnce();
}

/** React.lazy that reloads the page when the code can't be fetched instead of crashing to a blank page */
export function loadPage<T>(load: () => Promise<{ default: T }>) {
  return () => load().then(
    m => m ?? new Promise<never>(() => {}),   // undefined: the preload listener above is already reloading
    err => { if (reloadOnce()) return new Promise<never>(() => {}); throw err; },
  );
}
