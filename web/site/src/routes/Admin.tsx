// "/admin": the back office for the owner. Overview, downloads (with a back-fill from GitHub Releases)
// and people. The page only renders for addresses in ADMIN_EMAILS; the API enforces that again on
// every call (server/routes/admin.ts), so hiding things here is a convenience, not the lock.
import { useEffect, useRef, useState, type ReactNode } from 'react';
import { Link, Navigate, useLocation, useNavigate, useParams } from 'react-router';
import { useMutation, useQueryClient } from '@tanstack/react-query';
import { ArrowUpRight, Download, LayoutDashboard, RefreshCw, Users, X } from 'lucide-react';
import { PageShell } from '../components/Layout';
import { Field, SubmitButton, useToast } from '../components/ui/Kit';
import { Seo } from '../lib/seo';
import { useAuth } from '../lib/auth';
import { useT } from '../i18n';
import { ApiError, useAdminQuery, useApi, useAdminAccess, useMe, type AdminDownloads, type AdminOverview, type AdminUser } from '../lib/api';
import '../styles/admin.css';

const SECTIONS = [
  { id: 'overview', label: 'ภาพรวม', description: 'บัญชีและการใช้งาน', icon: LayoutDashboard },
  { id: 'downloads', label: 'ยอดดาวน์โหลด', description: 'เวอร์ชันและแหล่งที่มา', icon: Download },
  { id: 'users', label: 'ผู้ใช้', description: 'ค้นหาและจัดการสิทธิ์', icon: Users },
] as const;
type SectionId = (typeof SECTIONS)[number]['id'];

const n = (v: number) => v.toLocaleString('en-US');
const date = (iso: string | null) => (iso ? new Date(iso).toLocaleDateString('th-TH', { day: 'numeric', month: 'short', year: '2-digit', timeZone: 'Asia/Bangkok' }) : 'ยังไม่มี');

function Stat({ value, label }: { value: ReactNode; label: string }) {
  return <div className="stat"><b>{value}</b><span>{label}</span></div>;
}

/** one bar per day; `title` on each bar gives the exact number on hover */
function Bars({ data, label }: { data: { day: string; count: number }[]; label: string }) {
  const max = Math.max(1, ...data.map(d => d.count));
  const w = data.length ? 100 / data.length : 0;
  return (
    <div className="adm-chart">
    <svg className="chart" viewBox="0 0 100 40" preserveAspectRatio="none" role="img" aria-label={`${label}: ${data.reduce((a, d) => a + d.count, 0)}`}>
      {data.map((d, i) => {
        const h = d.count === 0 ? 0.6 : Math.max(1.5, (d.count / max) * 36);
        return (
          <rect key={d.day} className={d.count === 0 ? 'zero' : undefined} x={i * w + w * 0.12} y={38 - h} width={w * 0.76} height={h} rx="0.5">
            <title>{`${d.day}: ${d.count}`}</title>
          </rect>
        );
      })}
    </svg>
      <div className="chart-axis"><span>{date(data[0]?.day ?? null)}</span><span>รวม {n(data.reduce((sum, d) => sum + d.count, 0))}</span><span>{date(data.at(-1)?.day ?? null)}</span></div>
      <details className="chart-data"><summary>ดูตัวเลขรายวัน</summary><div className="tbl-wrap" tabIndex={0} role="region" aria-label={`${label}รายวัน`}><table className="tbl"><caption className="sr-only">{label}รายวัน</caption><thead><tr><th scope="col">วันที่ (UTC)</th><th scope="col" className="n">จำนวน</th></tr></thead><tbody>{data.map(d => <tr key={d.day}><td>{d.day}</td><td className="n">{n(d.count)}</td></tr>)}</tbody></table></div></details>
    </div>
  );
}

