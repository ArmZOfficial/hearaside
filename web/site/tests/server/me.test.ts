// Profile, photo, synced settings, privacy (A1, A3, A5, A7).
import sharp from 'sharp';
import { describe, expect, it } from 'vitest';
import { setup, signInPlugin } from './helpers';

describe('/me', () => {
  it('needs a sign-in', async () => {
    const { api } = await setup();
    expect((await api.get('/api/v1/me')).status).toBe(401);
    expect((await api.get('/api/v1/me').set('Authorization', 'Bearer not-a-real-token')).status).toBe(401);
  });

  it('edits name, about, language and username', async () => {
    const { api, auth } = await setup();
    const r = await api.patch('/api/v1/me').set(auth).send({ displayName: '  มิ้นท์  ', about: 'hi', locale: 'th', handle: 'Mint_01' });
    expect(r.status).toBe(200);
    expect(r.body).toMatchObject({ displayName: 'มิ้นท์', about: 'hi', locale: 'th', handle: 'mint_01' });
  });

  it('refuses bad or reserved usernames and unknown fields', async () => {
    const { api, auth } = await setup();
    expect((await api.patch('/api/v1/me').set(auth).send({ handle: 'ab' })).status).toBe(400);
    expect((await api.patch('/api/v1/me').set(auth).send({ handle: 'mint-01' })).status).toBe(400);
    expect((await api.patch('/api/v1/me').set(auth).send({ handle: 'admin' })).status).toBe(409);
    expect((await api.patch('/api/v1/me').set(auth).send({ plan: 'pro' })).status).toBe(400);
    expect((await api.patch('/api/v1/me').set(auth).send({ about: 'x'.repeat(161) })).status).toBe(400);
  });

  it('a username can only belong to one person', async () => {
    const ctx = await setup();
    ctx.store.signUp('beam@example.test', 'correct horse battery', 'Beam', 'en');
    const beam = ctx.store.signIn('beam@example.test', 'correct horse battery')!;
    await ctx.api.patch('/api/v1/me').set(ctx.auth).send({ handle: 'karaoke' });
    const check = await ctx.api.get('/api/v1/handle/available?h=karaoke').set('Authorization', `Bearer ${beam}`);
    expect(check.body.available).toBe(false);
    expect((await ctx.api.patch('/api/v1/me').set('Authorization', `Bearer ${beam}`).send({ handle: 'karaoke' })).status).toBe(409);
    // your own username counts as available for you
    expect((await ctx.api.get('/api/v1/handle/available?h=karaoke').set(ctx.auth)).body.available).toBe(true);
  });

  it('photo: any JPG/PNG/WebP becomes a 512 x 512 WebP; other files are refused', async () => {
    const { api, auth, store } = await setup();
    const png = await sharp({ create: { width: 900, height: 600, channels: 3, background: '#808080' } }).png().toBuffer();
    const r = await api.post('/api/v1/me/avatar').set(auth).attach('photo', png, { filename: 'me.png', contentType: 'image/png' });
    expect(r.status).toBe(200);
    expect(r.body.avatarUrl).toMatch(/\.webp$/);
    const saved = [...store.avatars.values()][0];
    const meta = await sharp(saved).metadata();
    expect(meta).toMatchObject({ format: 'webp', width: 512, height: 512 });
    expect(meta.exif).toBeUndefined();

    const fake = Buffer.from('<svg xmlns="http://www.w3.org/2000/svg"/>');
    const bad = await api.post('/api/v1/me/avatar').set(auth).attach('photo', fake, { filename: 'me.png', contentType: 'image/png' });
    expect(bad.status).toBe(400);

    const del = await api.delete('/api/v1/me/avatar').set(auth);
    expect(del.body.avatarUrl).toBeNull();
    expect(store.avatars.size).toBe(0);
  });

  it('photo over 5 MB is refused', async () => {
    const { api, auth } = await setup();
    const big = Buffer.alloc(5 * 1024 * 1024 + 1, 1);
    const r = await api.post('/api/v1/me/avatar').set(auth).attach('photo', big, { filename: 'big.jpg', contentType: 'image/jpeg' });
    expect(r.status).toBe(413);
  });
});

