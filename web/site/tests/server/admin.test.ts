// Back office: who gets in, download counting and the GitHub back-fill, people management.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import release from '../../src/content/release.json';
import { setup, signInPlugin } from './helpers';
import { summarizeDownloads } from '../../server/routes/admin';

beforeEach(() => { process.env.ADMIN_EMAILS = 'singer@example.test'; });
afterEach(() => { delete process.env.ADMIN_EMAILS; vi.restoreAllMocks(); });

describe('who gets in', () => {
  it('signed-out, ordinary users, plug-in tokens and an empty ADMIN_EMAILS all see 404 / 401', async () => {
    const ctx = await setup();
    expect((await ctx.api.get('/api/v1/admin/overview')).status).toBe(401);
    ctx.store.signUp('fan@example.test', 'correct horse battery', 'Fan', 'en');
    const fan = ctx.store.signIn('fan@example.test', 'correct horse battery')!;
    expect((await ctx.api.get('/api/v1/admin/overview').set('Authorization', `Bearer ${fan}`)).status).toBe(404);
    const plugin = await signInPlugin(ctx);
    expect((await ctx.api.get('/api/v1/admin/overview').set('Authorization', `Bearer ${plugin.access}`)).status).toBe(403);
    expect((await ctx.api.get('/api/v1/admin/me').set(ctx.auth)).body).toEqual({ admin: true });
    process.env.ADMIN_EMAILS = '';
    expect((await ctx.api.get('/api/v1/admin/me').set(ctx.auth)).status).toBe(404);
  });

  it('an unconfirmed address is not an admin even if listed', async () => {
    const ctx = await setup();
    [...ctx.store.users.values()][0].emailConfirmed = false;
    expect((await ctx.api.get('/api/v1/admin/me').set(ctx.auth)).status).toBe(404);
  });
});

describe('downloads', () => {
  it('last30 excludes earlier downloads even when the chart covers 60 days', () => {
    const day = (ago: number) => new Date(Date.now() - ago * 86400000).toISOString().slice(0, 10);
    const result = summarizeDownloads([
      { day: day(1), version: '1.0.10', source: 'site', count: 3 },
      { day: day(40), version: '1.0.10', source: 'site', count: 17 },
    ], 60);
    expect(result.last30).toBe(3);
    expect(result.total).toBe(20);
    expect(result.daily.reduce((sum, d) => sum + d.count, 0)).toBe(20);
  });
  it('counts a download and redirects to the installer; unknown versions are 404', async () => {
    const { api, auth } = await setup();
    const r = await api.get(`/api/v1/download/${release.version}`);
    expect(r.status).toBe(302);
    expect(r.headers.location).toBe(`/downloads/HEARASIDE-Setup-${release.version}.exe`);
    expect((await api.get('/api/v1/download/9.9.9')).status).toBe(404);
    expect((await api.get('/api/v1/download/..%2F..%2Fx')).status).toBe(404);
    const o = await api.get('/api/v1/admin/overview').set(auth);
    expect(o.body.downloads.total).toBe(1);
    expect(o.body.downloads.last30).toBe(1);
    expect(o.body.downloads.daily).toHaveLength(30);
    expect(o.body.downloads.versions[0]).toMatchObject({ version: release.version, site: 1, total: 1 });
  });

  it('the same browser asking again straight away counts once', async () => {
    const { api, auth } = await setup();
    for (let i = 0; i < 6; i++) await api.get(`/api/v1/download/${release.version}`);
    expect((await api.get('/api/v1/admin/downloads').set(auth)).body.counted).toBe(2);
  });

  it('a broken counter never stops the download', async () => {
    const { api, store } = await setup();
    vi.spyOn(store, 'bumpDownload').mockRejectedValue(new Error('db down'));
    vi.spyOn(console, 'error').mockImplementation(() => {});
    expect((await api.get(`/api/v1/download/${release.version}`)).status).toBe(302);
  });

  it('back-fills from GitHub (replacing, not adding) and takes a typed-in number', async () => {
    const { api, auth } = await setup();
    const gh = (n: number) => vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response(JSON.stringify([
      { tag_name: 'v1.0.5', assets: [{ name: 'HEARASIDE-Setup-1.0.5.exe', download_count: n }, { name: 'HAS-Karaoke-1.0.5.exe', download_count: 900 }, { name: 'notes.txt', download_count: 99 }] },
      { tag_name: 'v1.0.4', assets: [] },
      { tag_name: 'nightly', assets: [{ name: 'x.exe', download_count: 5 }] },
    ])));
    gh(2);
    const a = await api.post('/api/v1/admin/downloads/import-github').set(auth);
    expect(a.body.imported).toEqual([{ version: '1.0.5', count: 2 }, { version: '1.0.4', count: 0 }]);
    gh(7);
    await api.post('/api/v1/admin/downloads/import-github').set(auth);
    await api.get('/api/v1/download/1.0.5');
    const d = (await api.get('/api/v1/admin/downloads').set(auth)).body;
    expect(d.versions.find((v: { version: string }) => v.version === '1.0.5')).toMatchObject({ github: 7, site: 1, total: 8 });
    // a typed-in number is a separate source; the larger of the two "before counting" numbers is used
    expect((await api.put('/api/v1/admin/downloads/base').set(auth).send({ version: '1.0.5', count: 100 })).status).toBe(200);
    expect((await api.get('/api/v1/admin/downloads').set(auth)).body).toMatchObject({ before: 100, counted: 1, total: 101 });
    expect((await api.put('/api/v1/admin/downloads/base').set(auth).send({ version: 'latest', count: 1 })).status).toBe(400);
    expect((await api.put('/api/v1/admin/downloads/base').set(auth).send({ version: '1.0.5', count: -1 })).status).toBe(400);
  });

  it('says so when GitHub cannot be reached', async () => {
    const { api, auth } = await setup();
    vi.spyOn(globalThis, 'fetch').mockRejectedValue(new Error('offline'));
    vi.spyOn(console, 'error').mockImplementation(() => {});
    expect((await api.post('/api/v1/admin/downloads/import-github').set(auth)).status).toBe(502);
  });
});

