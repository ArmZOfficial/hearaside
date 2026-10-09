// "/" (3.11 Home + 3.11.1 #1–#6). Text is the mock's English, word for word (i18n/en.json).
import { lazy, Suspense, useMemo, useRef, useState } from 'react';
import { Link } from 'react-router';
import { AnimatePresence, LayoutGroup, motion } from 'motion/react';
import { ChevronDown, Download, Play, Square, Check as CheckIcon } from 'lucide-react';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { AudibleToggle, Screenshot } from '../components/ui/Kit';
import { HeroWaves } from '../motion/HeroWaves';
import { Tilt } from '../motion/Tilt';
import { Parallax, Reveal } from '../motion/Reveal';
import { CountUp } from '../motion/CountUp';
import { Seo } from '../lib/seo';

const LineUp = lazy(() => import('../motion/LineUp'));

function MixDemo() {
  const { t } = useT();
  const names = t('home.mix.rows').split('|');
  const [rows, setRows] = useState(() => [
    { id: 'backing', you: true, view: true },
    { id: 'vocal', you: false, view: true },
    { id: 'click', you: true, view: false },
    { id: 'talk', you: true, view: false },
    { id: 'friend', you: true, view: true },
  ].map((r, i) => ({ ...r, name: names[i] ?? r.id })));
  const [playing, setPlaying] = useState<null | 'you' | 'viewers'>(null);
  const player = useRef<{ stop(): void; set(on: Record<string, boolean>): void } | null>(null);

  const flip = (id: string, key: 'you' | 'view') => {
    setRows(rs => {
      const next = rs.map(r => (r.id === id ? { ...r, [key]: !r[key] } : r));
      if (player.current && playing) player.current.set(Object.fromEntries(next.map(r => [r.id, playing === 'you' ? r.you : r.view])));
      return next;
    });
  };

  const play = async (as: 'you' | 'viewers') => {
    if (playing === as) { player.current?.stop(); player.current = null; setPlaying(null); return; }
    player.current?.stop();
    const { startDemo } = await import('../motion/demoAudio');
    player.current = startDemo(Object.fromEntries(rows.map(r => [r.id, as === 'you' ? r.you : r.view])), () => setPlaying(null));
    setPlaying(as);
  };

  const box = (key: 'you' | 'view', title: string) => {
    const on = rows.filter(r => r[key]);
    return (
      <div className="mixbox">
        <div className="text-xs muted">{title}</div>
        <div className="mixbox-names" aria-live="polite">
          <AnimatePresence initial={false} mode="popLayout">
            {on.length === 0
              ? <motion.span key="none" className="muted" initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>{t('home.mix.nothing')}</motion.span>
              : on.map(r => (
                <motion.span layout key={r.id} className="mixname" initial={{ opacity: 0, scale: 0.7, y: 8 }}
                             animate={{ opacity: 1, scale: 1, y: 0 }} exit={{ opacity: 0, scale: 0.7 }}
                             transition={{ type: 'spring', stiffness: 420, damping: 30 }}>
                  {r.name}
                </motion.span>
              ))}
          </AnimatePresence>
        </div>
      </div>
    );
  };

  return (
    <div className="card mixdemo">
      <div className="mixgrid mixhead">
        <span>{t('home.mix.track')}</span><span className="text-center">{t('home.mix.you')}</span><span className="text-center">{t('home.mix.viewers')}</span>
      </div>
      {rows.map(r => (
        <div key={r.id} className="mixgrid mixrow">
          <span className="mixrow-name">
            <span className="mixdot" />
            <span className="min-w-0 truncate">{r.name}</span>
            <span className={`mini-meter ${r.you || r.view ? 'live' : ''}`} aria-hidden="true"><i /></span>
          </span>
          <AudibleToggle kind="you" on={r.you} onChange={() => flip(r.id, 'you')} label={t('home.mix.youHear', { name: r.name })}
                         title={r.you ? t('home.mix.youOn') : t('home.mix.youOff')} />
          <AudibleToggle kind="viewers" on={r.view} onChange={() => flip(r.id, 'view')} label={t('home.mix.viewersHearName', { name: r.name })}
                         title={r.view ? t('home.mix.viewersOn') : t('home.mix.viewersOff')} />
        </div>
      ))}
      <LayoutGroup>
        <div className="grid grid-cols-2 gap-2.5 pt-1.5">
          {box('you', t('home.mix.inHeadphones'))}
          {box('view', t('home.mix.viewersHear'))}
        </div>
      </LayoutGroup>
      <div className="flex flex-wrap items-center gap-2 pt-1">
        <span className="text-[13px] muted">{t('home.mix.playDemo')} · {t('home.mix.listenAs')}</span>
        {(['you', 'viewers'] as const).map(as => (
          <button key={as} type="button" className={`btn small ${playing === as ? 'solid' : ''}`} aria-pressed={playing === as} onClick={() => play(as)}>
            {playing === as ? <Square size={14} aria-hidden="true" /> : <Play size={14} aria-hidden="true" />}
            {as === 'you' ? t('home.mix.you') : t('home.mix.viewers')}
          </button>
        ))}
      </div>
    </div>
  );
}

