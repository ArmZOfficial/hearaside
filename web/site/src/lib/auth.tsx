// Sign-in on the website. Two back ends behind one interface:
//   supabase  Supabase Auth (email + password, Google ID token, Discord OAuth): the real site
//   memory    the in-memory accounts of `npm run dev` / tests (VITE_AUTH_MODE=memory), no Supabase needed
// Pages only ever see `useAuth()`. The plug-ins never use any of this (they use device codes, A4).
import { createContext, useContext, useEffect, useMemo, useState, type ReactNode } from 'react';
import type { SupabaseClient } from '@supabase/supabase-js';

export type AuthStatus = 'loading' | 'signedOut' | 'signedIn';

export interface AuthApi {
  configured: boolean;
  status: AuthStatus;
  token: string | null;
  signUp(a: { email: string; password: string; displayName: string; locale: string; captchaToken?: string }): Promise<void>;
  signIn(email: string, password: string): Promise<void>;
  signInWithGoogle(idToken: string, nonce: string): Promise<void>;
  signInWithDiscord(next: string): Promise<void>;
  linkDiscord(): Promise<void>;
  unlinkProvider(provider: string): Promise<void>;
  forgot(email: string, captchaToken?: string): Promise<void>;
  setPassword(password: string): Promise<void>;
  changeEmail(email: string): Promise<void>;
  signOut(everywhere?: boolean): Promise<void>;
}

export class AuthError extends Error {
  constructor(public code: 'wrong' | 'unconfirmed' | 'weak' | 'rate' | 'failed') {
    super(code);
  }
}

const env = import.meta.env;
const MODE: 'supabase' | 'memory' | 'none' =
  env.VITE_AUTH_MODE === 'memory' ? 'memory' : env.VITE_SUPABASE_URL && env.VITE_SUPABASE_PUBLISHABLE_KEY ? 'supabase' : 'none';

let supabasePromise: Promise<SupabaseClient> | null = null;
/** loaded on demand so the marketing pages don't carry supabase-js */
export function supabase(): Promise<SupabaseClient> {
  supabasePromise ??= import('@supabase/supabase-js').then(({ createClient }) =>
    createClient(env.VITE_SUPABASE_URL, env.VITE_SUPABASE_PUBLISHABLE_KEY, {
      auth: { flowType: 'pkce', detectSessionInUrl: true, persistSession: true, autoRefreshToken: true },
    }),
  );
  return supabasePromise;
}

const origin = () => (typeof window === 'undefined' ? '' : window.location.origin);

function mapSupabaseError(e: { message?: string; status?: number; code?: string } | null): never {
  const code = e?.code ?? '';
  if (code === 'invalid_credentials') throw new AuthError('wrong');
  if (code === 'email_not_confirmed') throw new AuthError('unconfirmed');
  if (code === 'weak_password') throw new AuthError('weak');
  if (e?.status === 429 || code.includes('rate_limit')) throw new AuthError('rate');
  throw new AuthError('failed');
}

// --- memory back end (development only) ---------------------------------------------------------

