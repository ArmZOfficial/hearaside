// Nav + footer around every page (3.11 Nav, 3.11.1 #7 and #8).
//  - full nav: wordmark · Features · How it works · Guides · Download · EN · ไทย · theme · Sign in + Get HEARASIDE | avatar
//  - minimal nav on sign-up / sign-in pages: wordmark + "Already have an account? Sign in"
//  - scrolling down turns the nav into a glass card stuck to the top; the current section is underlined
//  - phones get a full-screen menu that slides down
import { useEffect, useState, type ReactNode } from 'react';
import { Link, NavLink, useLocation } from 'react-router';
import { AnimatePresence, motion, useReducedMotion } from 'motion/react';
import { Menu, Monitor, Moon, Sun, X } from 'lucide-react';
import { otherLangPath, useT } from '../i18n';
import { useTheme } from '../lib/theme';
import { useAuth } from '../lib/auth';
import { useMe } from '../lib/api';
import { Avatar, Wordmark } from './ui/Kit';


function useScrolled() {
  const [s, setS] = useState(false);
  useEffect(() => {
    const on = () => setS(window.scrollY > 24);
    on();
    window.addEventListener('scroll', on, { passive: true });
    return () => window.removeEventListener('scroll', on);
  }, []);
  return s;
}

/** which home-page section is on screen (for the sliding underline) */
function useSection(enabled: boolean) {
  const [id, setId] = useState<string | null>(null);
  useEffect(() => {
    if (!enabled) { setId(null); return; }
    const ids = ['features', 'how', 'guides'];
    const els = ids.map(i => document.getElementById(i)).filter(Boolean) as HTMLElement[];
    const io = new IntersectionObserver(es => {
      const vis = es.filter(e => e.isIntersecting).sort((a, b) => a.boundingClientRect.top - b.boundingClientRect.top)[0];
      if (vis) setId(vis.target.id);
    }, { rootMargin: '-40% 0px -50% 0px' });
    els.forEach(e => io.observe(e));
    return () => io.disconnect();
  }, [enabled]);
  return id;
}

function ThemeButton({ className = '' }: { className?: string }) {
  const { t } = useT();
  const { mode, cycle } = useTheme();
  const Icon = mode === 'light' ? Sun : mode === 'dark' ? Moon : Monitor;
  return (
    <button type="button" className={`iconbtn ${className}`} onClick={cycle} aria-label={t('nav.themeAria', { mode: t(`nav.theme.${mode}`) })}
            title={t('nav.themeAria', { mode: t(`nav.theme.${mode}`) })}>
      <Icon size={17} strokeWidth={1.8} aria-hidden="true" />
    </button>
  );
}

function LangButton() {
  const { t } = useT();
  const { pathname, search } = useLocation();
  return (
    <Link to={otherLangPath(pathname) + search} className="langbtn" aria-label={t('nav.langAria')} hrefLang={pathname.startsWith('/th') ? 'en' : 'th'}>
      {t('nav.lang')}
    </Link>
  );
}

function AccountButton() {
  const { t, path } = useT();
  const { status } = useAuth();
  const me = useMe();
  if (status !== 'signedIn') return null;
  const name = me.data?.displayName || me.data?.handle || '';
  return (
    <Link to={path('/account')} className="acctbtn" aria-label={me.data ? t('nav.account', { name, handle: me.data.handle }) : 'Account'}>
      <Avatar name={name || '?'} url={me.data?.avatarUrl} size={40} />
    </Link>
  );
}

