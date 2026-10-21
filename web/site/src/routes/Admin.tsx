// "/admin": the back office for the owner. Overview, downloads (with a back-fill from GitHub Releases)
// and people. The page only renders for addresses in ADMIN_EMAILS; the API enforces that again on
// every call (server/routes/admin.ts), so hiding things here is a convenience, not the lock.
import { useState, type ReactNode } from 'react';
import { Navigate, useNavigate, useParams } from 'react-router';
import { useMutation, useQueryClient } from '@tanstack/react-query';
import { Download, LayoutDashboard, Users } from 'lucide-react';
import { PageShell } from '../components/Layout';
import { Field, SubmitButton, useToast } from '../components/ui/Kit';
import { Seo } from '../lib/seo';
import { useAuth } from '../lib/auth';
import { ApiError, useAdminQuery, useApi, useIsAdmin, type AdminDownloads, type AdminOverview, type AdminUser } from '../lib/api';
import '../styles/admin.css';

const SECTIONS = [
  { id: 'overview', label: 'ภาพรวม', icon: LayoutDashboard },
  { id: 'downloads', label: 'ยอดดาวน์โหลด', icon: Download },
  { id: 'users', label: 'ผู้ใช้', icon: Users },
] as const;
type SectionId = (typeof SECTIONS)[number]['id'];

const n = (v: number) => v.toLocaleString('en-US');
const date = (iso: string | null) => (iso ? new Date(iso).toLocaleDateString('th-TH', { day: 'numeric', month: 'short', year: '2-digit' }) : '—');

function Stat({ value, label }: { value: ReactNode; label: string }) {
  return <div className="stat"><b>{value}</b><span>{label}</span></div>;
}

/** one bar per day; `title` on each bar gives the exact number on hover */
function Bars({ data, label }: { data: { day: string; count: number }[]; label: string }) {
  const max = Math.max(1, ...data.map(d => d.count));
  const w = 100 / data.length;
  return (
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
  );
}

function Loading({ q }: { q: { isLoading: boolean; isError: boolean } }) {
  if (q.isLoading) return <div className="spin my-6" aria-hidden="true" />;
  if (q.isError) return <p className="alert" role="alert">โหลดข้อมูลไม่สำเร็จ ลองรีเฟรชหน้านี้</p>;
  return null;
}