describe('people', () => {
  it('lists, searches and counts', async () => {
    const ctx = await setup();
    ctx.store.signUp('beam@example.test', 'correct horse battery', 'Beam', 'en');
    const all = await ctx.api.get('/api/v1/admin/users').set(ctx.auth);
    expect(all.body.total).toBe(2);
    expect(all.body.users[0]).toHaveProperty('email');
    expect((await ctx.api.get('/api/v1/admin/users?q=beam').set(ctx.auth)).body.users.map((u: { email: string }) => u.email)).toEqual(['beam@example.test']);
    expect((await ctx.api.get('/api/v1/admin/users?q=nobody').set(ctx.auth)).body.total).toBe(0);
    const o = (await ctx.api.get('/api/v1/admin/overview').set(ctx.auth)).body.counts;
    expect(o).toMatchObject({ users: 2, deleted: 0, devices: 0, links: 0 });
    expect(o.signups.reduce((n: number, s: { count: number }) => n + s.count, 0)).toBe(2);
  });

  it('signs a person\'s plug-ins out and removes the account; never your own', async () => {
    const ctx = await setup();
    ctx.store.signUp('beam@example.test', 'correct horse battery', 'Beam', 'en');
    const beamId = [...ctx.store.users.values()].find(u => u.email === 'beam@example.test')!.id;
    const myId = [...ctx.store.users.values()].find(u => u.email === 'singer@example.test')!.id;
    await signInPlugin(ctx);   // singer's own device
    expect((await ctx.api.post(`/api/v1/admin/users/${myId}/revoke-devices`).set(ctx.auth)).status).toBe(200);
    expect(await ctx.store.listDevices(myId)).toHaveLength(0);
    expect((await ctx.api.post(`/api/v1/admin/users/${beamId}/revoke-links`).set(ctx.auth)).status).toBe(200);
    expect((await ctx.api.delete(`/api/v1/admin/users/${myId}`).set(ctx.auth)).status).toBe(400);
    expect((await ctx.api.delete(`/api/v1/admin/users/${beamId}`).set(ctx.auth)).status).toBe(200);
    expect((await ctx.store.getProfile(beamId))?.deletedAt).toBeTruthy();
    expect((await ctx.api.delete('/api/v1/admin/users/nope').set(ctx.auth)).status).toBe(404);
  });
});