export default function Home() {
  const { t, lang, path } = useT();
  const features = useMemo(() => [
    { k: 'HUB', where: t('home.plugins.hub'), text: t('home.plugins.hubText') },
    { k: 'TRACK', where: t('home.plugins.track'), text: t('home.plugins.trackText') },
    { k: 'APP AUDIO', where: t('home.plugins.app'), text: t('home.plugins.appText') },
  ], [t]);
  const faqs = [1, 2, 3, 4] as const;

  return (
    <PageShell>
      <Seo title={t('home.title')} description={t('home.lead')} />
      <div className="wrap">
        <section className="hero">
          <div className="hero-text">
            <span className="eyebrow rise" style={{ animationDelay: '0ms' }}>{t('home.eyebrow')}</span>
            <h1 className="h1">
              <span className="rise block" style={{ animationDelay: '80ms' }}>{t('home.h1a')}</span>
              <span className="rise block" style={{ animationDelay: '160ms' }}>{t('home.h1b')}</span>
            </h1>
            <div className="rise" style={{ animationDelay: '240ms' }}><HeroWaves /></div>
            <p className="lead max-w-[520px] rise" style={{ animationDelay: '300ms' }}>{t('home.lead')}</p>
            <div className="flex flex-wrap gap-3 pt-1.5 rise" style={{ animationDelay: '360ms' }}>
              <Link className="btn solid big shine" to={path('/download')}>
                <Download size={18} strokeWidth={1.8} aria-hidden="true" />{t('home.download')}
              </Link>
              <a className="btn big" href="#how">{t('home.seeHow')}</a>
            </div>
            <p className="text-[13px] muted m-0">{t('home.fine')}</p>
          </div>
          <div className="hero-shot">
            <Tilt><Screenshot name="hub" lang={lang} alt={t('home.shotAlt')} eager width={1040} height={790} /></Tilt>
          </div>
        </section>

        <Reveal as="section" aria-label={t('home.worksWith')} className="works">
          <span className="text-[13px] muted mr-1.5">{t('home.worksWith')}</span>
          {['Studio One', 'Cubase', 'Reaper', 'FL Studio', 'Ableton Live', 'OBS Studio'].map(n => <span key={n} className="chiptext">{n}</span>)}
        </Reveal>

        <section id="features" className="split">
          <Reveal className="split-text">
            <span className="eyebrow">{t('home.mix.eyebrow')}</span>
            <h2 className="h2">{t('home.mix.h2')}</h2>
            <p className="body">{t('home.mix.text')}</p>
            <p className="text-sm muted m-0">{t('home.mix.try')}</p>
          </Reveal>
          <Reveal className="split-wide" delay={80}><MixDemo /></Reveal>
        </section>

        <section aria-labelledby="plugins-h" className="sect">
          <Reveal>
            <span className="eyebrow">{t('home.plugins.eyebrow')}</span>
            <h2 id="plugins-h" className="h2 mt-3 mb-7">{t('home.plugins.h2')}</h2>
          </Reveal>
          <div className="grid gap-4" style={{ gridTemplateColumns: 'repeat(auto-fit, minmax(280px, 1fr))' }}>
            {features.map((f, i) => (
              <Reveal key={f.k} className="card p-6 flex flex-col gap-3 lift" delay={i * 80}>
                <div className="flex items-baseline justify-between gap-3">
                  <span className="text-[13px] font-semibold tracking-[.24em]">{f.k}</span>
                  <span className="text-xs muted">{f.where}</span>
                </div>
                <p className="m-0 text-[15px] leading-relaxed text-ink2">{f.text}</p>
              </Reveal>
            ))}
          </div>
        </section>

        <section className="split reverse">
          <div className="split-wide">
            <Parallax><Screenshot name="share" lang={lang} alt={t('home.friends.shotAlt')} width={1040} height={790} /></Parallax>
          </div>
          <Reveal className="split-text">
            <span className="eyebrow">{t('home.friends.eyebrow')}</span>
            <h2 className="h2">
              {lang === 'en'
                ? <>Up to <CountUp to={8} /> voices. One beat.</>
                : t('home.friends.h2')}
            </h2>
            <ul className="numlist">
              {[1, 2, 3].map(n => (
                <li key={n}><span className="num small">{n}</span>{t(`home.friends.${n}` as 'home.friends.1')}</li>
              ))}
            </ul>
            <Suspense fallback={<div className="lineup-ph" />}><LineUp /></Suspense>
          </Reveal>
        </section>

        <section id="how" aria-labelledby="how-h" className="sect">
          <Reveal>
            <span className="eyebrow">{t('home.how.eyebrow')}</span>
            <h2 id="how-h" className="h2 mt-3 mb-7">{t('home.how.h2')}</h2>
          </Reveal>
          <div className="flex flex-wrap gap-4 items-stretch">
            <div className="grid gap-4 min-w-0" style={{ flex: '999 1 560px', gridTemplateColumns: 'repeat(auto-fit, minmax(220px, 1fr))' }}>
              {[1, 2, 3].map((n, i) => (
                <Reveal key={n} className="card p-6 flex flex-col gap-2.5 lift" delay={i * 80}>
                  <span className="text-[32px] font-semibold muted">0<CountUp to={n} ms={500} /></span>
                  <span className="text-lg font-semibold">{t(`home.how.${n}t` as 'home.how.1t')}</span>
                  <span className="text-[15px] leading-relaxed text-ink2">{t(`home.how.${n}` as 'home.how.1')}</span>
                </Reveal>
              ))}
            </div>
            <Reveal className="card p-5 grid place-items-center" style={{ flex: '1 1 260px' }} delay={160}>
              <Parallax range={16}>
                <Screenshot name="track" lang={lang} alt={t('home.how.shotAlt')} className="track-shot" width={380} height={640} />
              </Parallax>
            </Reveal>
          </div>
        </section>

        <Reveal as="section" aria-labelledby="acct-h" className="sect">
          <div className="card p-8 flex flex-wrap gap-8 items-center justify-between">
            <div className="flex flex-col gap-3 min-w-0" style={{ flex: '1 1 380px' }}>
              <span className="eyebrow">{t('home.acct.eyebrow')}</span>
              <h2 id="acct-h" className="h3">{t('home.acct.h2')}</h2>
            </div>
            <div className="flex flex-col gap-4 min-w-0" style={{ flex: '1 1 380px' }}>
              {(['benefit.links', 'benefit.friends', 'benefit.appearance'] as const).map(k => (
                <div key={k} className="flex gap-3 items-start text-[15px] leading-normal text-ink2">
                  <CheckIcon size={18} strokeWidth={2} className="mt-0.5 flex-none" aria-hidden="true" /><span>{t(k)}</span>
                </div>
              ))}
              <div className="flex flex-wrap gap-2.5 pt-1">
                <Link className="btn solid" to="/signup">{t('home.acct.create')}</Link>
                <Link className="btn" to="/login">{t('home.acct.signIn')}</Link>
              </div>
            </div>
          </div>
        </Reveal>

        <section id="guides" aria-labelledby="faq-h" className="sect flex flex-wrap gap-10">
          <Reveal className="flex flex-col gap-3.5 min-w-0" style={{ flex: '1 1 300px' }}>
            <span className="eyebrow">{t('home.faq.eyebrow')}</span>
            <h2 id="faq-h" className="h2">{t('home.faq.h2')}</h2>
            <p className="m-0 text-[15px] leading-relaxed text-ink2">{t('home.faq.text')}</p>
            <Link className="btn self-start" to={path('/guides')}>{t('home.faq.guides')}</Link>
          </Reveal>
          <Reveal className="min-w-0" style={{ flex: '999 1 560px' }} delay={80}>
            {faqs.map(n => (
              <details key={n} className="faq">
                <summary>{t(`home.faq.q${n}` as 'home.faq.q1')}<ChevronDown size={18} aria-hidden="true" /></summary>
                <p className="faq-a">{t(`home.faq.a${n}` as 'home.faq.a1')}</p>
              </details>
            ))}
          </Reveal>
        </section>
      </div>
    </PageShell>
  );
}

