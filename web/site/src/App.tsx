// Every route of the site. Marketing pages exist in English ("/...") and Thai ("/th/..."); they are
// prerendered to HTML at build time (scripts/prerender.mjs reads PRERENDER below). Account pages are
// a normal SPA. Pages are code-split so the home page carries only what it needs (3.11.1 budget).
import { lazy, Suspense, useEffect, useRef, type ReactNode } from 'react';
import { Route, Routes, useLocation } from 'react-router';
import { loadPage, reloadIfNewer } from './lib/update';
import Home from './routes/Home';
import { langOf, useT } from './i18n';

const Download = lazy(loadPage(() => import('./routes/Download')));
const Guides = lazy(loadPage(() => import('./routes/Guides')));
const Guide = lazy(loadPage(() => import('./routes/Guides').then(m => ({ default: m.GuidePage }))));
const Legal = lazy(loadPage(() => import('./routes/Legal')));
const Signup = lazy(loadPage(() => import('./routes/Signup')));
const Login = lazy(loadPage(() => import('./routes/Login')));
const Forgot = lazy(loadPage(() => import('./routes/Forgot')));
const Reset = lazy(loadPage(() => import('./routes/Reset')));
const Verify = lazy(loadPage(() => import('./routes/Verify')));
const LinkPage = lazy(loadPage(() => import('./routes/Link')));
const Account = lazy(loadPage(() => import('./routes/Account')));
const Admin = lazy(loadPage(() => import('./routes/Admin')));
const NotFound = lazy(loadPage(() => import('./routes/NotFound')));

/** marketing pages, in both languages, written out as HTML by the prerender step */
export const PRERENDER = ['/', '/download', '/guides', '/privacy', '/terms'];

function PageLoading() {
  const { t } = useT();
  return <div className="page-wait query-feedback wrap" role="status"><span className="spin" aria-hidden="true" />{t('loading')}</div>;
}
const page = (el: ReactNode) => <Suspense fallback={<PageLoading />}>{el}</Suspense>;

function both(path: string, el: ReactNode) {
  const th = path === '/' ? '/th' : `/th${path}`;
  return [
    <Route key={path} path={path} element={page(el)} />,
    <Route key={th} path={th} element={page(el)} />,
  ];
}

/** a newer build deployed while this tab was open is loaded on the next page change */
function useNewBuildOnNavigate() {
  const { pathname } = useLocation();
  const first = useRef(true);
  useEffect(() => {
    if (first.current) { first.current = false; return; }
    void reloadIfNewer();
  }, [pathname]);
}

export default function App() {
  useNewBuildOnNavigate();
  const { pathname } = useLocation();
  useEffect(() => { document.documentElement.lang = langOf(pathname); }, [pathname]);
  return (
    <Routes>
      {both('/', <Home />)}
      {both('/download', <Download />)}
      {both('/guides', <Guides />)}
      {both('/guides/:slug', <Guide />)}
      {both('/privacy', <Legal kind="privacy" />)}
      {both('/terms', <Legal kind="terms" />)}
      {both('/signup', <Signup />)}
      {both('/login', <Login />)}
      {both('/forgot', <Forgot />)}
      {both('/reset', <Reset />)}
      {both('/verify', <Verify />)}
      {both('/link', <LinkPage />)}
      {both('/account', <Account />)}
      {both('/account/:section', <Account />)}
      {both('/admin', <Admin />)}
      {both('/admin/:section', <Admin />)}
      <Route path="*" element={page(<NotFound />)} />
    </Routes>
  );
}
