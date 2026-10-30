// "/login" (3.11 Login). A 440 px card. Coming from a plug-in (next=/link?code=...) shows which
// plug-in and computer is waiting. A wrong email or password never says which one was wrong.
import { useEffect, useState } from 'react';
import { Link, Navigate, useSearchParams } from 'react-router';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { Field, SubmitButton } from '../components/ui/Kit';
import { DiscordButton, GoogleButton, OrLine, Turnstile, authErrorText, needsCaptcha, safeNext } from '../components/AuthBits';
import { AuthError, useAuth } from '../lib/auth';
import { call } from '../lib/api';
import { Seo } from '../lib/seo';
import { loginForm, normalizeUserCode, type LoginForm } from '../../shared/schemas';

const PLUGIN_NAMES: Record<string, string> = { hub: 'HEARASIDE Hub', track: 'HEARASIDE Track', app: 'HEARASIDE App Audio' };

/** "Signing in to connect HEARASIDE Hub on ArmZ-PC", when the sign-in was started by a plug-in */
function usePluginBanner(next: string) {
  const [info, setInfo] = useState<{ plugin: string; computer: string } | null>(null);
  useEffect(() => {
    if (!/^\/(th\/)?link(?:\?|$)/.test(next)) return;
    const code = normalizeUserCode(new URLSearchParams(next.split('?')[1] ?? '').get('code') ?? '');
    if (!code) return;
    call<{ plugin: string; computer: string }>(null, 'POST', '/device/peek', { user_code: code })
      .then(setInfo).catch(() => setInfo(null));
  }, [next]);
  return info;
}

export default function Login() {
  const { t, path } = useT();
  const auth = useAuth();
  const [params] = useSearchParams();
  const next = safeNext(params.get('next'), '');
  const fromPlugin = /^\/(th\/)?link(?:\?|$)/.test(next);
  const banner = usePluginBanner(next);
  const [err, setErr] = useState<AuthError | null>(null);
  const [captcha, setCaptcha] = useState<string | undefined>();
  const [challenge, setChallenge] = useState(0);   // a Turnstile token works once: a new widget after every failed try
  const { register, handleSubmit, formState, watch } = useForm<LoginForm>({ resolver: zodResolver(loginForm), defaultValues: { email: '', password: '' } });

  const submit = handleSubmit(async v => {
    setErr(null);
    try { await auth.signIn(v.email, v.password, captcha); }
    catch (e) { setErr(e instanceof AuthError ? e : new AuthError('failed')); setCaptcha(undefined); setChallenge(n => n + 1); }
  });

  // a wrong email or password never says which (nobody can test whose email has an account here), so after a
  // failed sign-in the card offers to create an account with that email instead
  const wrong = err?.code === 'wrong';
  const typed = watch('email')?.trim() ?? '';
  const signupTo = `${path('/signup')}${typed || next ? '?' : ''}${new URLSearchParams({ ...(typed ? { email: typed } : {}), ...(next ? { next } : {}) })}`;
  if (auth.status === 'signedIn') return <Navigate to={next || path('/account')} replace />;

  return (
    <PageShell nav="login">
      <Seo title={`${t('login.h1')} · HEARASIDE`} noindex />
      <div className="wrap auth-center">
        {fromPlugin && banner && (
          <div className="plugin-banner rise" role="status">
            {t('login.fromPlugin', { plugin: PLUGIN_NAMES[banner.plugin] ?? 'HEARASIDE', computer: banner.computer })}
          </div>
        )}
        <div className="card auth-card narrow">
            <form className="flex flex-col gap-4" noValidate onSubmit={submit}>
              <h1 className="h4">{t('login.h1')}</h1>
              <Field label={t('login.email')} type="email" autoComplete="email" inputMode="email" error={formState.errors.email && t('form.email')} {...register('email')} />
              <Field label={t('login.password')} type="password" autoComplete="current-password"
                     error={formState.errors.password && t('form.required')}
                     right={<Link to={`${path('/forgot')}${next ? `?next=${encodeURIComponent(next)}` : ''}`} className="text-[13px]">{t('login.forgot')}</Link>} {...register('password')} />
              {err && (
                <div className="flex flex-col gap-1" role="alert">
                  <p className="alert">{authErrorText(t, err)}</p>
                  {wrong && <p className="m-0 text-sm text-ink2">{t('login.wrongMaybeNew')}</p>}
                </div>
              )}
              <Turnstile key={challenge} onToken={setCaptcha} />
              <SubmitButton busy={formState.isSubmitting} disabled={needsCaptcha() && !captcha} className={`${wrong ? '' : 'solid'} big`}>{t('login.submit')}</SubmitButton>
              <OrLine />
              <GoogleButton next={`${path('/login')}${next ? `?next=${encodeURIComponent(next)}` : ''}`} onError={setErr} />
              <DiscordButton next={`${path('/login')}${next ? `?next=${encodeURIComponent(next)}` : ''}`} onError={setErr} />
              <div className="auth-switch">
                <span>{t('login.newHere')}</span>
                <Link className={`btn big w-full ${wrong ? 'solid' : 'accent-outline'}`} to={signupTo}>{t('login.createCta')}</Link>
              </div>
            </form>
        </div>
        <p className="text-sm muted text-center m-0">{t('login.noAccountNeeded')}</p>
      </div>
    </PageShell>
  );
}
