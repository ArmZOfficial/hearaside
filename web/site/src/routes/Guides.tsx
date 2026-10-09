// "/guides" and "/guides/:slug" (3.11): EN/TH guides written in MDX, same look as the rest of the site.
import type { AnchorHTMLAttributes } from 'react';
import { Link, useParams } from 'react-router';
import { ArrowLeft, ArrowRight } from 'lucide-react';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { Reveal } from '../motion/Reveal';
import { Seo } from '../lib/seo';
import { guideFor, guidesFor } from '../content/guides';
import NotFound from './NotFound';

/** links inside a guide stay in the app (and in the page's language) */
function MdxLink({ href = '', children, ...rest }: AnchorHTMLAttributes<HTMLAnchorElement>) {
  if (href.startsWith('/')) return <Link to={href} {...rest}>{children}</Link>;
  return <a href={href} rel="noopener noreferrer" target="_blank" {...rest}>{children}</a>;
}

export default function Guides() {
  const { t, lang, path } = useT();
  const list = guidesFor(lang);
  return (
    <PageShell>
      <Seo title={`${t('guides.eyebrow')} · HEARASIDE`} description={t('guides.lead')} />
      <div className="wrap">
        <header className="pt-12 pb-8 flex flex-col gap-3">
          <span className="eyebrow rise">{t('guides.eyebrow')}</span>
          <h1 className="h1 dl-h1 rise" style={{ animationDelay: '80ms' }}>{t('guides.h1')}</h1>
          <p className="lead rise" style={{ animationDelay: '160ms' }}>{t('guides.lead')}</p>
        </header>
        <div className="grid gap-4 pb-20" style={{ gridTemplateColumns: 'repeat(auto-fit, minmax(min(300px, 100%), 1fr))' }}>
          {list.map((g, i) => (
            <Reveal key={g.slug} delay={i * 60}>
              <Link to={path(`/guides/${g.slug}`)} className="card guide-card lift">
                <span className="num">{i + 1}</span>
                <span className="text-lg font-semibold text-ink">{g.meta.title}</span>
                <span className="text-[15px] leading-relaxed text-ink2">{g.meta.description}</span>
                <ArrowRight size={18} className="guide-arrow" aria-hidden="true" />
              </Link>
            </Reveal>
          ))}
        </div>
      </div>
    </PageShell>
  );
}

export function GuidePage() {
  const { slug = '' } = useParams();
  const { t, lang, path } = useT();
  const g = guideFor(lang, slug);
  if (!g) return <NotFound />;
  const list = guidesFor(lang);
  const at = list.findIndex(x => x.slug === slug);
  const next = list[at + 1];
  const Body = g.Body as React.ComponentType<{ components?: Record<string, unknown> }>;
  return (
    <PageShell>
      <Seo title={`${g.meta.title} · HEARASIDE`} description={g.meta.description} />
      <div className="wrap guide-wrap">
        <Link to={path('/guides')} className="back-link"><ArrowLeft size={16} aria-hidden="true" />{t('guides.back')}</Link>
        <article className="card guide-body prose-hs" lang={g.lang}>
          <Body components={{ a: MdxLink }} />
        </article>
        {next && (
          <Link to={path(`/guides/${next.slug}`)} className="card next-guide lift">
            <span className="text-[13px] muted">{at + 2} / {list.length}</span>
            <span className="text-lg font-semibold text-ink flex items-center gap-2">{next.meta.title}<ArrowRight size={18} aria-hidden="true" /></span>
          </Link>
        )}
      </div>
    </PageShell>
  );
}
