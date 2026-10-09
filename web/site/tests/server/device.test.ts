// Signing a plug-in in with a code (A4): the whole flow, and the ways it must refuse.
import { describe, expect, it } from 'vitest';
import { CLIENT, setup, signInPlugin } from './helpers';

describe('device flow', () => {
  it('start gives a secret device code and a readable XXXX-XXXX user code', async () => {
    const { api } = await setup();
    const r = await api.post('/api/v1/device/start').send({ client_info: CLIENT });
    expect(r.status).toBe(200);
    expect(r.body.device_code).toMatch(/^[A-Za-z0-9_-]{43}$/);
    expect(r.body.user_code).toMatch(/^[A-HJ-NP-Z2-9]{4}-[A-HJ-NP-Z2-9]{4}$/);   // no 0 O 1 I
    expect(r.body.verification_uri_complete).toContain(`/link?code=${r.body.user_code}`);
    expect(r.body).toMatchObject({ expires_in: 600, interval: 5 });
  });

  it('refuses client info that carries anything extra (no track names, no audio)', async () => {
    const { api } = await setup();
    const r = await api.post('/api/v1/device/start').send({ client_info: { ...CLIENT, tracks: ['Vocal'] } });
    expect(r.status).toBe(400);
  });

  it('pending -> approved -> tokens; the code works once', async () => {
    const ctx = await setup();
    const start = await ctx.api.post('/api/v1/device/start').send({ client_info: CLIENT });
    const poll = () => ctx.api.post('/api/v1/device/token').send({ device_code: start.body.device_code });

    expect((await poll()).body.error).toBe('authorization_pending');
    const peek = await ctx.api.post('/api/v1/device/peek').send({ user_code: start.body.user_code.toLowerCase().replace('-', ' ') });
    expect(peek.body).toEqual({ plugin: 'hub', computer: 'Test-PC' });

    const look = await ctx.api.post('/api/v1/device/lookup').set(ctx.auth).send({ user_code: start.body.user_code });
    expect(look.body.clientInfo).toEqual(CLIENT);
    expect((await ctx.api.post('/api/v1/device/approve').set(ctx.auth).send({ user_code: start.body.user_code, allow: true })).status).toBe(200);

    const tok = await poll();
    expect(tok.status).toBe(200);
    expect(tok.body).toMatchObject({ token_type: 'Bearer', expires_in: 900 });
    expect(tok.body.user.handle).toMatch(/^singer\d{4}$/);
    expect((await poll()).body.error).toBe('invalid_grant');

    const me = await ctx.api.get('/api/v1/me').set('Authorization', `Bearer ${tok.body.access_token}`);
    expect(me.body.displayName).toBe('Singer');
    const devices = await ctx.api.get('/api/v1/me/devices').set(ctx.auth);
    expect(devices.body.devices).toHaveLength(1);
    expect(devices.body.devices[0]).toMatchObject({ name: 'Test-PC · Studio One', plugin: 'hub', daw: 'Studio One' });
  });

  it('polling faster than the interval gets slow_down', async () => {
    const { api } = await setup();
    const start = await api.post('/api/v1/device/start').send({ client_info: CLIENT });
    await api.post('/api/v1/device/token').send({ device_code: start.body.device_code });
    const fast = await api.post('/api/v1/device/token').send({ device_code: start.body.device_code });
    expect(fast.body.error).toBe('slow_down');
  });

  it('"Don\'t allow" ends it with access_denied', async () => {
    const ctx = await setup();
    const start = await ctx.api.post('/api/v1/device/start').send({ client_info: CLIENT });
    await ctx.api.post('/api/v1/device/approve').set(ctx.auth).send({ user_code: start.body.user_code, allow: false });
    const r = await ctx.api.post('/api/v1/device/token').send({ device_code: start.body.device_code });
    expect(r.body.error).toBe('access_denied');
  });

  it('an expired code is refused everywhere', async () => {
    const ctx = await setup();
    const start = await ctx.api.post('/api/v1/device/start').send({ client_info: CLIENT });
    for (const c of ctx.store.codes.values()) c.expiresAt = new Date(Date.now() - 1000).toISOString();
    expect((await ctx.api.post('/api/v1/device/lookup').set(ctx.auth).send({ user_code: start.body.user_code })).status).toBe(404);
    expect((await ctx.api.post('/api/v1/device/token').send({ device_code: start.body.device_code })).body.error).toBe('expired_token');
  });

  it('approving needs a website session, not a plug-in token', async () => {
    const ctx = await setup();
    const plugin = await signInPlugin(ctx);
    const start = await ctx.api.post('/api/v1/device/start').send({ client_info: CLIENT });
    const r = await ctx.api.post('/api/v1/device/approve').set('Authorization', `Bearer ${plugin.access}`).send({ user_code: start.body.user_code, allow: true });
    expect(r.status).toBe(403);
    expect((await ctx.api.post('/api/v1/device/approve').send({ user_code: start.body.user_code, allow: true })).status).toBe(401);
  });

  it('refresh rotates the token; an old one coming back signs the computer out', async () => {
    const ctx = await setup();
    const p = await signInPlugin(ctx);
    const r1 = await ctx.api.post('/api/v1/token/refresh').send({ refresh_token: p.refresh });
    expect(r1.status).toBe(200);
    expect(r1.body.refresh_token).not.toBe(p.refresh);

    const replay = await ctx.api.post('/api/v1/token/refresh').send({ refresh_token: p.refresh });
    expect(replay.status).toBe(401);
    // the real owner's newer token stops working too, and so does the access token
    expect((await ctx.api.post('/api/v1/token/refresh').send({ refresh_token: r1.body.refresh_token })).status).toBe(401);
    expect((await ctx.api.get('/api/v1/me').set('Authorization', `Bearer ${r1.body.access_token}`)).body.error).toBe('signed_out');
  });

  it('signing a device out on the website stops its tokens', async () => {
    const ctx = await setup();
    const p = await signInPlugin(ctx);
    expect((await ctx.api.delete(`/api/v1/me/devices/${p.deviceId}`).set(ctx.auth)).status).toBe(200);
    expect((await ctx.api.get('/api/v1/me').set('Authorization', `Bearer ${p.access}`)).status).toBe(401);
    expect((await ctx.api.post('/api/v1/token/refresh').send({ refresh_token: p.refresh })).status).toBe(401);
  });

  it('"Sign out" in the plug-in and renaming "This computer"', async () => {
    const ctx = await setup();
    const p = await signInPlugin(ctx);
    const bearer = { Authorization: `Bearer ${p.access}` };
    expect((await ctx.api.patch('/api/v1/device').set(bearer).send({ name: 'Studio PC' })).status).toBe(200);
    expect((await ctx.api.get('/api/v1/me/devices').set(ctx.auth)).body.devices[0].name).toBe('Studio PC');
    expect((await ctx.api.post('/api/v1/device/revoke').set(bearer)).status).toBe(200);
    expect((await ctx.api.get('/api/v1/me/devices').set(ctx.auth)).body.devices).toHaveLength(0);
  });

  it('rate-limits code requests per address', async () => {
    const { api } = await setup();
    let last = 0;
    for (let i = 0; i < 12; i++) last = (await api.post('/api/v1/device/start').send({ client_info: CLIENT })).status;
    expect(last).toBe(429);
  });
});
