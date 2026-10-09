import { Link } from 'react-router';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { Seo } from '../lib/seo';

export default function NotFound() {
  const { t, path } = useT();
  return (
    <PageShell>
      <Seo title={`${t('notFound.h1')} · HEARASIDE`} noindex />
      <div className="wrap py-24 flex flex-col items-start gap-5">
        <span className="eyebrow">404</span>
        <h1 className="h2">{t('notFound.h1')}</h1>
        <Link className="btn solid" to={path('/')}>{t('notFound.home')}</Link>
      </div>
    </PageShell>
  );
}
