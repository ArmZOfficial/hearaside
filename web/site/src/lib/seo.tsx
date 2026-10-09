// Page title + description + language alternates. React 19 lifts <title>/<meta>/<link> into
// <head>; the prerender script moves them there in the built HTML too.
import { useLocation } from 'react-router';
import { langOf, otherLangPath } from '../i18n';

export const SITE_URL = 'https://hearaside.vercel.app';

export function Seo({ title, description, noindex = false }: { title: string; description?: string; noindex?: boolean }) {
  const { pathname } = useLocation();
  const lang = langOf(pathname);
  const other = otherLangPath(pathname);
  return (
    <>
      <title>{title}</title>
      {description && <meta name="description" content={description} />}
      {description && <meta property="og:description" content={description} />}
      <meta property="og:title" content={title} />
      <meta property="og:locale" content={lang === 'th' ? 'th_TH' : 'en_US'} />
      {noindex && <meta name="robots" content="noindex" />}
      {!noindex && <link rel="canonical" href={SITE_URL + pathname} />}
      {!noindex && <link rel="alternate" hrefLang={lang === 'th' ? 'en' : 'th'} href={SITE_URL + other} />}
    </>
  );
}