export function Nav() {
  const { t, path, lang } = useT();
  const { pathname } = useLocation();
  const { status } = useAuth();
  const scrolled = useScrolled();
  const home = pathname === '/' || pathname === '/th';
  const section = useSection(home);
  const [open, setOpen] = useState(false);
  const reduce = useReducedMotion();
  useEffect(() => setOpen(false), [pathname]);
  useEffect(() => {
    if (!open) return;
    const esc = (e: KeyboardEvent) => e.key === 'Escape' && setOpen(false);
    window.addEventListener('keydown', esc);
    document.body.style.overflow = 'hidden';
    return () => { window.removeEventListener('keydown', esc); document.body.style.overflow = ''; };
  }, [open]);

  const homeHash = (h: string) => (home ? `#${h}` : `${path('/')}#${h}`);
  const links: { id: string; label: string; to: string; hash?: boolean }[] = [
    { id: 'features', label: t('nav.features'), to: homeHash('features'), hash: true },
    { id: 'how', label: t('nav.how'), to: homeHash('how'), hash: true },
    { id: 'guides', label: t('nav.guides'), to: path('/guides') },
    { id: 'download', label: t('nav.download'), to: path('/download') },
  ];
  const isActive = (id: string) =>
    id === section || (id === 'guides' && /\/guides/.test(pathname)) || (id === 'download' && /\/download$/.test(pathname));

  const linkEls = (onClick?: () => void) =>
    links.map(l => {
      const active = isActive(l.id);
      const inner = (
        <>
          {l.label}
          {active && <motion.span layoutId="nav-underline" className="nav-underline" transition={{ type: 'spring', stiffness: 400, damping: 34 }} />}
        </>
      );
      return l.hash
        ? <a key={l.id} className="navlink" href={l.to} onClick={onClick} aria-current={active ? 'true' : undefined}>{inner}</a>
        : <NavLink key={l.id} className="navlink" to={l.to} onClick={onClick}>{inner}</NavLink>;
    });

  return (
    <header className={`nav-shell ${scrolled ? 'scrolled' : ''}`}>
      <nav aria-label="Main" className="wrap nav">
        <Link to={path('/')} className="brand" aria-label="HEARASIDE home"><Wordmark /></Link>
        <div className="nav-links">{linkEls()}</div>
        <div className="nav-right">
          <LangButton />
          <ThemeButton className="nav-theme" />
          {status === 'signedIn' ? <AccountButton /> : (
            <span className="nav-auth">
              <Link className="navlink" to={`/login${lang === 'th' ? '?lang=th' : ''}`}>{t('nav.signIn')}</Link>
              <Link className="btn solid" to={path('/download')}>{t('nav.get')}</Link>
            </span>
          )}
          <button type="button" className="iconbtn nav-menu" aria-label={open ? t('nav.close') : t('nav.menu')} aria-expanded={open}
                  onClick={() => setOpen(v => !v)}>
            {open ? <X size={18} aria-hidden="true" /> : <Menu size={18} aria-hidden="true" />}
          </button>
        </div>
      </nav>
      <AnimatePresence>
        {open && (
          <motion.div className="mobile-menu" role="dialog" aria-modal="true" aria-label={t('nav.menu')}
                      initial={reduce ? { opacity: 0 } : { y: '-100%' }} animate={reduce ? { opacity: 1 } : { y: 0 }}
                      exit={reduce ? { opacity: 0 } : { y: '-100%' }} transition={{ type: 'spring', stiffness: 320, damping: 34 }}>
            <div className="wrap mobile-inner">
              {linkEls(() => setOpen(false))}
              <div className="flex items-center gap-3 pt-4"><LangButton /><ThemeButton /></div>
              {status !== 'signedIn' && (
                <div className="flex flex-col gap-3 pt-4">
                  <Link className="btn solid big" to={path('/download')}>{t('nav.get')}</Link>
                  <Link className="btn big" to="/login">{t('nav.signIn')}</Link>
                </div>
              )}
            </div>
          </motion.div>
        )}
      </AnimatePresence>
    </header>
  );
}

export function MinimalNav({ kind }: { kind: 'signup' | 'login' | 'none' }) {
  const { t, path } = useT();
  return (
    <header className="nav-shell">
      <nav aria-label="Main" className="wrap nav">
        <Link to={path('/')} className="brand" aria-label="HEARASIDE home"><Wordmark /></Link>
        <div className="nav-right">
          {kind === 'signup' && <span className="nav-hint">{t('nav.haveAccount')} <Link to="/login">{t('nav.signIn')}</Link></span>}
          {kind === 'login' && <span className="nav-hint">{t('nav.newHere')} <Link to="/signup">{t('nav.createAccount')}</Link></span>}
          <LangButton />
          <ThemeButton />
        </div>
      </nav>
    </header>
  );
}

export function Footer() {
  const { t, path } = useT();
  return (
    <footer className="wrap">
      <div className="footer">
        <span className="wordmark">HEARASIDE</span>
        <div className="flex flex-wrap gap-5">
          <Link to={path('/guides')}>{t('footer.guides')}</Link>
          <Link to={path('/download')}>{t('footer.download')}</Link>
          <Link to={path('/privacy')}>{t('footer.privacy')}</Link>
          <Link to={path('/terms')}>{t('footer.terms')}</Link>
          <a href="mailto:[CONTACT EMAIL]">{t('footer.contact')}</a>
        </div>
        <span>{t('footer.owner')}</span>
      </div>
    </footer>
  );
}

let firstPage = true;   // the first page is prerendered HTML: no entrance animation for it

/** fade + 8 px rise between routes (≤ 200 ms); plain fade with reduced motion */
export function PageShell({ nav = 'full', children }: { nav?: 'full' | 'signup' | 'login' | 'none'; children: ReactNode }) {
  const { t } = useT();
  const { pathname } = useLocation();
  const reduce = useReducedMotion();
  const [initial] = useState(() => (firstPage ? false : { opacity: 0, y: reduce ? 0 : 8 }));
  useEffect(() => { firstPage = false; }, []);
  useEffect(() => {
    if (!window.location.hash) window.scrollTo(0, 0);
  }, [pathname]);
  return (
    <>
      <a className="skip" href="#main">{t('skip')}</a>
      {nav === 'full' ? <Nav /> : <MinimalNav kind={nav} />}
      <motion.main id="main" key={pathname} className="relative z-[1]" tabIndex={-1}
                   initial={initial} animate={{ opacity: 1, y: 0 }}
                   transition={{ duration: reduce ? 0.12 : 0.2, ease: 'easeOut' }}>
        {children}
      </motion.main>
      {nav === 'full' && <Footer />}
    </>
  );
}
