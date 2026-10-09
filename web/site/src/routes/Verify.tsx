// "/verify": where the confirmation email and the Discord sign-in come back to. Supabase turns the
// code in the URL into a session; then "Continue" goes on (to /link when a plug-in started it).
import { useEffect, useState } from 'react';
import { Link, useSearchParams } from 'react-router';
import { Check } from '../components/ui/Kit';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { safeNext } from '../components/AuthBits';
import { useAuth } from '../lib/auth';
import { Seo } from '../lib/seo';

export default function Verify() {
  const { t, path } = useT();
  const auth = useAuth();
  const [params] = useSearchParams();
  const next = safeNext(params.get('next'), path('/account'));
  const urlError = params.get('error') || params.get('error_description')
    || (typeof window !== 'undefined' && /error=/.test(window.location.hash));
  const [gaveUp, setGaveUp] = useState(false);
  useEffect(() => {
    // the code exchange takes a moment after the page opens
    const id = window.setTimeout(() => setGaveUp(true), 6000);
    return () => window.clearTimeout(id);
  }, []);

  const ok = auth.status === 'signedIn';
  const failed = !ok && (urlError || (auth.status === 'signedOut' && gaveUp));

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
