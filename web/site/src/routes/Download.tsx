// "/download" (3.11 + 5D; 3.11.1 #11). No sign-in needed. The installer link and numbers come
// from src/content/release.json, written by installer/windows/build-installer.ps1.
import { useEffect, useState } from 'react';
import { Link } from 'react-router';
import { motion, useReducedMotion } from 'motion/react';
import { Download as DownloadIcon } from 'lucide-react';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { AppleIcon, WindowsIcon } from '../components/ui/Icons';
import { Check } from '../components/ui/Kit';
import { Reveal } from '../motion/Reveal';
import { Seo } from '../lib/seo';
import release from '../content/release.json';

const placeholder = (v: string) => /^\[.*\]$/.test(v);

export default function Download() {
  const { t, lang } = useT();
  const reduce = useReducedMotion();
  const [phase, setPhase] = useState<'idle' | 'starting' | 'started'>('idle');
  const [lit, setLit] = useState(-1);
  const ready = !placeholder(release.version);

  useEffect(() => {
    if (phase !== 'starting') return;
    const id = window.setTimeout(() => setPhase('started'), reduce ? 50 : 1300);
    return () => window.clearTimeout(id);
  }, [phase, reduce]);
  useEffect(() => {
    if (phase !== 'started') return;
    setLit(0);
    const ids = [1, 2].map(n => window.setTimeout(() => setLit(n), n * 700));
    return () => ids.forEach(window.clearTimeout);
  }, [phase]);

  const steps = [1, 2, 3] as const;
  return (
    <PageShell>
      <Seo title={`${t('dl.h1')} · HEARASIDE`} description={t('dl.winText')} />
      <div className="wrap">
        <header className="pt-12 pb-8 flex flex-col gap-3">
          <span className="eyebrow rise">{t('dl.eyebrow')}</span>
          <h1 className="h1 dl-h1 rise" style={{ animationDelay: '80ms' }}>{t('dl.h1')}</h1>
          <p className="m-0 text-[17px] text-ink2 rise" style={{ animationDelay: '160ms' }}>
            {t('dl.meta', { version: release.version, date: release.date })}
          </p>
        </header>

        <section className="grid gap-4 pb-14" style={{ gridTemplateColumns: 'repeat(auto-fit, minmax(min(320px, 100%), 1fr))' }}>
          <Reveal className="card p-7 flex flex-col gap-4">
            <div className="flex items-center gap-3">
              <span className="tile solid"><WindowsIcon size={20} /></span>
              <div><div className="text-xl font-semibold">{t('dl.win')}</div><div className="text-[13px] muted">{t('dl.winSub')}</div></div>
            </div>
            <p className="m-0 text-[15px] leading-relaxed text-ink2">{t('dl.winText')}</p>
            {ready ? (
              <a className={`btn solid big self-start dl-btn ${phase}`} href={release.installer} download
                 onClick={() => phase === 'idle' && setPhase('starting')} aria-live="polite">
                {phase === 'starting' && <motion.span className="dl-fill" aria-hidden="true" initial={{ scaleX: 0 }} animate={{ scaleX: 1 }} transition={{ duration: reduce ? 0 : 1.2, ease: 'easeInOut' }} />}
                <span className="relative flex items-center gap-2">
                  {phase === 'started' ? <Check /> : <DownloadIcon size={18} strokeWidth={1.8} aria-hidden="true" />}
                  {phase === 'idle' ? t('dl.winButton') : phase === 'starting' ? t('dl.downloading') : t('dl.downloaded')}
                </span>
              </a>
            ) : (
              <>
                <button type="button" className="btn solid big self-start" disabled>
                  <DownloadIcon size={18} strokeWidth={1.8} aria-hidden="true" />{t('dl.winButton')}
                </button>
                <span className="text-[13px] muted">{t('dl.notReady')}</span>
              </>
            )}
            <span className="text-xs muted break-all">{t('dl.size', { size: release.sizeMb, checksum: release.sha256 })}</span>
          </Reveal>
          <Reveal className="card p-7 flex flex-col gap-4" delay={80}>
            <div className="flex items-center gap-3">
              <span className="tile"><AppleIcon size={20} /></span>
              <div><div className="text-xl font-semibold">{t('dl.mac')}</div><div className="text-[13px] muted">{t('dl.macSub')}</div></div>
            </div>
            <p className="m-0 text-[15px] leading-relaxed text-ink2">
              {t('dl.macText')}
            </p>
            <div className="flex flex-wrap gap-2.5">
              <button type="button" className="btn" disabled>{t('dl.macButton')}</button>
              <Link className="btn" to="/signup">{t('home.acct.create')}</Link>
            </div>
          </Reveal>
        </section>

        <section className="flex flex-wrap gap-4 pb-14">
          <Reveal className="card p-7 flex flex-col gap-4.5 min-w-0" style={{ flex: '999 1 560px' }}>
            <h2 className="h4">{t('dl.steps')}</h2>
            {steps.map((n, i) => (
              <div key={n} className={`step ${lit >= i ? 'lit' : ''} ${lit === i ? 'now' : ''}`}>
                <span className="num">{lit > i ? <Check size={15} /> : n}</span>
                <div>
                  <div className="text-base font-medium">{t(`dl.step${n}` as 'dl.step1')}</div>
                  <div className="text-sm muted pt-0.5 leading-normal">{t(`dl.step${n}sub` as 'dl.step1sub')}</div>
                </div>
              </div>
            ))}
          </Reveal>
          <Reveal className="card p-7 flex flex-col gap-3.5 min-w-0" style={{ flex: '1 1 320px' }} delay={80}>
            <h2 className="h4">{t('dl.need')}</h2>
            <ul className="m-0 pl-[18px] text-[15px] leading-[1.8] text-ink2">
              <li>{t('dl.need1')}</li>
              <li>{t('dl.need2', { obs: release.obsMin })}</li>
            </ul>
            <div className="included"><Check size={16} /><span>{t('dl.included')}</span></div>
          </Reveal>
        </section>

        <Reveal as="section" aria-labelledby="new-h" className="pb-20">
          <h2 id="new-h" className="h4 mb-4">{t('dl.new')}</h2>
          <div className="card px-7 py-2">
            {release.notes.map((n, i) => (
              <div key={i} className="note">
                <span className="note-v">{n.version}</span>
                <span className="note-t">{lang === 'th' ? n.th : n.en}</span>
              </div>
            ))}
          </div>
        </Reveal>
      </div>
    </PageShell>
  );
}
