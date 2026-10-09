// "/account" and "/account/<section>" (3.11 Account, A3). Left menu with five sections; rows are a
// name + description on the left and a control (≤ 300 px) on the right. Everything saves by itself
// and says "Saved". Email, password and deleting the account live only here, never in a plug-in.
import { useEffect, useRef, useState, type ReactNode } from 'react';
import { Link, Navigate, useNavigate, useParams } from 'react-router';
import { useQueryClient } from '@tanstack/react-query';
import { Download, Link2, Monitor, Shield, Trash2, User, X } from 'lucide-react';
import { useT } from '../i18n';
import { PageShell } from '../components/Layout';
import { Avatar, Field, SubmitButton, useToast } from '../components/ui/Kit';
import { useAuth, authMode, AuthError } from '../lib/auth';
import { ApiError, useApi, useDevices, useFriends, useLinks, useMe, useUpdateMe } from '../lib/api';
import { Seo } from '../lib/seo';
import { HANDLE_RE, maskEmail, passwordScore, type Me, type ProfilePatch } from '../../shared/schemas';
import { authErrorText } from '../components/AuthBits';

const SECTIONS = [
  { id: 'profile', icon: User, key: 'acct.sec.profile' },
  { id: 'security', icon: Shield, key: 'acct.sec.security' },
  { id: 'devices', icon: Monitor, key: 'acct.sec.devices' },
  { id: 'links', icon: Link2, key: 'acct.sec.links' },
  { id: 'data', icon: Download, key: 'acct.sec.data' },
] as const;
type SectionId = (typeof SECTIONS)[number]['id'];

type T = ReturnType<typeof useT>['t'];

function Row({ title, desc, children, danger }: { title: ReactNode; desc?: ReactNode; children?: ReactNode; danger?: boolean }) {
  return (
    <div className="row">
      <div className="rl">
        <span className={`rt ${danger ? 'text-danger' : ''}`}>{title}</span>
        {desc && <span className="rd">{desc}</span>}
      </div>
      {children && <div className="rc">{children}</div>}
    </div>
  );
}

function when(iso: string | null | undefined, t: T, lang: string) {
  if (!iso) return '';
  const ms = Date.now() - new Date(iso).getTime();
  const min = Math.floor(ms / 60000);
  if (min < 20) return t('acct.activeNow');
  if (min < 60) return t('link.minutesAgo', { n: min });
  const h = Math.floor(min / 60);
  if (h < 24) return t('acct.hoursAgo', { n: h });
  const d = Math.floor(h / 24);
  if (d < 30) return t('acct.daysAgo', { n: d });
  return new Date(iso).toLocaleDateString(lang === 'th' ? 'th-TH' : 'en-GB', { day: 'numeric', month: 'short', year: 'numeric' });
}

/** saves a patch and says "Saved"; an error says so instead (the field keeps what was typed) */
function useSave() {
  const update = useUpdateMe();
  const toast = useToast();
  const { t } = useT();
  return async (patch: ProfilePatch) => {
    try {
      await update.mutateAsync(patch);
      toast(t('acct.saved'));
      return true;
    } catch (e) {
      toast(e instanceof ApiError && e.code === 'handle_taken' ? t('acct.handleTaken') : t('acct.offline'));
      return false;
    }
  };
}

// --- Profile -------------------------------------------------------------------------------------

