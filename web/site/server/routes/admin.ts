// The back office (/api/v1/admin/*): dashboard numbers, downloads, people. Website sessions only
// (a plug-in token never gets in), and only for the e-mail addresses in ADMIN_EMAILS with a confirmed
// address. Everyone else gets 404, so the area doesn't announce itself.
import { Router, type NextFunction, type Request, type Response } from 'express';
import { z } from 'zod';
import { BASE_DAY, type DownloadRow, type Store } from '../lib/store.js';
import { authenticate } from '../lib/context.js';
import { body } from '../lib/http.js';
import { limit } from '../lib/limit.js';
import { revokeHash, type DirStore } from '../share/directory.js';
import { store as redisStore } from '../share/redis.js';

const adminEmails = () => new Set((process.env.ADMIN_EMAILS ?? '').split(',').map(e => e.trim().toLowerCase()).filter(Boolean));

export function adminOnly(store: Store) {
  const web = authenticate(store, { webOnly: true });
  return (req: Request, res: Response, next: NextFunction) => {
    const notFound = () => res.status(404).json({ error: 'not_found' });
    if (adminEmails().size === 0) return notFound();
    web(req, res, async err => {
      if (err) return next(err);
      if (res.headersSent) return;
      try {
        const u = await store.getUser(req.caller!.userId);
        if (!u || !u.emailConfirmed || !adminEmails().has(u.email.toLowerCase())) return notFound();
        next();
      } catch (e) {
        next(e);
      }
    });
  };
}

const VERSION_RE = /^\d+\.\d+\.\d+$/;
const versionKey = (v: string) => v.split('.').map(Number).reduce((a, n) => a * 1000 + n, 0);

export function summarizeDownloads(rows: DownloadRow[], days = 30) {
  const by = new Map<string, { version: string; site: number; github: number; manual: number }>();
  const perDay = new Map<string, number>();
  for (const r of rows) {
    const v = by.get(r.version) ?? { version: r.version, site: 0, github: 0, manual: 0 };
    v[r.source] += r.count;
    by.set(r.version, v);
    if (r.source === 'site' && r.day !== BASE_DAY) perDay.set(r.day, (perDay.get(r.day) ?? 0) + r.count);
  }
  const versions = [...by.values()].sort((a, b) => versionKey(b.version) - versionKey(a.version))
    .map(v => ({ ...v, total: v.site + Math.max(v.github, v.manual) }));
  // every one of the last `days` UTC days, zeros included, so the chart has no gaps
  const daily: { day: string; count: number }[] = [];
  for (let i = days - 1; i >= 0; i--) {
    const d = new Date(Date.now() - i * 86_400_000).toISOString().slice(0, 10);
    daily.push({ day: d, count: perDay.get(d) ?? 0 });
  }
  const sum = (f: (v: (typeof versions)[number]) => number) => versions.reduce((n, v) => n + f(v), 0);
  return {
    total: sum(v => v.total), counted: sum(v => v.site), before: sum(v => Math.max(v.github, v.manual)),
    last30: daily.reduce((n, d) => n + d.count, 0), versions, daily,
  };
}

/** installer downloads of every GitHub release, summed per version (the numbers GitHub kept before we counted) */
export async function githubDownloads(repo = process.env.GITHUB_REPO || 'ArmZOfficial/hearaside'): Promise<{ version: string; count: number }[]> {
  const r = await fetch(`https://api.github.com/repos/${repo}/releases?per_page=100`, {
    headers: { Accept: 'application/vnd.github+json', 'User-Agent': 'hearaside-admin' },
    signal: AbortSignal.timeout(10_000),
  });
  if (!r.ok) throw new Error(`github ${r.status}`);
  const releases = await r.json() as { tag_name: string; assets: { name: string; download_count: number }[] }[];
  return releases.flatMap(rel => {
    const version = rel.tag_name.replace(/^v/, '');
    if (!VERSION_RE.test(version)) return [];
    return [{ version, count: rel.assets.filter(a => /\.exe$/i.test(a.name)).reduce((n, a) => n + a.download_count, 0) }];
  });
}

export function adminRoutes(store: Store, dir: () => DirStore = redisStore, fetchGithub = githubDownloads) {
  const r = Router();
  const admin = adminOnly(store);
  const audit = (req: Request, what: string, target = '') => console.info(`admin ${req.caller!.userId} ${what} ${target}`.trim());

  r.get('/admin/me', admin, (_req, res) => { res.json({ admin: true }); });

  r.get('/admin/overview', admin, async (_req, res) => {
    const [counts, rows] = await Promise.all([store.adminCounts(), store.downloadRows()]);
    const d = summarizeDownloads(rows);
    res.json({ counts, downloads: { total: d.total, last30: d.last30, daily: d.daily, versions: d.versions } });
  });

  r.get('/admin/downloads', admin, async (_req, res) => { res.json(summarizeDownloads(await store.downloadRows(), 60)); });

  r.post('/admin/downloads/import-github', limit('admin-import', 6), admin, async (req, res) => {
    let found: { version: string; count: number }[];
    try {
      found = await fetchGithub();
    } catch (e) {
      console.error('admin github import:', e instanceof Error ? e.message : e);
      return res.status(502).json({ error: 'github_unreachable' });
    }
    for (const f of found) await store.setDownloadBase(f.version, 'github', f.count);
    audit(req, 'import-github', `${found.length} versions`);
    res.json({ imported: found });
  });

  r.put('/admin/downloads/base', admin, async (req, res) => {
    const b = body(z.object({ version: z.string().regex(VERSION_RE), count: z.number().int().min(0).max(100_000_000) }).strict(), req, res);
    if (!b) return;
    await store.setDownloadBase(b.version, 'manual', b.count);
    audit(req, 'set-base', `${b.version}=${b.count}`);
    res.json({ ok: true });
  });

  r.get('/admin/users', admin, async (req, res) => {
    const q = String(req.query.q ?? '').slice(0, 80);
    const page = Math.max(0, Math.min(10_000, Number.parseInt(String(req.query.page ?? '0'), 10) || 0));
    const size = 25;
    const { total, users } = await store.adminUsers(q, page * size, size);
    res.json({ total, page, size, users });
  });

  const target = async (req: Request, res: Response) => {
    const id = String(req.params.id);
    const p = await store.getProfile(id);
    if (!p) { res.status(404).json({ error: 'not_found' }); return null; }
    return p;
  };

  r.post('/admin/users/:id/revoke-devices', admin, async (req, res) => {
    const p = await target(req, res);
    if (!p) return;
    await store.revokeAllDevices(p.id);
    audit(req, 'revoke-devices', p.id);
    res.json({ ok: true });
  });

  r.post('/admin/users/:id/revoke-links', admin, async (req, res) => {
    const p = await target(req, res);
    if (!p) return;
    for (const l of await store.listLinks(p.id)) {
      await revokeHash(dir(), l.tokenHash).catch(() => {});
      await store.revokeLink(p.id, l.id);
    }
    audit(req, 'revoke-links', p.id);
    res.json({ ok: true });
  });

  r.delete('/admin/users/:id', admin, async (req, res) => {
    const p = await target(req, res);
    if (!p) return;
    if (p.id === req.caller!.userId) return res.status(400).json({ error: 'not_yourself' });
    for (const l of await store.listLinks(p.id)) await revokeHash(dir(), l.tokenHash).catch(() => {});
    await store.deleteAccount(p.id);
    audit(req, 'delete-user', p.id);
    res.json({ ok: true });
  });

  return r;
}