function Loading({ q }: { q: { isLoading: boolean; isError: boolean; refetch(): unknown } }) {
  if (q.isLoading) return <p className="query-feedback" role="status"><span className="spin" aria-hidden="true" />กำลังโหลดข้อมูล</p>;
  if (q.isError) return <div className="adm-error" role="alert"><p>โหลดข้อมูลไม่ได้ ลองใหม่อีกครั้ง</p><button type="button" className="btn" onClick={() => { void q.refetch(); }}>ลองใหม่</button></div>;
  return null;
}

function Updated({ query }: { query: { dataUpdatedAt: number; isFetching: boolean; refetch(): unknown } }) {
  return <div className="adm-updated"><span role="status">{query.isFetching ? 'กำลังอัปเดตข้อมูล' : `ข้อมูลเมื่อ ${new Date(query.dataUpdatedAt).toLocaleTimeString('th-TH', { hour: '2-digit', minute: '2-digit', timeZone: 'Asia/Bangkok' })} (เวลาไทย)`}</span><button type="button" className="btn small" disabled={query.isFetching} onClick={() => { void query.refetch(); }}><RefreshCw size={15} aria-hidden="true" />รีเฟรช</button></div>;
}

type UserAction = 'revoke-devices' | 'revoke-links' | 'delete';
const ACTION_TEXT: Record<UserAction, string> = { 'revoke-devices': 'ออกจากระบบปลั๊กอินทุกเครื่อง', 'revoke-links': 'เพิกถอนลิงก์ถาวรทั้งหมด', delete: 'ลบบัญชี' };

