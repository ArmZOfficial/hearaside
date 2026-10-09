// Every route of the site. Marketing pages exist in English ("/...") and Thai ("/th/..."); they are
// prerendered to HTML at build time (scripts/prerender.mjs reads PRERENDER below). Account pages are
// a normal SPA. Pages are code-split so the home page carries only what it needs (3.11.1 budget).
import { lazy, Suspense, type ReactNode } from 'react';
import { Route, Routes } from 'react-router';
import Home from './routes/Home';

const Download = lazy(() => import('./routes/Download'));
const Guides = lazy(() => import('./routes/Guides'));
const Guide = lazy(() => import('./routes/Guides').then(m => ({ default: m.GuidePage })));
const Legal = lazy(() => import('./routes/Legal'));
const Signup = lazy(() => import('./routes/Signup'));
const Login = lazy(() => import('./routes/Login'));
const Forgot = lazy(() => import('./routes/Forgot'));
const Reset = lazy(() => import('./routes/Reset'));
const Verify = lazy(() => import('./routes/Verify'));
const LinkPage = lazy(() => import('./routes/Link'));
const Account = lazy(() => import('./routes/Account'));
const NotFound = lazy(() => import('./routes/NotFound'));

/** marketing pages, in both languages, written out as HTML by the prerender step */
export const PRERENDER = ['/', '/download', '/guides', '/privacy', '/terms'];

const page = (el: ReactNode) => <Suspense fallback={<div className="page-wait" />}>{el}</Suspense>;

function both(path: string, el: ReactNode) {
  const th = path === '/' ? '/th' : `/th${path}`;
  return [
    <Route key={path} path={path} element={page(el)} />,
    <Route key={th} path={th} element={page(el)} />,
  ];
}

export default function App() {
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
      <Route path="*" element={page(<NotFound />)} />
    </Routes>
  );
}
