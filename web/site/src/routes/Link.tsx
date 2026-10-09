// "/link" (3.11 Link, 3.11.1 #10): approve a plug-in's sign-in code (device flow, A4).
//   1 enter the code (8 boxes, jumps ahead, paste the whole code) -> 2 "Sign in HEARASIDE on this
//   computer?" -> 3 signed in / 4 not allowed. Must be signed in; otherwise /login comes first.
import { useEffect, useRef, useState, type ClipboardEvent, type KeyboardEvent } from 'react';
import { Link as RouterLink, Navigate, useLocation, useSearchParams } from 'react-router';
import { motion, useReducedMotion } from 'motion/react';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { SubmitButton } from '../components/ui/Kit';
import { useAuth } from '../lib/auth';
import { ApiError, useApi } from '../lib/api';
import { Seo } from '../lib/seo';
import { USER_CODE_ALPHABET, formatUserCode, normalizeUserCode, type PendingDevice } from '../../shared/schemas';

const PLUGIN_NAMES: Record<string, string> = { hub: 'HEARASIDE Hub', track: 'HEARASIDE Track', app: 'HEARASIDE App Audio' };
const PLUGIN_TILE: Record<string, string> = { hub: 'HUB', track: 'TRACK', app: 'APP' };

function CodeBoxes({ value, onChange, invalid }: { value: string; onChange(v: string): void; invalid: boolean }) {
  const { t } = useT();
  const refs = useRef<(HTMLInputElement | null)[]>([]);
  const chars = Array.from({ length: 8 }, (_, i) => value[i] ?? '');
  const clean = (s: string) => s.toUpperCase().split('').filter(c => USER_CODE_ALPHABET.includes(c)).join('');

  const setAt = (i: number, raw: string) => {
    const c = clean(raw);
    if (c.length > 1) {   // typed fast or autofilled several characters
      const merged = (value.slice(0, i) + c).slice(0, 8);
      onChange(merged);
      refs.current[Math.min(merged.length, 7)]?.focus();
      return;
    }
    const arr = chars.slice();
    arr[i] = c;
    const next = arr.join('').slice(0, 8);
    onChange(next);
    if (c && i < 7) refs.current[i + 1]?.focus();
  };
  const key = (i: number, e: KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Backspace' && !chars[i] && i > 0) { refs.current[i - 1]?.focus(); onChange(value.slice(0, i - 1)); e.preventDefault(); }
    if (e.key === 'ArrowLeft' && i > 0) refs.current[i - 1]?.focus();
    if (e.key === 'ArrowRight' && i < 7) refs.current[i + 1]?.focus();
  };
  const paste = (e: ClipboardEvent<HTMLInputElement>) => {
    const c = clean(e.clipboardData.getData('text').replace(/[\s-]/g, '')).slice(0, 8);
    if (!c) return;
    e.preventDefault();
    onChange(c);
    refs.current[Math.min(c.length, 7)]?.focus();
  };
  return (
    <div className={`codeboxes ${invalid ? 'shake' : ''}`} role="group" aria-label={t('link.code')}>
      {chars.map((c, i) => (
        <span key={i} className="contents">
          {i === 4 && <span className="code-dash" aria-hidden="true">–</span>}
          <input ref={el => { refs.current[i] = el; }} className="codebox" value={c} inputMode="text" autoCapitalize="characters"
                 autoComplete={i === 0 ? 'one-time-code' : 'off'} spellCheck={false} maxLength={8}
                 aria-label={t('link.codeDigit', { n: i + 1 })} aria-invalid={invalid || undefined}
                 onChange={e => setAt(i, e.target.value)} onKeyDown={e => key(i, e)} onPaste={paste}
                 onFocus={e => e.target.select()} />
        </span>
      ))}
    </div>
  );
}

function ago(iso: string, t: ReturnType<typeof useT>['t']) {
  const min = Math.floor((Date.now() - new Date(iso).getTime()) / 60000);
  return min < 1 ? t('link.justNow') : t('link.minutesAgo', { n: min });
}

/** the check mark draws itself and a ring ripples once (3.11.1 #10) */
function DoneMark() {
  const reduce = useReducedMotion();
  return (
    <span className="donemark" aria-hidden="true">
      {!reduce && <motion.span className="ripple" initial={{ scale: 0.6, opacity: 0.5 }} animate={{ scale: 1.8, opacity: 0 }} transition={{ duration: 0.8, ease: 'easeOut' }} />}
      <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={2.2} strokeLinecap="round" strokeLinejoin="round">
        <motion.path d="M5 12.5l4.5 4.5L19 7.5" initial={{ pathLength: reduce ? 1 : 0 }} animate={{ pathLength: 1 }} transition={{ duration: 0.45, ease: 'easeOut', delay: 0.1 }} />
      </svg>
    </span>
  );
}

