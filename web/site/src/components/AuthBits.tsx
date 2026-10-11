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

interface GoogleId {
  initialize(o: Record<string, unknown>): void;
  renderButton(el: HTMLElement, o: Record<string, unknown>): void;
}
declare global {
  interface Window {
    google?: { accounts: { id: GoogleId } };
    turnstile?: { render(el: HTMLElement, o: Record<string, unknown>): string; reset(id?: string): void; remove(id: string): void };
  }
}

/** Our own button (same look as Discord's) with Google Identity Services' button laid invisibly on top,
 *  so the click still lands on Google's: the ID token goes to Supabase with our nonce and the Google
 *  page shows our domain. Google's button can't be restyled, which is why it isn't shown. */
export function GoogleButton({ onDone, onError }: { onDone(): void; onError(e: AuthError): void }) {
  const { t, lang } = useT();
  const auth = useAuth();
  const box = useRef<HTMLDivElement>(null);
  const [failed, setFailed] = useState(false);
  const ready = !!env.VITE_GOOGLE_CLIENT_ID && authMode === 'supabase';

  useEffect(() => {
    if (!ready || !box.current) return;
    let alive = true;
    const el = box.current;
    // Google's button is at most 400 px wide and 40 tall: stretch it over the whole of ours
    let drawn = 0;
    const fit = () => { if (drawn) el.style.setProperty('--gsi-sx', String(el.clientWidth / drawn)); };
    const ro = new ResizeObserver(fit);
    ro.observe(el);
    (async () => {
      try {
        await loadScript(`https://accounts.google.com/gsi/client?hl=${lang}`);
        if (!alive || !window.google) return;
        const raw = randomNonce();
        const hashed = await sha256Hex(raw);
        window.google.accounts.id.initialize({
          client_id: env.VITE_GOOGLE_CLIENT_ID,
          nonce: hashed,
          callback: async (r: { credential: string }) => {
            try { await auth.signInWithGoogle(r.credential, raw); onDone(); }
            catch (e) { onError(e instanceof AuthError ? e : new AuthError('failed')); }
          },
        });
        el.innerHTML = '';
        drawn = Math.min(400, Math.max(200, el.clientWidth));
        window.google.accounts.id.renderButton(el, {
          theme: 'outline', shape: 'rectangular', text: 'continue_with', size: 'large', width: drawn, locale: lang,
        });
        fit();
      } catch {
        if (alive) setFailed(true);
      }
    })();
    return () => { alive = false; ro.disconnect(); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready, lang]);

  if (!ready) {
    return (
      <button type="button" className="btn oauth" disabled title={t('auth.notConfigured')}>
        <GoogleLogo />{t('signup.google')}
      </button>
    );
  }
  if (failed) {   // Google's script was blocked (Brave Shields, an ad blocker): say so instead of a dead button
    return (
      <div className="flex flex-col gap-2">
        <button type="button" className="btn oauth" aria-describedby="gsi-blocked" onClick={() => document.getElementById('gsi-blocked')?.focus()}>
          <GoogleLogo />{t('signup.google')}
        </button>
        <p id="gsi-blocked" tabIndex={-1} className="m-0 text-[13px] leading-snug text-ink2">{t('login.googleBlocked')}</p>
      </div>
    );
  }
  return (
    <div className="gsi-wrap">
      <button type="button" className="btn oauth" tabIndex={-1} aria-hidden="true"><GoogleLogo />{t('signup.google')}</button>
      <div ref={box} className="gsi-overlay" aria-label={t('signup.google')} />
    </div>
  );
}

export function DiscordButton({ next, onError }: { next: string; onError(e: AuthError): void }) {
  const { t } = useT();
  const auth = useAuth();
  const [busy, setBusy] = useState(false);
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
