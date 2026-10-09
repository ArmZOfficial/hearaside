// "/forgot": always the same answer, whether or not the email has an account.
import { useState } from 'react';
import { Link } from 'react-router';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { z } from 'zod';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { Field, SubmitButton } from '../components/ui/Kit';
import { Turnstile, needsCaptcha } from '../components/AuthBits';
import { useAuth } from '../lib/auth';
import { Seo } from '../lib/seo';
import { email } from '../../shared/schemas';

const form = z.object({ email });

export default function Forgot() {
  const { t } = useT();
  const auth = useAuth();
  const [sent, setSent] = useState(false);
  const [captcha, setCaptcha] = useState<string | undefined>();
  const { register, handleSubmit, formState } = useForm<{ email: string }>({ resolver: zodResolver(form), mode: 'onChange' });
  const submit = handleSubmit(async v => {
    await auth.forgot(v.email, captcha).catch(() => {});
    setSent(true);
  });
  return (
    <PageShell nav="login">
      <Seo title={`${t('forgot.h1')} · HEARASIDE`} noindex />
      <div className="wrap auth-center">
        <div className="card auth-card narrow">
          {sent ? (
            <div className="flex flex-col gap-4" role="status">
              <h1 className="h4">{t('forgot.h1')}</h1>
              <p className="m-0 text-[15px] leading-relaxed text-ink2">{t('forgot.sent')}</p>
              <Link className="btn self-start" to="/login">{t('forgot.back')}</Link>
            </div>
          ) : (
            <form className="flex flex-col gap-4" noValidate onSubmit={submit}>
              <h1 className="h4">{t('forgot.h1')}</h1>
              <p className="m-0 text-[15px] leading-relaxed text-ink2">{t('forgot.lead')}</p>
              <Field label={t('login.email')} type="email" autoComplete="email" inputMode="email" {...register('email')} />
              <Turnstile onToken={setCaptcha} />
              <SubmitButton busy={formState.isSubmitting} disabled={!formState.isValid || (needsCaptcha() && !captcha)} className="solid big">
                {t('forgot.submit')}
              </SubmitButton>
              <Link to="/login" className="text-sm text-center">{t('forgot.back')}</Link>
            </form>
          )}
        </div>
      </div>
    </PageShell>
  );
}