export default function LinkPage() {
  const { t, path } = useT();
  const auth = useAuth();
  const api = useApi();
  const loc = useLocation();
  const [params, setParams] = useSearchParams();
  const fromUrl = normalizeUserCode(params.get('code') ?? '') ?? '';
  const [code, setCode] = useState(fromUrl);
  const [stage, setStage] = useState<'enter' | 'confirm' | 'done' | 'denied'>('enter');
  const [pending, setPending] = useState<PendingDevice | null>(null);
  const [bad, setBad] = useState(false);
  const [busy, setBusy] = useState(false);
  const tried = useRef(false);

  const lookup = async (c: string) => {
    const norm = normalizeUserCode(c);
    if (!norm) { setBad(true); return; }
    setBusy(true); setBad(false);
    try {
      const p = await api<PendingDevice>('POST', '/device/lookup', { user_code: norm });
      setPending(p);
      setStage('confirm');
    } catch {
      setBad(true);
    } finally {
      setBusy(false);
    }
  };

  // "Open the page" in the plug-in fills the code in: go straight to the question
  useEffect(() => {
    if (auth.status !== 'signedIn' || tried.current || !fromUrl) return;
    tried.current = true;
    lookup(fromUrl);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [auth.status, fromUrl]);

  if (auth.status === 'signedOut') {
    return <Navigate to={`/login?next=${encodeURIComponent(loc.pathname + loc.search)}`} replace />;
  }

  const decide = async (allow: boolean) => {
    if (!pending) return;
    setBusy(true);
    try {
      await api('POST', '/device/approve', { user_code: pending.userCode, allow });
      setStage(allow ? 'done' : 'denied');
    } catch (e) {
      setBad(true);
      setStage(e instanceof ApiError && e.status === 404 ? 'enter' : stage);
    } finally {
      setBusy(false);
    }
  };

  const startOver = () => {
    setCode(''); setPending(null); setBad(false); setStage('enter'); tried.current = true;
    setParams({}, { replace: true });
  };

  const info = pending?.clientInfo;
  return (
    <PageShell>
      <Seo title={`${t('link.h1')} · HEARASIDE`} noindex />
      <div className="wrap auth-center">
        <div className="card auth-card link-card">
          {auth.status === 'loading' ? <div className="spin mx-auto" aria-hidden="true" /> : stage === 'enter' ? (
            <form className="flex flex-col gap-5" noValidate onSubmit={e => { e.preventDefault(); lookup(code); }}>
              <h1 className="h4">{t('link.h1')}</h1>
              <p className="m-0 text-[15px] leading-relaxed text-ink2">{t('link.lead')}</p>
              <CodeBoxes value={code} onChange={v => { setCode(v); setBad(false); }} invalid={bad} />
              {bad && <p className="alert" role="alert">{t('link.bad')}</p>}
              <SubmitButton busy={busy} disabled={code.length !== 8} className="solid big">{t('link.continue')}</SubmitButton>
            </form>
          ) : stage === 'confirm' && info ? (
            <div className="flex flex-col gap-5">
              <h1 className="h4">{t('link.confirmH1')}</h1>
              <div className="devcard">
                <div className="flex items-center gap-3">
                  <span className="tile solid text-[11px] font-semibold tracking-[.12em]">{PLUGIN_TILE[info.plugin] ?? 'HUB'}</span>
                  <div>
                    <div className="text-base font-semibold">{PLUGIN_NAMES[info.plugin] ?? 'HEARASIDE'}</div>
                    <div className="text-[13px] muted">{t('link.version', { version: info.version })}</div>
                  </div>
                  <span className="ml-auto font-mono text-sm muted">{formatUserCode(pending!.userCode)}</span>
                </div>
                <div className="kv"><span className="muted">{t('link.computer')}</span><span>{info.computer}</span></div>
                <div className="kv"><span className="muted">{t('link.daw')}</span><span>{info.daw}</span></div>
                <div className="kv"><span className="muted">{t('link.system')}</span><span>{info.os}</span></div>
                <div className="kv"><span className="muted">{t('link.requested')}</span><span>{ago(pending!.createdAt, t)}</span></div>
              </div>
              <p className="m-0 text-sm leading-relaxed text-ink2">{t('link.warn')}</p>
              {bad && <p className="alert" role="alert">{t('link.bad')}</p>}
              <div className="flex flex-wrap gap-2.5">
                <SubmitButton type="button" busy={busy} className="solid big" onClick={() => decide(true)}>{t('link.allow')}</SubmitButton>
                <button type="button" className="btn big" disabled={busy} onClick={() => decide(false)}>{t('link.deny')}</button>
              </div>
            </div>
          ) : stage === 'done' ? (
            <div className="flex flex-col gap-4 items-start" role="status">
              <DoneMark />
              <h1 className="h4">{t('link.doneH1', { computer: info?.computer ?? '' })}</h1>
              <p className="m-0 text-[15px] leading-relaxed text-ink2">
                {t('link.doneA')} <RouterLink to={path('/account/devices')}>{t('link.doneLink')}</RouterLink>.
              </p>
              <button type="button" className="btn" onClick={startOver}>{t('link.another')}</button>
            </div>
          ) : (
            <div className="flex flex-col gap-4 items-start" role="status">
              <h1 className="h4">{t('link.deniedH1')}</h1>
              <p className="m-0 text-[15px] leading-relaxed text-ink2">{t('link.denied')}</p>
              <button type="button" className="btn" onClick={startOver}>{t('link.startOver')}</button>
            </div>
          )}
        </div>
      </div>
    </PageShell>
  );
}
