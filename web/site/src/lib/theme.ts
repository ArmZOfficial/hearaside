// Theme: "system" follows the OS; light / dark are remembered in this browser only.
import { createContext, createElement, useContext, useEffect, useState, type ReactNode } from 'react';

export type ThemeMode = 'system' | 'light' | 'dark';

function read(): ThemeMode {
  try {
    const t = localStorage.getItem('hs-theme');
    return t === 'light' || t === 'dark' ? t : 'system';
  } catch {
    return 'system';
  }
}

function apply(mode: ThemeMode) {
  const root = document.documentElement;
  const change = () => {
    if (mode === 'system') root.removeAttribute('data-theme');
    else root.setAttribute('data-theme', mode);
  };
  // cross-fade where the browser can (View Transitions), plain switch elsewhere
  const doc = document as Document & { startViewTransition?: (cb: () => void) => unknown };
  const reduce = window.matchMedia?.('(prefers-reduced-motion: reduce)').matches;
  if (doc.startViewTransition && !reduce) doc.startViewTransition(change);
  else change();
}

const ThemeContext = createContext<{ mode: ThemeMode; cycle(): void } | null>(null);

export function ThemeProvider({ children }: { children: ReactNode }) {
  const [mode, setMode] = useState<ThemeMode>('system');
  useEffect(() => {
    setMode(read());
    const sync = (event: StorageEvent) => {
      if (event.key !== 'hs-theme' && event.key !== null) return;
      const next = read();
      setMode(next);
      apply(next);
    };
    window.addEventListener('storage', sync);
    return () => window.removeEventListener('storage', sync);
  }, []);
  const cycle = () => {
    const next: ThemeMode = mode === 'system' ? 'light' : mode === 'light' ? 'dark' : 'system';
    setMode(next);
    try {
      if (next === 'system') localStorage.removeItem('hs-theme');
      else localStorage.setItem('hs-theme', next);
    } catch { /* private window: this page only */ }
    apply(next);
  };
  return createElement(ThemeContext.Provider, { value: { mode, cycle } }, children);
}

export function useTheme() {
  const theme = useContext(ThemeContext);
  if (!theme) throw new Error('useTheme requires ThemeProvider');
  return theme;
}

/** Whether the page is dark right now (data-theme, else the OS), following every change: the theme button
 *  in the nav, another tab, the OS switching at night. For widgets drawn outside our CSS (Google's button). */
export function useResolvedDark() {
  const get = () => {
    const t = document.documentElement.getAttribute('data-theme');
    return t ? t === 'dark' : !!window.matchMedia?.('(prefers-color-scheme: dark)').matches;
  };
  const [dark, setDark] = useState(false);
  useEffect(() => {
    const on = () => setDark(get());
    on();
    const mo = new MutationObserver(on);
    mo.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    const mq = window.matchMedia?.('(prefers-color-scheme: dark)');
    mq?.addEventListener('change', on);
    return () => { mo.disconnect(); mq?.removeEventListener('change', on); };
  }, []);
  return dark;
}
