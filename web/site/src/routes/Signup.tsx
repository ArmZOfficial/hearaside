// "/signup" (3.11 Signup, 3.11.1 #9). Email + password with a strength bar, or Google / Discord.
// After sending: "Check your email". Never says whether an email already has an account.
import { useState } from 'react';
import { Link, useSearchParams } from 'react-router';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { motion, useReducedMotion } from 'motion/react';
import { Mail } from 'lucide-react';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { Check, Field, SubmitButton } from '../components/ui/Kit';
import { DiscordButton, GoogleButton, OrLine, Turnstile, authErrorText, needsCaptcha, safeNext } from '../components/AuthBits';
import { AuthError, useAuth } from '../lib/auth';
import { Seo } from '../lib/seo';
import { passwordScore, signupForm, type SignupForm } from '../../shared/schemas';
import { useNavigate } from 'react-router';

function Strength({ pw }: { pw: string }) {
  const { t } = useT();
  const reduce = useReducedMotion();
  const { score, missing } = passwordScore(pw);
  const filled = pw.length === 0 ? 0 : missing > 0 ? 1 : score + 1;
  const hint = pw.length === 0 ? t('signup.pwHint') : missing > 0 ? t('signup.pwMore', { n: missing }) : t(`signup.pw.${score}` as 'signup.pw.0');
  return (
    <div className="flex flex-col gap-1.5" aria-live="polite">
      <div className="flex gap-1.5" aria-hidden="true">
        {[0, 1, 2, 3].map(i => (
          <span key={i} className="bar">
            <motion.i initial={false} animate={{ scaleX: i < filled ? 1 : 0 }}
                      transition={reduce ? { duration: 0 } : { type: 'spring', stiffness: 380, damping: 30, delay: i * 0.04 }} />
          </span>
        ))}
      </div>
      <span className="field-hint">{hint}</span>
    </div>
  );
}

export default function Signup() {
  const { t, lang, path } = useT();
  const auth = useAuth();
  const navigate = useNavigate();
  const [params] = useSearchParams();
  const next = safeNext(params.get('next'));
  const [sentTo, setSentTo] = useState<string | null>(null);
  const [err, setErr] = useState<AuthError | null>(null);
  const [captcha, setCaptcha] = useState<string | undefined>();
  const { register, handleSubmit, watch, formState, reset } = useForm<SignupForm>({
    resolver: zodResolver(signupForm), mode: 'onChange',
    defaultValues: { displayName: '', email: '', password: '', agree: false as unknown as true },
  });
  const pw = watch('password') ?? '';
  const canSend = formState.isValid && (!needsCaptcha() || !!captcha);

  const submit = handleSubmit(async v => {
    setErr(null);
    try {
      await auth.signUp({ email: v.email, password: v.password, displayName: v.displayName, locale: lang, captchaToken: captcha });
      setSentTo(v.email);
    } catch (e) {
      setErr(e instanceof AuthError ? e : new AuthError('failed'));
    }
  });

  return (
    <PageShell nav="signup">
      <Seo title={`${t('signup.card')} · HEARASIDE`} noindex />
      <div className="wrap auth-split">
        <div className="auth-pitch">
          <h1 className="h2 rise">{t('signup.h1')}</h1>
          <p className="lead rise" style={{ animationDelay: '80ms' }}>{t('signup.lead')}</p>
          <ul className="checks rise" style={{ animationDelay: '160ms' }}>
            {(['benefit.links', 'benefit.friends', 'benefit.appearance'] as const).map(k => (
              <li key={k}><Check /><span>{t(k)}</span></li>
            ))}
          </ul>
        </div>

        <div className="card auth-card">
          {sentTo ? (
            <div className="flex flex-col gap-4 items-start" role="status">
              <span className="tile solid"><Mail size={20} strokeWidth={1.8} aria-hidden="true" /></span>
              <h2 className="h4">{t('signup.sentTitle')}</h2>
              <p className="m-0 text-[15px] leading-relaxed text-ink2">
                {t('signup.sentA')} <strong className="text-ink break-all">{sentTo}</strong>. {t('signup.sentB')}
              </p>
              <button type="button" className="btn" onClick={() => { setSentTo(null); reset(); }}>{t('signup.different')}</button>
            </div>
          ) : (
            <form className="flex flex-col gap-4" noValidate onSubmit={submit}>
              <h2 className="h4">{t('signup.card')}</h2>
              <Field label={t('signup.name')} placeholder={t('signup.namePh')} autoComplete="nickname" maxLength={40} {...register('displayName')} />
              <Field label={t('signup.email')} type="email" placeholder={t('signup.emailPh')} autoComplete="email" inputMode="email" {...register('email')} />
              <Field label={t('signup.password')} type="password" placeholder={t('signup.passwordPh')} autoComplete="new-password"
                     maxLength={128} {...register('password')} />
              <Strength pw={pw} />
              <label className="agree">
                <input type="checkbox" {...register('agree')} />
                <span>
                  {t('signup.agreeA')} <Link to={path('/terms')} target="_blank">{t('signup.terms')}</Link>{' '}
                  {t('signup.and')} <Link to={path('/privacy')} target="_blank">{t('signup.privacy')}</Link>
                </span>
              </label>
              <Turnstile onToken={setCaptcha} />
              {err && <p className="alert" role="alert">{authErrorText(t, err)}</p>}
              <SubmitButton busy={formState.isSubmitting} disabled={!canSend} className="solid big">{t('signup.submit')}</SubmitButton>
              <OrLine />
              <GoogleButton onDone={() => navigate(next)} onError={setErr} />
              <DiscordButton next={next} onError={setErr} />
            </form>
          )}
        </div>
      </div>
    </PageShell>
  );
}