function ConfirmAction({ target, busy, error, onCancel, onConfirm }: {
  target: { user: AdminUser; what: UserAction } | null; busy: boolean; error: boolean; onCancel(): void; onConfirm(): void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [confirmation, setConfirmation] = useState('');
  useEffect(() => {
    setConfirmation('');
    if (target) dialog.current?.showModal();
    else dialog.current?.close();
  }, [target]);
  const deleting = target?.what === 'delete';
  return <dialog ref={dialog} className="adm-dialog" aria-labelledby="action-title" onCancel={event => { event.preventDefault(); if (!busy) onCancel(); }}>
    {target && <>
      <div className="adm-dialog-heading"><h2 id="action-title" className="h4">{ACTION_TEXT[target.what]}</h2><button type="button" className="iconbtn" aria-label="ปิด" disabled={busy} onClick={onCancel}><X size={18} aria-hidden="true" /></button></div>
      <div className="adm-target"><strong>{target.user.displayName || target.user.handle}</strong><span>@{target.user.handle}</span><span>{target.user.email}</span></div>
      <p className="adm-note">{deleting ? 'บัญชีจะใช้งานไม่ได้ ปลั๊กอินถูกออกจากระบบและลิงก์ถูกเพิกถอน ข้อมูลที่เหลือจะถูกลบภายใน 30 วัน' : target.what === 'revoke-devices' ? `ปลั๊กอิน ${n(target.user.devices)} เครื่องต้องเข้าสู่ระบบใหม่ การตั้งค่าเสียงในโปรเจกต์ไม่ถูกเปลี่ยน` : `ลิงก์ ${n(target.user.links)} รายการจะใช้ไม่ได้ ผู้ใช้ต้องสร้างลิงก์ใหม่ใน Hub`}</p>
      {error && <p className="alert" role="alert">ดำเนินการไม่สำเร็จ ลองใหม่ได้โดยข้อมูลในรายการยังอยู่</p>}
      <form onSubmit={event => { event.preventDefault(); if (!busy && (!deleting || confirmation === target.user.handle)) onConfirm(); }}>
        {deleting && <Field label={`พิมพ์ ${target.user.handle} เพื่อยืนยัน`} name="confirm-delete" autoComplete="off" value={confirmation} onChange={event => setConfirmation(event.target.value)} />}
        <div className="adm-dialog-actions"><button type="button" autoFocus className="btn" disabled={busy} onClick={onCancel}>ยกเลิก</button><SubmitButton className="danger" busy={busy} disabled={deleting && confirmation !== target.user.handle}>{ACTION_TEXT[target.what]}</SubmitButton></div>
      </form>
    </>}
  </dialog>;
}

function Overview() {
  const { path } = useT();
  const q = useAdminQuery<AdminOverview>('overview', '/admin/overview');
  if (!q.data) return <Loading q={q} />;
  const { counts, downloads } = q.data;
  const signupDays = new Map(counts.signups.map(s => [s.day, s.count]));
  const signups = downloads.daily.map(d => ({ day: d.day, count: signupDays.get(d.day) ?? 0 }));
  return (
    <>
      <Updated query={q} />
      {q.isError && <Loading q={q} />}
      <div className="stats">
        <Stat value={n(downloads.total)} label="ดาวน์โหลดทั้งหมด" />
        <Stat value={n(downloads.last30)} label="ดาวน์โหลด 30 วันล่าสุด" />
        <Stat value={n(counts.users - counts.deleted)} label="บัญชีที่ใช้งาน" />
        <Stat value={n(counts.activeDevices7d)} label={`ใช้ปลั๊กอินใน 7 วัน (จาก ${n(counts.devices)})`} />
        <Stat value={n(counts.links)} label="ลิงก์ถาวรที่เปิดอยู่" />
        <Stat value={n(counts.deleted)} label="บัญชีที่ลบแล้ว" />
      </div>
      <div className="adm-chart-grid">
        <section className="adm-panel" aria-labelledby="c1"><h3 id="c1" className="h4">ดาวน์โหลดต่อวัน</h3><p className="adm-note">30 วันล่าสุด · วันตาม UTC</p><Bars data={downloads.daily} label="ดาวน์โหลด" /></section>
        <section className="adm-panel" aria-labelledby="c2"><h3 id="c2" className="h4">บัญชีใหม่</h3><p className="adm-note">30 วันล่าสุด · วันตาม UTC</p><Bars data={signups} label="สมัครใหม่" /></section>
      </div>
      <div className="adm-panel adm-shortcut"><div><h3 className="h4">จัดการบัญชีและปลั๊กอิน</h3><p className="adm-note">ค้นหาผู้ใช้ ออกจากระบบปลั๊กอิน หรือเพิกถอนลิงก์ที่แชร์</p></div><Link className="btn" to={path('/admin/users')}>ไปที่ผู้ใช้<ArrowUpRight size={16} aria-hidden="true" /></Link></div>
    </>
  );
}

function DownloadsPage() {
  const q = useAdminQuery<AdminDownloads>('downloads', '/admin/downloads');
  const api = useApi();
  const qc = useQueryClient();
  const toast = useToast();
  const [version, setVersion] = useState('');
  const [count, setCount] = useState('');
  const versionValid = /^\d+\.\d+\.\d+$/.test(version.trim());
  const countValid = count !== '' && Number.isSafeInteger(Number(count)) && Number(count) <= 100_000_000;
  const refresh = () => qc.invalidateQueries({ queryKey: ['admin'] });

  const importGh = useMutation({
    mutationFn: () => api<{ imported: { version: string; count: number }[] }>('POST', '/admin/downloads/import-github'),
    onSuccess: r => { toast(`ดึงจาก GitHub แล้ว ${r.imported.length} เวอร์ชัน`); void refresh(); },
    onError: e => toast(e instanceof ApiError && e.code === 'github_unreachable' ? 'ติดต่อ GitHub ไม่ได้' : 'ไม่สำเร็จ'),
  });
  const setBase = useMutation({
    mutationFn: () => api('PUT', '/admin/downloads/base', { version: version.trim(), count: Number(count) }),
    onSuccess: () => { toast('บันทึกแล้ว'); setVersion(''); setCount(''); void refresh(); },
    onError: () => toast('เวอร์ชันหรือตัวเลขไม่ถูกต้อง'),
  });

  if (!q.data) return <Loading q={q} />;
  const d = q.data;
  return (
    <>
      <Updated query={q} />
      {q.isError && <Loading q={q} />}
      <div className="stats">
        <Stat value={n(d.total)} label="รวมทั้งหมด" />
        <Stat value={n(d.counted)} label="นับจากเว็บนี้ (ตั้งแต่เริ่มระบบ)" />
        <Stat value={n(d.before)} label="ก่อนเริ่มนับ (GitHub / ใส่เอง)" />
      </div>
      <section className="adm-panel" aria-labelledby="d1"><h3 id="d1" className="h4 pb-2">ดาวน์โหลดต่อวัน (60 วัน)</h3><Bars data={d.daily} label="ดาวน์โหลด" /></section>
      <section aria-labelledby="d2" className="flex flex-col gap-3">
        <h3 id="d2" className="h4">ต่อเวอร์ชัน</h3>
        <div className="tbl-wrap" tabIndex={0} role="region" aria-label="ยอดดาวน์โหลดแยกเวอร์ชัน">
          <table className="tbl">
            <thead><tr><th>เวอร์ชัน</th><th className="n">นับจากเว็บ</th><th className="n">GitHub</th><th className="n">ใส่เอง</th><th className="n">รวม</th></tr></thead>
            <tbody>
              {d.versions.map(v => (
                <tr key={v.version}>
                  <td>{v.version}</td><td className="n">{n(v.site)}</td><td className="n">{n(v.github)}</td><td className="n">{n(v.manual)}</td><td className="n"><b>{n(v.total)}</b></td>
                </tr>
              ))}
              {d.versions.length === 0 && <tr><td colSpan={5} className="muted">ยังไม่มีข้อมูล กด “ดึงยอดย้อนหลังจาก GitHub” ด้านล่าง</td></tr>}
            </tbody>
          </table>
        </div>
        <p className="adm-note">ยอดรวมคือยอดจากเว็บ บวกยอดก่อนเริ่มนับ ระบบเลือกยอดที่มากกว่าระหว่าง GitHub กับที่ใส่เอง จึงไม่นับสองยอดนี้ซ้ำกัน</p>
      </section>
      <section aria-labelledby="d3" className="adm-panel flex flex-col gap-3">
        <h3 id="d3" className="h4">ยอดย้อนหลังก่อนมีระบบนี้</h3>
        <div><SubmitButton type="button" className="solid" busy={importGh.isPending} onClick={() => importGh.mutate()}>ดึงยอดย้อนหลังจาก GitHub Releases</SubmitButton></div>
        {importGh.isError && <p className="alert" role="alert">ดึงข้อมูลจาก GitHub ไม่สำเร็จ ลองใหม่ได้โดยยอดเดิมยังอยู่</p>}
        <p className="adm-note">
          ดึงยอดดาวน์โหลดตัวติดตั้งจาก GitHub กดซ้ำได้โดยไม่บวกซ้ำ หรือกรอกยอดย้อนหลังเองด้านล่าง
        </p>
        <form className="adm-bar" onSubmit={e => { e.preventDefault(); setBase.mutate(); }}>
          <Field label="เวอร์ชัน" name="version" placeholder="1.0.5" value={version} error={version && !versionValid ? 'ใช้รูปแบบเวอร์ชัน เช่น 1.0.10' : undefined} onChange={e => setVersion(e.target.value)} />
          <Field label="จำนวนก่อนเริ่มนับ" name="count" inputMode="numeric" placeholder="120" value={count} error={count && !countValid ? 'ใส่จำนวนตั้งแต่ 0 ถึง 100,000,000' : undefined} onChange={e => setCount(e.target.value.replace(/\D/g, ''))} />
          <SubmitButton busy={setBase.isPending} disabled={!versionValid || !countValid}>บันทึก</SubmitButton>
        </form>
        {setBase.isError && <p className="alert" role="alert">บันทึกไม่สำเร็จ ตรวจการเชื่อมต่อแล้วลองใหม่</p>}
      </section>
    </>
  );
}

function UsersPage() {
  const [q, setQ] = useState('');
  const [term, setTerm] = useState('');
  const [page, setPage] = useState(0);
  const res = useAdminQuery<{ total: number; page: number; size: number; users: AdminUser[] }>('users', `/admin/users?q=${encodeURIComponent(term)}&page=${page}`);
  const api = useApi();
  const qc = useQueryClient();
  const toast = useToast();
  const me = useMe();
  const [pending, setPending] = useState<{ user: AdminUser; what: UserAction } | null>(null);
  const act = useMutation({
    mutationFn: ({ id, what }: { id: string; what: UserAction }) =>
      what === 'delete' ? api('DELETE', `/admin/users/${id}`) : api('POST', `/admin/users/${id}/${what}`),
    onSuccess: () => { setPending(null); toast('เรียบร้อย'); void qc.invalidateQueries({ queryKey: ['admin'] }); },
    onError: e => toast(e instanceof ApiError && e.code === 'not_yourself' ? 'ลบบัญชีตัวเองไม่ได้' : 'ไม่สำเร็จ'),
  });
  const ask = (u: AdminUser, what: 'revoke-devices' | 'revoke-links' | 'delete') => {
    act.reset();
    setPending({ user: u, what });
  };
  return (
    <>
      <ConfirmAction target={pending} busy={act.isPending} error={act.isError} onCancel={() => { setPending(null); act.reset(); }} onConfirm={() => { if (pending) act.mutate({ id: pending.user.id, what: pending.what }); }} />
      <form className="adm-bar" onSubmit={e => { e.preventDefault(); setPage(0); setTerm(q.trim()); }}>
        <Field label="ค้นหา" name="q" placeholder="อีเมล, username หรือชื่อ" maxLength={80} value={q} onChange={e => setQ(e.target.value)} />
        <SubmitButton busy={res.isFetching}>ค้นหา</SubmitButton>
        {term && <button type="button" className="btn" onClick={() => { setQ(''); setTerm(''); setPage(0); }}>ล้างการค้นหา</button>}
      </form>
      {!res.data ? <Loading q={res} /> : (
        <>
          <Updated query={res} />
          {res.isError && <Loading q={res} />}
          <div className="tbl-wrap">
            <table className="tbl users-table">
              <caption className="sr-only">บัญชีผู้ใช้และการเชื่อมต่อปลั๊กอิน</caption>
              <thead><tr><th scope="col">ผู้ใช้</th><th scope="col">สมัครเมื่อ</th><th scope="col">ใช้งานปลั๊กอินล่าสุด</th><th scope="col" className="n">ปลั๊กอิน</th><th scope="col" className="n">ลิงก์</th><th scope="col"><span className="sr-only">จัดการ</span></th></tr></thead>
              <tbody>
                {res.data.users.map(u => (
                  <tr key={u.id}>
                    <td>
                      <div>{u.displayName || u.handle} <span className="muted">@{u.handle}</span> {u.deletedAt && <span className="pill warn">ลบแล้ว</span>}</div>
                      <div className="muted text-[13px] break-all">{u.email}</div>
                    </td>
                    <td data-label="สมัครเมื่อ">{date(u.createdAt)}</td><td data-label="ใช้ปลั๊กอินล่าสุด">{date(u.lastSeenAt)}</td><td className="n" data-label="ปลั๊กอิน">{u.devices}</td><td className="n" data-label="ลิงก์">{u.links}</td>
                    <td><div className="acts">
                      <button type="button" className="btn small" disabled={!u.devices || act.isPending || !!u.deletedAt} onClick={() => ask(u, 'revoke-devices')}>ออกจากปลั๊กอิน</button>
                      <button type="button" className="btn small" disabled={!u.links || act.isPending || !!u.deletedAt} onClick={() => ask(u, 'revoke-links')}>ปิดลิงก์</button>
                      <button type="button" className="btn small danger" disabled={!!u.deletedAt || u.id === me.data?.id || act.isPending} onClick={() => ask(u, 'delete')}>ลบบัญชี</button>
                    </div></td>
                  </tr>
                ))}
                {res.data.users.length === 0 && <tr><td colSpan={6} className="muted">ไม่พบผู้ใช้</td></tr>}
              </tbody>
            </table>
          </div>
          <div className="flex items-center justify-between gap-3">
            <span className="adm-note" role="status">{term ? `พบ ${n(res.data.total)} คน` : `ทั้งหมด ${n(res.data.total)} คน`} · หน้า {page + 1} / {Math.max(1, Math.ceil(res.data.total / res.data.size))}</span>
            <span className="flex gap-2">
              <button type="button" className="btn small" disabled={page === 0} onClick={() => setPage(page - 1)}>ก่อนหน้า</button>
              <button type="button" className="btn small" disabled={(page + 1) * res.data.size >= res.data.total} onClick={() => setPage(page + 1)}>ถัดไป</button>
            </span>
          </div>
        </>
      )}
    </>
  );
}

export default function Admin() {
  const auth = useAuth();
  const { path } = useT();
  const location = useLocation();
  const { section = 'overview' } = useParams();
  const navigate = useNavigate();
  const access = useAdminAccess();
  const sec = (SECTIONS.some(s => s.id === section) ? section : 'overview') as SectionId;

  if (auth.status === 'signedOut') return <Navigate to={`${path('/login')}?next=${encodeURIComponent(location.pathname + location.search)}`} replace />;
  const title = SECTIONS.find(s => s.id === sec)!.label;
  return (
    <PageShell>
      <Seo title="Admin · HEARASIDE" noindex />
      <div className="wrap pt-10 pb-20" lang="th">
        <header className="adm-heading"><div><p className="eyebrow">HEARASIDE / ADMIN</p><h1 className="h2">หลังบ้าน</h1><p className="adm-note">บัญชีผู้ใช้ การเชื่อมต่อปลั๊กอิน และยอดดาวน์โหลด</p></div><Link className="btn" to={path('/')}>เปิดหน้าเว็บ<ArrowUpRight size={16} aria-hidden="true" /></Link></header>
        {auth.status === 'loading' || access.isLoading ? <p className="query-feedback" role="status"><span className="spin" aria-hidden="true" />กำลังตรวจสอบสิทธิ์</p>
          : access.isError ? <Loading q={access} />
          : !access.data ? <p className="card p-6 m-0" role="status">บัญชีนี้ไม่มีสิทธิ์เข้าหน้าผู้ดูแล</p>
          : (
            <div className="adm-grid">
              <nav className="card adm-menu" aria-label="Admin">
                <span className="adm-menu-caption">การจัดการ</span>
                {SECTIONS.map(s => (
                  <button key={s.id} type="button" className={`navbtn ${s.id === sec ? 'on' : ''}`} aria-current={s.id === sec ? 'page' : undefined}
                          onClick={() => navigate(path(s.id === 'overview' ? '/admin' : `/admin/${s.id}`))}>
                    <s.icon size={18} strokeWidth={1.8} aria-hidden="true" /><span>{s.label}<small>{s.description}</small></span>
                  </button>
                ))}
                {import.meta.env.VITE_AUTH_MODE === 'memory' && <p className="adm-local" role="status">ทดสอบในเครื่อง<br />ข้อมูลรีเซ็ตเมื่อเริ่ม API ใหม่</p>}
              </nav>
              <section className="card adm-body" aria-labelledby="adm-sec">
                <h2 id="adm-sec" className="h4">{title}</h2>
                {sec === 'overview' ? <Overview /> : sec === 'downloads' ? <DownloadsPage /> : <UsersPage />}
              </section>
            </div>
          )}
      </div>
    </PageShell>
  );
}
