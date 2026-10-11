// Pieces shared by the sign-up / sign-in pages: the "or" line, Google and Discord buttons, the
// Turnstile bot check and the safe "next" redirect.
import { useEffect, useRef, useState } from 'react';
import { useT } from '../i18n';
import { AuthError, useAuth, authMode } from '../lib/auth';
import { DiscordLogo, GoogleLogo } from './ui/Icons';

const env = import.meta.env;

/** only same-site paths: "/link?code=..." yes, "//evil.example" or "https://..." no */
export function safeNext(raw: string | null | undefined, fallback = '/account'): string {
  if (!raw || !raw.startsWith('/') || raw.startsWith('//') || raw.startsWith('/\\')) return fallback;
  return raw;
}

function loadScript(src: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const had = document.querySelector<HTMLScriptElement>(`script[src="${src}"]`);
    if (had) {
      if (had.dataset.loaded) resolve();
      else { had.addEventListener('load', () => resolve()); had.addEventListener('error', () => reject(new Error('load'))); }
      return;
    }
    const s = document.createElement('script');
    s.src = src;
    s.async = true;
    s.onload = () => { s.dataset.loaded = '1'; resolve(); };
    s.onerror = () => reject(new Error('load'));
    document.head.appendChild(s);
  });
}

async function sha256Hex(text: string) {
  const buf = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
  return [...new Uint8Array(buf)].map(b => b.toString(16).padStart(2, '0')).join('');
}

function randomNonce() {
  const a = new Uint8Array(24);
  crypto.getRandomValues(a);
  return btoa(String.fromCharCode(...a)).replace(/[+/=]/g, '');
}

declare global {
  interface Window {
    turnstile?: { render(el: HTMLElement, o: Record<string, unknown>): string; reset(id?: string): void; remove(id: string): void };
  }
}

const GOOGLE_KEY = 'hs-google-signin';

/** "Back" from Google or Discord brings this page back exactly as it was left, button still busy */
function useNotBusyOnReturn(setBusy: (busy: boolean) => void) {
  useEffect(() => {
    const back = (e: PageTransitionEvent) => { if (e.persisted) setBusy(false); };
    window.addEventListener('pageshow', back);
    return () => window.removeEventListener('pageshow', back);
  }, [setBusy]);
}

/** Google sign-in by full-page redirect (same as Discord), straight to Google's OpenID endpoint so the
 *  Google page shows our domain, not Supabase's. Google sends an ID token back to /verify in the URL
 *  fragment; finishGoogleSignIn hands it to Supabase with our nonce. (Google's own rendered button
 *  can't be styled, and it ignores clicks when laid invisibly over ours.) */
export function GoogleButton({ next, onError }: { next: string; onError(e: AuthError): void }) {
  const { t, lang } = useT();
  const [busy, setBusy] = useState(false);
  useNotBusyOnReturn(setBusy);
  const ready = !!env.VITE_GOOGLE_CLIENT_ID && authMode === 'supabase';
  return (
    <button type="button" className="btn oauth" disabled={!ready || busy} title={ready ? undefined : t('auth.notConfigured')}
            onClick={async () => {
              setBusy(true);
              try {
                const raw = randomNonce();
                const state = randomNonce();
                sessionStorage.setItem(GOOGLE_KEY, JSON.stringify({ raw, state, next }));
                const q = new URLSearchParams({
                  client_id: env.VITE_GOOGLE_CLIENT_ID, redirect_uri: `${window.location.origin}/verify`,
                  response_type: 'id_token', scope: 'openid email profile', nonce: await sha256Hex(raw),
                  state, prompt: 'select_account', hl: lang,
                });
                window.location.assign(`https://accounts.google.com/o/oauth2/v2/auth?${q}`);
              } catch {
                setBusy(false);
                onError(new AuthError('failed'));
              }
            }}>
      <GoogleLogo />{t('signup.google')}
    </button>
  );
}

/** On /verify: if Google sent us back an ID token, sign in with it. Returns where to go next, null when
 *  the page wasn't opened by Google, and throws when the sign-in failed. */
export async function finishGoogleSignIn(signIn: (idToken: string, nonce: string) => Promise<void>): Promise<string | null> {
  const hash = new URLSearchParams(window.location.hash.slice(1));
  if (!hash.has('state') || !(hash.has('id_token') || hash.has('error'))) return null;
  window.history.replaceState(null, '', window.location.pathname + window.location.search);   // keep the token out of history
  let saved: { raw?: string; state?: string; next?: string } = {};
  try { saved = JSON.parse(sessionStorage.getItem(GOOGLE_KEY) || '{}'); } catch { /* none */ }
  sessionStorage.removeItem(GOOGLE_KEY);
  const token = hash.get('id_token');
  if (!token || !saved.raw || hash.get('state') !== saved.state) throw new AuthError('failed');
  await signIn(token, saved.raw);
  return safeNext(saved.next);
}

export function DiscordButton({ next, onError }: { next: string; onError(e: AuthError): void }) {
  const { t } = useT();
  const auth = useAuth();
  const [busy, setBusy] = useState(false);
  useNotBusyOnReturn(setBusy);
  const ready = authMode === 'supabase';
  return (
    <button type="button" className="btn oauth" disabled={!ready || busy} title={ready ? undefined : t('auth.notConfigured')}
            onClick={async () => {
              setBusy(true);
              try { await auth.signInWithDiscord(next); }
              catch (e) { setBusy(false); onError(e instanceof AuthError ? e : new AuthError('failed')); }
            }}>
      <DiscordLogo />{t('signup.discord')}
    </button>
  );
}

export function OrLine() {
  const { t } = useT();
  return <div className="orline"><span>{t('signup.or')}</span></div>;
}

/** Cloudflare Turnstile (sign-up and password reset). No site key = no check (development). */
export function Turnstile({ onToken }: { onToken(token: string | undefined): void }) {
  const box = useRef<HTMLDivElement>(null);
  const { lang } = useT();
  const key = env.VITE_TURNSTILE_SITE_KEY;
  useEffect(() => {
    if (!key || !box.current) { onToken(undefined); return; }
    let id = '';
    let alive = true;
    loadScript('https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit').then(() => {
      if (!alive || !window.turnstile || !box.current) return;
      id = window.turnstile.render(box.current, {
        sitekey: key, language: lang, appearance: 'interaction-only',
        callback: (tk: string) => onToken(tk), 'expired-callback': () => onToken(undefined),
      });
    }).catch(() => onToken(undefined));
    return () => { alive = false; if (id && window.turnstile) window.turnstile.remove(id); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key, lang]);
  return key ? <div ref={box} className="turnstile" /> : null;
}

export const needsCaptcha = () => !!env.VITE_TURNSTILE_SITE_KEY;

export function authErrorText(t: ReturnType<typeof useT>['t'], e: AuthError | null): string | null {
  if (!e) return null;
  switch (e.code) {
    case 'wrong': return t('login.wrong');
    case 'unconfirmed': return t('login.unconfirmed');
    case 'rate': return t('auth.rate');
    case 'weak': return t('signup.pwHint');
    default: return authMode === 'none' ? t('auth.notConfigured') : t('signup.error');
  }
}
