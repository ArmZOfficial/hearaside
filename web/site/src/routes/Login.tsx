// "/login" (3.11 Login). A 440 px card. Coming from a plug-in (next=/link?code=...) shows which
// plug-in and computer is waiting. A wrong email or password never says which one was wrong.
import { useEffect, useState } from 'react';
import { Link, useNavigate, useSearchParams } from 'react-router';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { Avatar, Field, SubmitButton } from '../components/ui/Kit';
import { DiscordButton, GoogleButton, OrLine, authErrorText, safeNext } from '../components/AuthBits';
import { AuthError, useAuth } from '../lib/auth';
import { call, useMe } from '../lib/api';
import { Seo } from '../lib/seo';
import { loginForm, normalizeUserCode, type LoginForm } from '../../shared/schemas';

const PLUGIN_NAMES: Record<string, string> = { hub: 'HEARASIDE Hub', track: 'HEARASIDE Track', app: 'HEARASIDE App Audio' };

/** "Signing in to connect HEARASIDE Hub on ArmZ-PC", when the sign-in was started by a plug-in */
function usePluginBanner(next: string) {
  const [info, setInfo] = useState<{ plugin: string; computer: string } | null>(null);
  useEffect(() => {
    if (!next.startsWith('/link')) return;
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
  const navigate = useNavigate();
  const [params] = useSearchParams();
  const next = safeNext(params.get('next'), '');
  const fromPlugin = next.startsWith('/link');
  const banner = usePluginBanner(next);
  const me = useMe();
  const [err, setErr] = useState<AuthError | null>(null);
  const { register, handleSubmit, formState } = useForm<LoginForm>({ resolver: zodResolver(loginForm), defaultValues: { email: '', password: '' } });

  const submit = handleSubmit(async v => {
    setErr(null);
    try { await auth.signIn(v.email, v.password); }
    catch (e) { setErr(e instanceof AuthError ? e : new AuthError('failed')); }
  });

  const done = auth.status === 'signedIn';
  const name = me.data?.displayName || me.data?.handle || '';

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
          {done ? (
            <div className="flex flex-col items-center gap-3 text-center py-2" role="status">
              <Avatar name={name || '?'} url={me.data?.avatarUrl} size={72} />
              <h1 className="h4">{t('login.signedInAs', { name: name || '…' })}</h1>
              {me.data && <span className="text-sm muted">@{me.data.handle}</span>}
              <button type="button" className="btn solid big w-full mt-2" onClick={() => navigate(next || path('/account'))}>
                {fromPlugin ? t('login.continueHub') : t('login.toAccount')}
              </button>
            </div>
          ) : (
            <form className="flex flex-col gap-4" noValidate onSubmit={submit}>
              <h1 className="h4">{t('login.h1')}</h1>
              <Field label={t('login.email')} type="email" autoComplete="email" inputMode="email" {...register('email')} />
              <Field label={t('login.password')} type="password" autoComplete="current-password"
                     right={<Link to="/forgot" className="text-[13px]">{t('login.forgot')}</Link>} {...register('password')} />
              {err && <p className="alert" role="alert">{authErrorText(t, err)}</p>}
              <SubmitButton busy={formState.isSubmitting} className="solid big">{t('login.submit')}</SubmitButton>
              <OrLine />
              <GoogleButton onDone={() => {}} onError={setErr} />
              <DiscordButton next={`/login${next ? `?next=${encodeURIComponent(next)}` : ''}`} onError={setErr} />
            </form>
          )}
        </div>
        <p className="text-sm muted text-center m-0">{t('login.noAccountNeeded')}</p>
      </div>
    </PageShell>
  );
}