function useHandleCheck(value: string, current: string) {
  const api = useApi();
  const [state, setState] = useState<'same' | 'checking' | 'ok' | 'taken' | 'short' | 'chars'>('same');
  useEffect(() => {
    const h = value.trim().toLowerCase();
    if (h === current) { setState('same'); return; }
    if (h.length < 3) { setState('short'); return; }
    if (!HANDLE_RE.test(h)) { setState('chars'); return; }
    setState('checking');
    const id = window.setTimeout(async () => {
      try {
        const r = await api<{ available: boolean }>('GET', `/handle/available?h=${encodeURIComponent(h)}`);
        setState(r.available ? 'ok' : 'taken');
      } catch {
        setState('taken');
      }
    }, 350);
    return () => window.clearTimeout(id);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [value, current]);
  return state;
}

function Profile({ me }: { me: Me }) {
  const { t } = useT();
  const save = useSave();
  const api = useApi();
  const qc = useQueryClient();
  const toast = useToast();
  const [name, setName] = useState(me.displayName);
  const [handle, setHandle] = useState(me.handle);
  const [about, setAbout] = useState(me.about);
  const [photoBusy, setPhotoBusy] = useState(false);
  const [photoErr, setPhotoErr] = useState<string | null>(null);
  const file = useRef<HTMLInputElement>(null);
  const check = useHandleCheck(handle, me.handle);

  // autosave after typing stops
  useEffect(() => {
    const v = name.trim();
    if (v === me.displayName || v.length < 1 || v.length > 40) return;
    const id = window.setTimeout(() => save({ displayName: v }), 700);
    return () => window.clearTimeout(id);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [name]);
  useEffect(() => {
    if (about === me.about || about.length > 160) return;
    const id = window.setTimeout(() => save({ about }), 700);
    return () => window.clearTimeout(id);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [about]);
  useEffect(() => {
    if (check !== 'ok') return;
    save({ handle: handle.trim().toLowerCase() });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [check]);

  const upload = async (f: File) => {
    setPhotoErr(null);
    if (f.size > 5 * 1024 * 1024) { setPhotoErr(t('acct.photoTooBig')); return; }
    if (!/^image\/(jpeg|png|webp)$/.test(f.type)) { setPhotoErr(t('acct.photoType')); return; }
    const fd = new FormData();
    fd.append('photo', f);
    setPhotoBusy(true);
    try {
      const me2 = await api<Me>('POST', '/me/avatar', fd);
      qc.setQueriesData({ queryKey: ['me'] }, me2);
      toast(t('acct.saved'));
    } catch (e) {
      setPhotoErr(e instanceof ApiError && e.code === 'bad_image' ? t('acct.photoType') : t('acct.offline'));
    } finally {
      setPhotoBusy(false);
    }
  };
  const removePhoto = async () => {
    setPhotoBusy(true);
    try {
      const me2 = await api<Me>('DELETE', '/me/avatar');
      qc.setQueriesData({ queryKey: ['me'] }, me2);
      toast(t('acct.saved'));
    } catch { toast(t('acct.offline')); } finally { setPhotoBusy(false); }
  };

  const handleHint =
    check === 'ok' ? <span className="text-ok">{t('acct.handleOk')}</span>
    : check === 'taken' ? <span className="text-danger">{t('acct.handleTaken')}</span>
    : check === 'short' ? <span className="text-danger">{t('acct.handleShort')}</span>
    : check === 'chars' ? <span className="text-danger">{t('acct.handleChars')}</span>
    : t('acct.handleCap');

  return (
    <>
      <p className="sec-lead">{t('acct.profileLead')}</p>
      <div className="row">
        <div className="flex items-center gap-5 flex-wrap">
          <Avatar name={me.displayName || me.handle} url={me.avatarUrl} size={96} />
          <div className="flex flex-col gap-2">
            <div className="flex gap-2 flex-wrap">
              <SubmitButton type="button" busy={photoBusy} className="" onClick={() => file.current?.click()}>{t('acct.changePhoto')}</SubmitButton>
              {me.avatarUrl && <button type="button" className="btn" disabled={photoBusy} onClick={removePhoto}>{t('acct.remove')}</button>}
            </div>
            <span className="rd">{t('acct.photoCap')}</span>
            {photoErr && <span className="field-err" role="alert">{photoErr}</span>}
          </div>
          <input ref={file} type="file" accept="image/jpeg,image/png,image/webp" hidden
                 onChange={e => { const f = e.target.files?.[0]; e.target.value = ''; if (f) upload(f); }} />
        </div>
      </div>
      <Row title={<label htmlFor="f-name">{t('acct.name')}</label>} desc={t('acct.nameCap')}>
        <input id="f-name" className="inp" value={name} maxLength={40} autoComplete="nickname" onChange={e => setName(e.target.value)}
               aria-invalid={name.trim().length === 0 || undefined} />
      </Row>
      <Row title={<label htmlFor="f-handle">{t('acct.handle')}</label>} desc={<span aria-live="polite">{handleHint}</span>}>
        <span className="inp-wrap w-full">
          <span className="inp-prefix" aria-hidden="true">@</span>
          <input id="f-handle" className="inp has-prefix" value={handle} maxLength={20} autoComplete="off" spellCheck={false}
                 onChange={e => setHandle(e.target.value.toLowerCase())}
                 aria-invalid={check === 'taken' || check === 'short' || check === 'chars' || undefined} />
        </span>
      </Row>
      <Row title={<label htmlFor="f-about">{t('acct.about')}</label>} desc={<>{t('acct.aboutCap')} · {about.length} / 160</>}>
        <textarea id="f-about" className="ta" value={about} maxLength={160} rows={3} onChange={e => setAbout(e.target.value)} />
      </Row>
      <Row title={t('acct.language')} desc={t('acct.languageCap')}>
        <div className="seg" role="radiogroup" aria-label={t('acct.language')}>
          {(['en', 'th'] as const).map(l => (
            <button key={l} type="button" role="radio" aria-checked={me.locale === l} className={me.locale === l ? 'on' : ''}
                    onClick={() => me.locale !== l && save({ locale: l })}>
              {l === 'en' ? 'English' : 'ไทย'}
            </button>
          ))}
        </div>
      </Row>
    </>
  );
}

// --- Sign-in and security ------------------------------------------------------------------------

function Security({ me }: { me: Me }) {
  const { t, lang } = useT();
  const auth = useAuth();
  const api = useApi();
  const navigate = useNavigate();
  const toast = useToast();
  const [showEmail, setShowEmail] = useState(false);
  const [editing, setEditing] = useState<null | 'email' | 'password'>(null);
  const [value, setValue] = useState('');
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string | null>(null);
  const [emailSent, setEmailSent] = useState<string | null>(null);
  const has = (p: string) => me.providers.includes(p);
  const canUnlink = me.providers.length > 1;

  const submit = async () => {
    setBusy(true); setErr(null);
    try {
      if (editing === 'email') { await auth.changeEmail(value.trim()); setEmailSent(value.trim()); }
      else { await auth.setPassword(value); toast(t('acct.saved')); }
      setEditing(null); setValue('');
    } catch (e) {
      setErr(authErrorText(t, e instanceof AuthError ? e : new AuthError('failed')));
    } finally { setBusy(false); }
  };

  const inline = (kind: 'email' | 'password') => editing === kind && (
    <form className="inline-form" onSubmit={e => { e.preventDefault(); submit(); }}>
      <Field label={kind === 'email' ? t('acct.newEmail') : t('acct.newPassword')} type={kind === 'email' ? 'email' : 'password'}
             autoComplete={kind === 'email' ? 'email' : 'new-password'} value={value} onChange={e => setValue(e.target.value)} autoFocus
             error={err} hint={kind === 'password' ? (passwordScore(value).missing > 0 ? t('signup.pwMore', { n: passwordScore(value).missing }) : t('signup.pwHint')) : undefined} />
      <div className="flex gap-2">
        <SubmitButton busy={busy} disabled={kind === 'password' ? value.length < 10 : !value.includes('@')}>{kind === 'email' ? t('acct.sendConfirm') : t('reset.submit')}</SubmitButton>
        <button type="button" className="btn" onClick={() => { setEditing(null); setErr(null); setValue(''); }}>{t('acct.cancel')}</button>
      </div>
    </form>
  );

  return (
    <>
      <Row title={t('acct.email')} desc={emailSent ? t('acct.emailSent', { email: emailSent }) : undefined}>
        <span className="text-[15px] break-all">{showEmail ? me.email : maskEmail(me.email)}</span>
        <button type="button" className="btn small" onClick={() => setShowEmail(v => !v)}>{showEmail ? t('acct.hide') : t('acct.show')}</button>
        {has('email') && <button type="button" className="btn small" onClick={() => { setEditing('email'); setValue(''); }}>{t('acct.change')}</button>}
      </Row>
      {inline('email')}
      <Row title={t('acct.password')} desc={me.passwordChangedAt ? t('acct.passwordChanged', { when: when(me.passwordChangedAt, t, lang).toLowerCase() }) : undefined}>
        <button type="button" className="btn" onClick={() => { setEditing('password'); setValue(''); }}>{t('acct.changePassword')}</button>
      </Row>
      {inline('password')}
      <Row title="Google" desc={has('google') ? t('acct.connected') : t('acct.notConnected')}>
        {has('google')
          ? <button type="button" className="btn" disabled={!canUnlink} title={canUnlink ? undefined : t('acct.lastWay')}
                    onClick={() => auth.unlinkProvider('google').then(() => window.location.reload()).catch(() => toast(t('acct.offline')))}>{t('acct.disconnect')}</button>
          : <span className="rd">{t('acct.googleHow')}</span>}
      </Row>
      <Row title="Discord" desc={has('discord') ? t('acct.connected') : t('acct.notConnected')}>
        {has('discord')
          ? <button type="button" className="btn" disabled={!canUnlink} title={canUnlink ? undefined : t('acct.lastWay')}
                    onClick={() => auth.unlinkProvider('discord').then(() => window.location.reload()).catch(() => toast(t('acct.offline')))}>{t('acct.disconnect')}</button>
          : <button type="button" className="btn" disabled={authMode !== 'supabase'} onClick={() => auth.linkDiscord().catch(() => toast(t('acct.offline')))}>{t('acct.connect')}</button>}
      </Row>
      <Row title={t('acct.signOutAll')} desc={t('acct.signOutAllCap')}>
        <button type="button" className="btn danger" onClick={async () => {
          await api('POST', '/me/devices/revoke-all').catch(() => {});
          await auth.signOut(true);
          navigate('/');
        }}>{t('acct.signOutAll')}</button>
      </Row>
    </>
  );
}

// --- Devices -------------------------------------------------------------------------------------

const PLUGIN_NAMES: Record<string, string> = { hub: 'HEARASIDE Hub', track: 'HEARASIDE Track', app: 'HEARASIDE App Audio' };

function Devices() {
  const { t, lang } = useT();
  const api = useApi();
  const qc = useQueryClient();
  const toast = useToast();
  const q = useDevices();
  const list = q.data?.devices ?? [];
  return (
    <>
      <p className="sec-lead">{t('acct.devicesLead')}</p>
      {q.isLoading ? <div className="spin" aria-hidden="true" /> : list.length === 0 ? <p className="rd py-4">{t('acct.noDevices')}</p> : (
        <div className="flex flex-col gap-2.5 pt-2">
          {list.map(d => {
            const w = when(d.lastSeenAt, t, lang);
            const active = w === t('acct.activeNow');
            return (
              <div key={d.id} className="dev">
                <span className="tile"><Monitor size={18} strokeWidth={1.8} aria-hidden="true" /></span>
                <div className="flex-1 min-w-0">
                  <div className="text-[15px] font-medium truncate">{PLUGIN_NAMES[d.plugin] ?? 'HEARASIDE'} · {d.name}</div>
                  <div className="text-[13px] muted">{[d.daw, d.os, d.pluginVersion && `version ${d.pluginVersion}`].filter(Boolean).join(' · ')}</div>
                  <div className="text-[13px] flex items-center gap-1.5 pt-0.5">
                    {active && <span className="dot-ok" aria-hidden="true" />}<span className={active ? '' : 'muted'}>{w}</span>
                  </div>
                </div>
                <button type="button" className="btn" onClick={async () => {
                  try { await api('DELETE', `/me/devices/${d.id}`); qc.invalidateQueries({ queryKey: ['devices'] }); toast(t('acct.saved')); }
                  catch { toast(t('acct.offline')); }
                }}>{t('acct.signOut')}</button>
              </div>
            );
          })}
        </div>
      )}
    </>
  );
}

// --- Links and friends ---------------------------------------------------------------------------

function Links() {
  const { t } = useT();
  const api = useApi();
  const qc = useQueryClient();
  const toast = useToast();
  const links = useLinks();
  const friends = useFriends();
  const [copied, setCopied] = useState<string | null>(null);
  const listen = (links.data?.links ?? []).filter(l => l.kind === 'listen');
  return (
    <>
      <p className="sec-lead">{t('acct.linksLead')}</p>
      {listen.length === 0 ? <Row title={t('acct.listenLink')} desc={t('acct.noLink')} /> : listen.map(l => (
        <Row key={l.id} title={t('acct.listenLink')} desc={<span className="break-all font-mono text-[12.5px]">{l.url ?? l.label}</span>}>
          {l.url && <button type="button" className="btn" onClick={async () => {
            try { await navigator.clipboard.writeText(l.url!); setCopied(l.id); window.setTimeout(() => setCopied(null), 1600); } catch { /* no clipboard */ }
          }}>{copied === l.id ? t('acct.copied') : t('acct.copy')}</button>}
          <button type="button" className="btn danger" onClick={async () => {
            try { await api('DELETE', `/me/links/${l.id}`); qc.invalidateQueries({ queryKey: ['links'] }); toast(t('acct.saved')); }
            catch { toast(t('acct.offline')); }
          }}>{t('acct.revoke')}</button>
        </Row>
      ))}
      <Row title={t('acct.savedFriends')} desc={t('acct.savedFriendsCap')} />
      <div className="flex flex-wrap gap-2 pb-2">
        {(friends.data?.friends ?? []).length === 0 && <span className="rd">{t('acct.noFriends')}</span>}
        {(friends.data?.friends ?? []).map(f => (
          <span key={f.id} className="friendchip">
            <Avatar name={f.name} size={26} />{f.name}
            <button type="button" className="chip-x" aria-label={t('acct.removeName', { name: f.name })} onClick={async () => {
              try { await api('DELETE', `/me/friends/${f.id}`); qc.invalidateQueries({ queryKey: ['friends'] }); }
              catch { toast(t('acct.offline')); }
            }}><X size={14} aria-hidden="true" /></button>
          </span>
        ))}
      </div>
    </>
  );
}

// --- Data and privacy ----------------------------------------------------------------------------

function Data({ me }: { me: Me }) {
  const { t, path } = useT();
  const api = useApi();
  const auth = useAuth();
  const navigate = useNavigate();
  const toast = useToast();
  const [confirm, setConfirm] = useState('');
  const [busy, setBusy] = useState(false);
  const download = async () => {
    try {
      const data = await api<unknown>('GET', '/me/export');
      const url = URL.createObjectURL(new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' }));
      const a = document.createElement('a');
      a.href = url; a.download = `hearaside-${me.handle}.json`; a.click();
      window.setTimeout(() => URL.revokeObjectURL(url), 2000);
    } catch { toast(t('acct.offline')); }
  };
  const del = async () => {
    setBusy(true);
    try {
      await api('POST', '/me/delete', { confirm });
      await auth.signOut(true).catch(() => {});
      navigate(path('/'));
    } catch { toast(t('acct.offline')); setBusy(false); }
  };
  return (
    <>
      <p className="sec-lead">{t('acct.dataLead')} <Link to={path('/privacy')}>{t('acct.privacyLink')}</Link></p>
      <Row title={t('acct.download')} desc={t('acct.downloadCap')}>
        <button type="button" className="btn" onClick={download}><Download size={16} aria-hidden="true" />{t('acct.downloadButton')}</button>
      </Row>
      <Row title={t('acct.delete')} desc={t('acct.deleteCap')} danger>
        <div className="flex flex-col gap-2 w-full">
          <input className="inp" value={confirm} onChange={e => setConfirm(e.target.value)} placeholder={t('acct.typeToConfirm', { handle: me.handle })}
                 aria-label={t('acct.typeToConfirm', { handle: me.handle })} autoComplete="off" spellCheck={false} />
          <SubmitButton type="button" busy={busy} className="danger" disabled={confirm.trim().toLowerCase() !== me.handle} onClick={del}>
            <Trash2 size={16} aria-hidden="true" />{t('acct.deleteButton')}
          </SubmitButton>
        </div>
      </Row>
    </>
  );
}

// -------------------------------------------------------------------------------------------------

export default function Account() {
  const { t, path } = useT();
  const auth = useAuth();
  const { section = 'profile' } = useParams();
  const me = useMe();
  const navigate = useNavigate();
  const sec = (SECTIONS.some(s => s.id === section) ? section : 'profile') as SectionId;

  if (auth.status === 'signedOut') return <Navigate to={`/login?next=${encodeURIComponent(path(`/account${section === 'profile' ? '' : `/${section}`}`))}`} replace />;

  const title = t(SECTIONS.find(s => s.id === sec)!.key);
  return (
    <PageShell>
      <Seo title={`${t('acct.h1')} · HEARASIDE`} noindex />
      <div className="wrap pt-10 pb-20">
        <header className="flex flex-col gap-2 pb-7">
          <h1 className="h2">{t('acct.h1')}</h1>
          <p className="m-0 text-[15px] text-ink2">{t('acct.lead')}</p>
        </header>
        {!auth.configured && <p className="banner-warn" role="status">{t('auth.notConfigured')}</p>}
        <div className="acct-grid">
          <nav className="card acct-menu" aria-label={t('acct.h1')}>
            {SECTIONS.map(s => (
              <button key={s.id} type="button" className={`navbtn ${s.id === sec ? 'on' : ''}`} aria-current={s.id === sec ? 'page' : undefined}
                      onClick={() => navigate(path(s.id === 'profile' ? '/account' : `/account/${s.id}`))}>
                <s.icon size={18} strokeWidth={1.8} aria-hidden="true" />{t(s.key)}
              </button>
            ))}
            <div className="acct-menu-foot">
              <button type="button" className="linkbtn" onClick={async () => { await auth.signOut(false); navigate(path('/')); }}>{t('acct.signOutBrowser')}</button>
            </div>
          </nav>
          <section className="card acct-body" aria-labelledby="acct-sec">
            <h2 id="acct-sec" className="h4 pb-1">{title}</h2>
            {me.isLoading || auth.status === 'loading' ? <div className="spin my-6" aria-hidden="true" />
              : me.isError || !me.data ? <p className="alert py-4" role="alert">{t('acct.offline')}</p>
              : sec === 'profile' ? <Profile key={me.data.id} me={me.data} />
              : sec === 'security' ? <Security me={me.data} />
              : sec === 'devices' ? <Devices />
              : sec === 'links' ? <Links />
              : <Data me={me.data} />}
          </section>
        </div>
      </div>
    </PageShell>
  );
}