const DEV_KEY = 'hs-dev-session';
async function devPost(path: string, body: unknown, token?: string | null) {
  const r = await fetch(`/api/v1/dev/auth/${path}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', ...(token ? { Authorization: `Bearer ${token}` } : {}) },
    body: JSON.stringify(body),
  });
  const j = await r.json().catch(() => ({}));
  if (r.status === 401) throw new AuthError('wrong');
  if (r.status === 429) throw new AuthError('rate');
  if (!r.ok) throw new AuthError('failed');
  return j as { token?: string };
}

// ------------------------------------------------------------------------------------------------

const Ctx = createContext<AuthApi | null>(null);

export function AuthProvider({ children }: { children: ReactNode }) {
  const [status, setStatus] = useState<AuthStatus>('loading');
  const [token, setToken] = useState<string | null>(null);

  useEffect(() => {
    if (MODE === 'none') { setStatus('signedOut'); return; }
    if (MODE === 'memory') {
      let t: string | null = null;
      try { t = sessionStorage.getItem(DEV_KEY); } catch { /* blocked storage */ }
      setToken(t);
      setStatus(t ? 'signedIn' : 'signedOut');
      return;
    }
    let off = () => {};
    let alive = true;
    supabase().then(sb => {
      if (!alive) return;
      sb.auth.getSession().then(({ data }) => {
        setToken(data.session?.access_token ?? null);
        setStatus(data.session ? 'signedIn' : 'signedOut');
      });
      const { data } = sb.auth.onAuthStateChange((_event, session) => {
        setToken(session?.access_token ?? null);
        setStatus(session ? 'signedIn' : 'signedOut');
      });
      off = () => data.subscription.unsubscribe();
    });
    return () => { alive = false; off(); };
  }, []);

  const api = useMemo<AuthApi>(() => {
    const devSet = (t: string | null) => {
      try { if (t) sessionStorage.setItem(DEV_KEY, t); else sessionStorage.removeItem(DEV_KEY); } catch { /* ignore */ }
      setToken(t);
      setStatus(t ? 'signedIn' : 'signedOut');
    };
    const notConfigured = async () => { throw new AuthError('failed'); };
    if (MODE === 'none') {
      return {
        configured: false, status, token,
        signUp: notConfigured, signIn: notConfigured, signInWithGoogle: notConfigured, signInWithDiscord: notConfigured,
        linkDiscord: notConfigured, unlinkProvider: notConfigured, forgot: notConfigured, setPassword: notConfigured,
        changeEmail: notConfigured, signOut: async () => {},
      };
    }
    if (MODE === 'memory') {
      return {
        configured: true, status, token,
        async signUp(a) { await devPost('signup', a); },
        async signIn(email, password) { devSet((await devPost('login', { email, password })).token ?? null); },
        signInWithGoogle: notConfigured,
        signInWithDiscord: notConfigured,
        linkDiscord: notConfigured,
        unlinkProvider: notConfigured,
        async forgot() { /* same answer whether or not the email exists */ },
        async setPassword(password) { await devPost('password', { password }, token); },
        async changeEmail(email) { await devPost('email', { email }, token); },
        async signOut() { await devPost('logout', {}, token).catch(() => {}); devSet(null); },
      };
    }
    return {
      configured: true, status, token,
      async signUp({ email, password, displayName, locale, captchaToken }) {
        const sb = await supabase();
        const { error } = await sb.auth.signUp({
          email, password,
          options: { emailRedirectTo: `${origin()}/verify`, data: { display_name: displayName, locale }, captchaToken },
        });
        if (error) mapSupabaseError(error);
      },
      async signIn(email, password) {
        const sb = await supabase();
        const { error } = await sb.auth.signInWithPassword({ email, password });
        if (error) mapSupabaseError(error);
      },
      async signInWithGoogle(idToken, nonce) {
        const sb = await supabase();
        const { error } = await sb.auth.signInWithIdToken({ provider: 'google', token: idToken, nonce });
        if (error) mapSupabaseError(error);
      },
      async signInWithDiscord(next) {
        const sb = await supabase();
        const { error } = await sb.auth.signInWithOAuth({
          provider: 'discord',
          options: { redirectTo: `${origin()}/verify?next=${encodeURIComponent(next)}`, scopes: 'identify email' },
        });
        if (error) mapSupabaseError(error);
      },
      async linkDiscord() {
        const sb = await supabase();
        const { error } = await sb.auth.linkIdentity({ provider: 'discord', options: { redirectTo: `${origin()}/account/security` } });
        if (error) mapSupabaseError(error);
      },
      async unlinkProvider(provider) {
        const sb = await supabase();
        const { data } = await sb.auth.getUserIdentities();
        const identity = data?.identities.find(i => i.provider === provider);
        if (!identity) return;
        const { error } = await sb.auth.unlinkIdentity(identity);
        if (error) mapSupabaseError(error);
      },
      async forgot(email, captchaToken) {
        const sb = await supabase();
        // the answer is the same whether or not the email has an account
        await sb.auth.resetPasswordForEmail(email, { redirectTo: `${origin()}/reset`, captchaToken });
      },
      async setPassword(password) {
        const sb = await supabase();
        // the date shows as "Changed 2 weeks ago" in Account › Sign-in and security
        const { error } = await sb.auth.updateUser({ password, data: { password_changed_at: new Date().toISOString() } });
        if (error) mapSupabaseError(error);
      },
      async changeEmail(email) {
        const sb = await supabase();
        const { error } = await sb.auth.updateUser({ email }, { emailRedirectTo: `${origin()}/verify` });
        if (error) mapSupabaseError(error);
      },
      async signOut(everywhere = false) {
        const sb = await supabase();
        await sb.auth.signOut({ scope: everywhere ? 'global' : 'local' });
      },
    };
  }, [status, token]);

  return <Ctx.Provider value={api}>{children}</Ctx.Provider>;
}

export function useAuth(): AuthApi {
  const v = useContext(Ctx);
  if (!v) throw new Error('useAuth outside AuthProvider');
  return v;
}

export const authMode = MODE;
