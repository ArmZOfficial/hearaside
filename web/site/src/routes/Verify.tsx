// "/verify": where the confirmation email and the Discord and Google sign-ins come back to. Supabase turns
// the code in the URL into a session (Google: we hand its ID token over); then "Continue" goes on (to /link
// when a plug-in started it).
import { useEffect, useRef, useState } from 'react';
import { Link, useSearchParams } from 'react-router';
import { Check } from '../components/ui/Kit';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { finishGoogleSignIn, safeNext } from '../components/AuthBits';
import { useAuth } from '../lib/auth';
import { Seo } from '../lib/seo';

export default function Verify() {
  const { t, path } = useT();
  const auth = useAuth();
  const [params] = useSearchParams();
  const [googleNext, setGoogleNext] = useState<string | null>(null);
  const [googleFailed, setGoogleFailed] = useState(false);
  const next = googleNext ?? safeNext(params.get('next'), path('/account'));
  const [urlError] = useState(() => !!(params.get('error') || params.get('error_description'))
    || (typeof window !== 'undefined' && /error=/.test(window.location.hash)));
  const [gaveUp, setGaveUp] = useState(false);
  const started = useRef(false);
  useEffect(() => {
    if (started.current) return;   // once, even when React runs effects twice
    started.current = true;
    finishGoogleSignIn(auth.signInWithGoogle).then(n => { if (n) setGoogleNext(n); }, () => setGoogleFailed(true));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  useEffect(() => {
    // the code exchange takes a moment after the page opens
    const id = window.setTimeout(() => setGaveUp(true), 6000);
    return () => window.clearTimeout(id);
  }, []);

  const ok = auth.status === 'signedIn';
  const failed = !ok && (urlError || googleFailed || (auth.status === 'signedOut' && gaveUp));

  return (
    <PageShell nav="none">
      <Seo title={`${t('verify.h1')} · HEARASIDE`} noindex />
      <div className="wrap auth-center">
        <div className="card auth-card narrow" role="status">
          {ok ? (
            <div className="flex flex-col gap-4 items-start">
              <span className="tile solid"><Check /></span>
              <h1 className="h4">{t('verify.done')}</h1>
              <Link className="btn solid big" to={next}>{t('verify.continue')}</Link>
            </div>
          ) : failed ? (
            <div className="flex flex-col gap-4 items-start">
              <h1 className="h4">{t('verify.failedH1')}</h1>
              <p className="alert">{t('verify.failed')}</p>
              <Link className="btn" to="/login">{t('login.submit')}</Link>
            </div>
          ) : (
            <div className="flex items-center gap-3"><span className="spin" aria-hidden="true" /><h1 className="h4">{t('verify.h1')}</h1></div>
          )}
        </div>
      </div>
    </PageShell>
  );
}
