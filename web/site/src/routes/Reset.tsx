// "/reset": the link from the reset email signs the browser in for recovery (Supabase reads the
// code in the URL); then the new password is saved. No session = the link is used or expired.
import { useState } from 'react';
import { Link } from 'react-router';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { z } from 'zod';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { Field, SubmitButton } from '../components/ui/Kit';
import { authErrorText } from '../components/AuthBits';
import { AuthError, useAuth } from '../lib/auth';
import { Seo } from '../lib/seo';
import { password, passwordScore } from '../../shared/schemas';

const form = z.object({ password });

export default function Reset() {
  const { t, path } = useT();
  const auth = useAuth();
  const [done, setDone] = useState(false);
  const [err, setErr] = useState<AuthError | null>(null);
  const { register, handleSubmit, formState, watch } = useForm<{ password: string }>({ resolver: zodResolver(form), mode: 'onChange' });
  const missing = passwordScore(watch('password') ?? '').missing;
  const submit = handleSubmit(async v => {
    setErr(null);
    try { await auth.setPassword(v.password); setDone(true); }
    catch (e) { setErr(e instanceof AuthError ? e : new AuthError('failed')); }
  });

  let body;
  if (auth.status === 'loading') body = <div className="spin mx-auto" aria-hidden="true" />;
  else if (done) body = (
    <div className="flex flex-col gap-4" role="status">
      <h1 className="h4">{t('reset.done')}</h1>
      <Link className="btn solid self-start" to={path('/account')}>{t('verify.continue')}</Link>
    </div>
  );
  else if (auth.status !== 'signedIn') body = (
    <div className="flex flex-col gap-4">
      <h1 className="h4">{t('reset.h1')}</h1>
      <p className="alert" role="alert">{t('reset.invalid')}</p>
      <Link className="btn self-start" to="/forgot">{t('forgot.submit')}</Link>
    </div>
  );
  else body = (
    <form className="flex flex-col gap-4" noValidate onSubmit={submit}>
      <h1 className="h4">{t('reset.h1')}</h1>
      <Field label={t('signup.password')} type="password" autoComplete="new-password" placeholder={t('signup.passwordPh')}
             hint={missing > 0 ? t('signup.pwMore', { n: missing }) : t('signup.pwHint')} {...register('password')} />
      {err && <p className="alert" role="alert">{authErrorText(t, err)}</p>}
      <SubmitButton busy={formState.isSubmitting} disabled={!formState.isValid} className="solid big">{t('reset.submit')}</SubmitButton>
    </form>
  );

  return (
    <PageShell nav="login">
      <Seo title={`${t('reset.h1')} · HEARASIDE`} noindex />
      <div className="wrap auth-center"><div className="card auth-card narrow">{body}</div></div>
    </PageShell>
  );
}
