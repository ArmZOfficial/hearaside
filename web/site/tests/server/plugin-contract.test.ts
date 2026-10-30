import { describe, expect, it } from 'vitest';
import { CLIENT, setup } from './helpers';

describe('the HTTP contract used by C++ and Rust clients', () => {
  it.each(['hub', 'track', 'app'] as const)('%s can sign in, sync appearance, refresh, and be revoked from the website', async plugin => {
    const ctx = await setup();
    const info = { ...CLIENT, plugin, version: '2.0.0-rust' };
    const start = await ctx.api.post('/api/v1/device/start').send({ client_info: info });
    expect(start.status).toBe(200);
    expect(start.body.verification_uri).toMatch(/\/link$/);
    expect(start.body.interval).toBe(5);
    expect(start.body.expires_in).toBe(600);
    expect((await ctx.api.post('/api/v1/device/approve').set(ctx.auth).send({ user_code: start.body.user_code, allow: true })).status).toBe(200);
    const issued = await ctx.api.post('/api/v1/device/token').send({ device_code: start.body.device_code });
    expect(issued.status).toBe(200);
    expect(issued.body).toMatchObject({ token_type: 'Bearer', expires_in: 900, user: { displayName: 'Singer' } });
    const auth = { Authorization: `Bearer ${issued.body.access_token}` };
    const settings = { appearance: { language: 'th', theme: 'dark', uiScale: 1.25, reduceMotion: true }, stemNames: ['Vocal', 'Backing'] };
    expect((await ctx.api.put('/api/v1/me/settings').set(auth).send(settings)).status).toBe(200);
    expect((await ctx.api.get('/api/v1/me/settings').set(auth)).body).toMatchObject(settings);
    expect((await ctx.api.patch('/api/v1/device').set(auth).send({ name: 'Rust test client' })).status).toBe(200);
    const devices = await ctx.api.get('/api/v1/me/devices').set(ctx.auth);
    expect(devices.body.devices[0]).toMatchObject({ id: issued.body.device_id, plugin, pluginVersion: '2.0.0-rust', name: 'Rust test client' });
    expect((await ctx.api.get('/api/v1/admin/me').set(auth)).status).not.toBe(200);
    const refreshed = await ctx.api.post('/api/v1/token/refresh').send({ refresh_token: issued.body.refresh_token });
    expect(refreshed.status).toBe(200);
    expect(refreshed.body.refresh_token).not.toBe(issued.body.refresh_token);
    expect((await ctx.api.delete(`/api/v1/me/devices/${issued.body.device_id}`).set(ctx.auth)).status).toBe(200);
    expect((await ctx.api.get('/api/v1/me').set('Authorization', `Bearer ${refreshed.body.access_token}`)).status).toBe(401);
    expect((await ctx.api.get('/api/turn').set('Authorization', `Bearer ${refreshed.body.access_token}`)).status).toBe(401);
    expect((await ctx.api.post('/api/v1/token/refresh').send({ refresh_token: refreshed.body.refresh_token })).status).toBe(401);
  });
});