function Overview() {
  const q = useAdminQuery<AdminOverview>('overview', '/admin/overview');
  if (!q.data) return <Loading q={q} />;
  const { counts, downloads } = q.data;
  const signupDays = new Map(counts.signups.map(s => [s.day, s.count]));
  const signups = downloads.daily.map(d => ({ day: d.day, count: signupDays.get(d.day) ?? 0 }));
  return (
    <>
      <div className="stats">
        <Stat value={n(downloads.total)} label="ดาวน์โหลดทั้งหมด" />
        <Stat value={n(downloads.last30)} label="ดาวน์โหลด 30 วันล่าสุด" />
        <Stat value={n(counts.users - counts.deleted)} label="บัญชีที่ใช้งาน" />
        <Stat value={n(counts.activeDevices7d)} label={`ปลั๊กอินที่ออนไลน์ 7 วัน (จาก ${n(counts.devices)})`} />
        <Stat value={n(counts.links)} label="ลิงก์ถาวรที่เปิดอยู่" />
      </div>
      <section aria-labelledby="c1"><h3 id="c1" className="h4 pb-2">ดาวน์โหลดต่อวัน (30 วัน)</h3><Bars data={downloads.daily} label="ดาวน์โหลด" /></section>
      <section aria-labelledby="c2"><h3 id="c2" className="h4 pb-2">สมัครใหม่ต่อวัน (30 วัน)</h3><Bars data={signups} label="สมัครใหม่" /></section>
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
      <div className="stats">
        <Stat value={n(d.total)} label="รวมทั้งหมด" />
        <Stat value={n(d.counted)} label="นับจากเว็บนี้ (ตั้งแต่เริ่มระบบ)" />
        <Stat value={n(d.before)} label="ก่อนเริ่มนับ (GitHub / ใส่เอง)" />
      </div>
      <section aria-labelledby="d1"><h3 id="d1" className="h4 pb-2">ต่อวัน (60 วัน)</h3><Bars data={d.daily} label="ดาวน์โหลด" /></section>
      <section aria-labelledby="d2" className="flex flex-col gap-3">
        <h3 id="d2" className="h4">ต่อเวอร์ชัน</h3>
        <div className="tbl-wrap">
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
        <p className="adm-note">รวม = นับจากเว็บ + ตัวเลข “ก่อนเริ่มนับ” (ใช้ค่าที่มากกว่าระหว่าง GitHub กับที่ใส่เอง เพื่อไม่ให้นับซ้ำ)</p>
      </section>
      <section aria-labelledby="d3" className="flex flex-col gap-3">
        <h3 id="d3" className="h4">ยอดย้อนหลังก่อนมีระบบนี้</h3>
        <div><SubmitButton type="button" className="solid" busy={importGh.isPending} onClick={() => importGh.mutate()}>ดึงยอดย้อนหลังจาก GitHub Releases</SubmitButton></div>
        <p className="adm-note">
          ดึงตัวเลข download ของไฟล์ติดตั้งในหน้า Releases ของ GitHub (กดซ้ำได้ ค่าเดิมจะถูกแทนที่ ไม่บวกซ้ำ)
          แต่ไฟล์ที่โหลดจากเว็บ hearaside.netlify.app ก่อนหน้านี้ Netlify ไม่ได้เก็บจำนวนรายไฟล์ไว้ให้ดึง
          ถ้ามีตัวเลขจากที่อื่น (เช่น Netlify Analytics / log) ใส่เองได้ตรงนี้
        </p>
        <form className="adm-bar" onSubmit={e => { e.preventDefault(); setBase.mutate(); }}>
          <Field label="เวอร์ชัน" name="version" placeholder="1.0.5" value={version} onChange={e => setVersion(e.target.value)} />
          <Field label="จำนวนก่อนเริ่มนับ" name="count" inputMode="numeric" placeholder="120" value={count} onChange={e => setCount(e.target.value.replace(/\D/g, ''))} />
          <SubmitButton busy={setBase.isPending} disabled={!version.trim() || !count}>บันทึก</SubmitButton>
        </form>
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
  const act = useMutation({
    mutationFn: ({ id, what }: { id: string; what: 'revoke-devices' | 'revoke-links' | 'delete' }) =>
      what === 'delete' ? api('DELETE', `/admin/users/${id}`) : api('POST', `/admin/users/${id}/${what}`),
    onSuccess: () => { toast('เรียบร้อย'); void qc.invalidateQueries({ queryKey: ['admin'] }); },
    onError: e => toast(e instanceof ApiError && e.code === 'not_yourself' ? 'ลบบัญชีตัวเองไม่ได้' : 'ไม่สำเร็จ'),
  });
  const ask = (u: AdminUser, what: 'revoke-devices' | 'revoke-links' | 'delete') => {
    const text = { 'revoke-devices': 'ออกจากระบบปลั๊กอินทุกเครื่องของ', 'revoke-links': 'ปิดลิงก์ถาวรทั้งหมดของ', delete: 'ลบบัญชี (ลบถาวรใน 30 วัน) ของ' }[what];
    if (window.confirm(`${text} ${u.email || u.handle} ?`)) act.mutate({ id: u.id, what });
  };
  return (
    <>
      <form className="adm-bar" onSubmit={e => { e.preventDefault(); setPage(0); setTerm(q.trim()); }}>
        <Field label="ค้นหา" name="q" placeholder="อีเมล, username หรือชื่อ" value={q} onChange={e => setQ(e.target.value)} />
        <SubmitButton>ค้นหา</SubmitButton>
      </form>
      {!res.data ? <Loading q={res} /> : (
        <>
          <div className="tbl-wrap">
            <table className="tbl">
              <thead><tr><th>ผู้ใช้</th><th>สมัครเมื่อ</th><th>ออนไลน์ล่าสุด</th><th className="n">ปลั๊กอิน</th><th className="n">ลิงก์</th><th /></tr></thead>
              <tbody>
                {res.data.users.map(u => (
                  <tr key={u.id}>
                    <td>
                      <div>{u.displayName || u.handle} <span className="muted">@{u.handle}</span> {u.deletedAt && <span className="pill warn">ลบแล้ว</span>}</div>
                      <div className="muted text-[13px] break-all">{u.email}</div>
                    </td>
                    <td>{date(u.createdAt)}</td><td>{date(u.lastSeenAt)}</td><td className="n">{u.devices}</td><td className="n">{u.links}</td>
                    <td><div className="acts">
                      <button type="button" className="btn small" disabled={!u.devices} onClick={() => ask(u, 'revoke-devices')}>ออกจากปลั๊กอิน</button>
                      <button type="button" className="btn small" disabled={!u.links} onClick={() => ask(u, 'revoke-links')}>ปิดลิงก์</button>
                      <button type="button" className="btn small danger" disabled={!!u.deletedAt} onClick={() => ask(u, 'delete')}>ลบบัญชี</button>
                    </div></td>
                  </tr>
                ))}
                {res.data.users.length === 0 && <tr><td colSpan={6} className="muted">ไม่พบผู้ใช้</td></tr>}
              </tbody>
            </table>
          </div>
          <div className="flex items-center justify-between gap-3">
            <span className="adm-note">ทั้งหมด {n(res.data.total)} คน</span>
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
  const { section = 'overview' } = useParams();
  const navigate = useNavigate();
  const isAdmin = useIsAdmin();
  const sec = (SECTIONS.some(s => s.id === section) ? section : 'overview') as SectionId;

  if (auth.status === 'signedOut') return <Navigate to={`/login?next=${encodeURIComponent('/admin')}`} replace />;
  const title = SECTIONS.find(s => s.id === sec)!.label;
  return (
    <PageShell>
      <Seo title="Admin · HEARASIDE" noindex />
      <div className="wrap pt-10 pb-20">
        <header className="flex flex-col gap-2 pb-7"><h1 className="h2">หลังบ้าน</h1></header>
        {auth.status === 'loading' ? <div className="spin my-6" aria-hidden="true" />
          : !isAdmin ? <p className="card p-6 m-0" role="status">ไม่มีสิทธิ์เข้าหน้านี้ (หรือกำลังตรวจสอบสิทธิ์)</p>
          : (
            <div className="adm-grid">
              <nav className="card adm-menu" aria-label="Admin">
                {SECTIONS.map(s => (
                  <button key={s.id} type="button" className={`navbtn ${s.id === sec ? 'on' : ''}`} aria-current={s.id === sec ? 'page' : undefined}
                          onClick={() => navigate(s.id === 'overview' ? '/admin' : `/admin/${s.id}`)}>
                    <s.icon size={18} strokeWidth={1.8} aria-hidden="true" />{s.label}
                  </button>
                ))}
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