describe('synced settings', () => {
  it('keeps Appearance + stem names only; anything that changes the sound is refused', async () => {
    const ctx = await setup();
    const p = await signInPlugin(ctx);
    const bearer = { Authorization: `Bearer ${p.access}` };
    const ok = await ctx.api.put('/api/v1/me/settings').set(bearer).send({ appearance: { language: 'th', theme: 'dark', uiScale: 1.25 }, stemNames: ['Music', 'Vocal'] });
    expect(ok.status).toBe(200);
    expect((await ctx.api.get('/api/v1/me/settings').set(ctx.auth)).body).toMatchObject({ appearance: { language: 'th', theme: 'dark' }, stemNames: ['Music', 'Vocal'] });

    for (const audio of [{ peakCeiling: -1 }, { syncSafety: 3 }, { lineUpLimit: 600 }, { busName: 'other' }]) {
      const r = await ctx.api.put('/api/v1/me/settings').set(bearer).send({ appearance: {}, stemNames: [], ...audio });
      expect(r.status, JSON.stringify(audio)).toBe(400);
      const r2 = await ctx.api.put('/api/v1/me/settings').set(bearer).send({ appearance: { ...audio }, stemNames: [] });
      expect(r2.status, JSON.stringify(audio)).toBe(400);
    }
  });
});

describe('privacy', () => {
  it('"Download my data" has the account and no secrets', async () => {
    const ctx = await setup();
    await signInPlugin(ctx);
    await ctx.api.post('/api/v1/me/friends').set(ctx.auth).send({ name: 'Mint' });
    const r = await ctx.api.get('/api/v1/me/export').set(ctx.auth);
    expect(r.status).toBe(200);
    expect(r.body.account.email).toBe('singer@example.test');
    expect(r.body.devices).toHaveLength(1);
    expect(r.body.savedFriends).toEqual([expect.objectContaining({ name: 'Mint' })]);
    const text = JSON.stringify(r.body);
    expect(text).not.toMatch(/refresh|token_hash|refreshTokenHash|passwordHash/i);
  });

  it('website-only actions refuse plug-in tokens', async () => {
    const ctx = await setup();
    const p = await signInPlugin(ctx);
    const bearer = { Authorization: `Bearer ${p.access}` };
    expect((await ctx.api.post('/api/v1/me/delete').set(bearer).send({ confirm: 'x' })).status).toBe(403);
    expect((await ctx.api.get('/api/v1/me/export').set(bearer)).status).toBe(403);
    expect((await ctx.api.post('/api/v1/me/devices/revoke-all').set(bearer)).status).toBe(403);
  });

  it('"Delete account" needs the username typed, then signs everything out', async () => {
    const ctx = await setup();
    const p = await signInPlugin(ctx);
    const me = await ctx.api.get('/api/v1/me').set(ctx.auth);
    expect((await ctx.api.post('/api/v1/me/delete').set(ctx.auth).send({ confirm: 'wrong' })).status).toBe(400);
    expect((await ctx.api.post('/api/v1/me/delete').set(ctx.auth).send({ confirm: me.body.handle.toUpperCase() })).status).toBe(200);
    expect((await ctx.api.get('/api/v1/me').set(ctx.auth)).status).toBe(401);
    expect((await ctx.api.post('/api/v1/token/refresh').send({ refresh_token: p.refresh })).status).toBe(401);
    expect(ctx.store.signIn('singer@example.test', 'correct horse battery')).toBeNull();
  });

  it('API answers are never cached and errors do not leak', async () => {
    const { api } = await setup();
    const r = await api.post('/api/v1/device/start').set('Content-Type', 'application/json').send('{not json');
    expect(r.status).toBe(400);
    expect(r.headers['cache-control']).toBe('no-store');
    expect((await api.get('/api/v1/nothing-here')).status).toBe(404);
  });
});
