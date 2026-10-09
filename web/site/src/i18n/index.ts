// English at "/", Thai under "/th/...". One hook gives the language of the current page, t()
// with {named} values, and a helper that keeps links in the same language.
import { useLocation } from 'react-router';
import en from './en.json';
import th from './th.json';

export type Lang = 'en' | 'th';
export type Key = keyof typeof en;

const tables: Record<Lang, Record<string, string>> = { en, th };

export function langOf(pathname: string): Lang {
  return pathname === '/th' || pathname.startsWith('/th/') ? 'th' : 'en';
}

export function translate(lang: Lang, key: Key, vars?: Record<string, string | number>): string {
  const text = tables[lang][key] ?? tables.en[key] ?? key;
  if (!vars) return text;
  return text.replace(/\{(\w+)\}/g, (_, k: string) => (k in vars ? String(vars[k]) : `{${k}}`));
}

/** "/download" in the page's language ("/th/download" on Thai pages). */
export function localPath(lang: Lang, path: string): string {
  if (lang === 'en') return path;
  if (path.startsWith('#')) return path;
  return path === '/' ? '/th' : `/th${path}`;
}

/** The same page in the other language. */
export function otherLangPath(pathname: string): string {
  if (langOf(pathname) === 'th') return pathname.replace(/^\/th(?=\/|$)/, '') || '/';
  return pathname === '/' ? '/th' : `/th${pathname}`;
}

export function useT() {
  const { pathname } = useLocation();
  const lang = langOf(pathname);
  return {
    lang,
    t: (key: Key, vars?: Record<string, string | number>) => translate(lang, key, vars),
    path: (p: string) => localPath(lang, p),
  };
}
