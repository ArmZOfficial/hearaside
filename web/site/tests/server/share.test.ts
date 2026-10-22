// Permanent share links on the account website: the directory, recording links on the account,
// and revoking one from Account › Links and friends.
import { describe, expect, it } from 'vitest';
import { setup, signInPlugin } from './helpers';

const secret = 'a'.repeat(64);
const L = 'listen0123456789abcdefgh', S = 'send0123456789abcdefghij', F = 'friendaaaaaaaaaaaaaaaaaaa';
const tunnel = 'https://able-dog-cat.trycloudflare.com';
const reg = { tunnel, secret, tokens: { l: L, s: S, f: [F] } };

describe('share directory on the account website', () => {
  it('works without an account, like web/share-vercel', async () => {
    const ctx = await setup();
    expect((await ctx.api.post('/api/register').send(reg)).status).toBe(200);
    const r = await ctx.api.get(`/api/resolve/${F}`);
    expect(r.body).toEqual({ online: true, ws: `wss://able-dog-cat.trycloudflare.com/ws/s/${F}` });
    expect((await ctx.api.get('/api/v1/me/links').set(ctx.auth)).body.links).toEqual([]);
  });

  it('a signed-in Hub records its links on the account; revoking one takes it offline for good', async () => {
    const ctx = await setup();
    const { access } = await signInPlugin(ctx);
    expect((await ctx.api.post('/api/register').set({ Authorization: `Bearer ${access}` }).send(reg)).status).toBe(200);
    await ctx.api.post('/api/register').set({ Authorization: `Bearer ${access}` }).send(reg);   // heartbeat: no duplicates
    const links = (await ctx.api.get('/api/v1/me/links').set(ctx.auth)).body.links as { id: string; kind: string }[];
    expect(links.map(l => l.kind).sort()).toEqual(['listen', 'send', 'send']);
    expect(JSON.stringify(links)).not.toContain(L);   // never the token itself

    const listen = links.find(l => l.kind === 'listen')!;
    expect((await ctx.api.delete(`/api/v1/me/links/${listen.id}`).set(ctx.auth)).status).toBe(200);
    expect((await ctx.api.get(`/api/resolve/${L}`)).body).toEqual({ online: false });
    const again = await ctx.api.post('/api/register').set({ Authorization: `Bearer ${access}` }).send(reg);
    expect(again.body.revoked).toEqual([L]);   // the Hub makes a new listen link
    expect((await ctx.api.get(`/api/resolve/${S}`)).body.online).toBe(true);
  });

  it('a bad plug-in token still registers (the link must not depend on the account) but records nothing', async () => {
    const ctx = await setup();
    expect((await ctx.api.post('/api/register').set({ Authorization: 'Bearer not-a-real-token-xyz' }).send(reg)).status).toBe(200);
    expect((await ctx.api.get('/api/v1/me/links').set(ctx.auth)).body.links).toEqual([]);
  });

  it('/api/turn requires a signed-in Hub and returns iceServers', async () => {
    const ctx = await setup();
    // Unauthorized without token
    expect((await ctx.api.get('/api/turn')).status).toBe(401);
    expect((await ctx.api.post('/api/turn')).status).toBe(401);

    // Bad token
    expect((await ctx.api.get('/api/turn').set({ Authorization: 'Bearer invalid-token' })).status).toBe(401);

    // Signed-in Hub
    const { access } = await signInPlugin(ctx);
    const resGet = await ctx.api.get('/api/turn').set({ Authorization: `Bearer ${access}` });
    expect(resGet.status).toBe(200);
    expect(Array.isArray(resGet.body.iceServers)).toBe(true);
    expect(resGet.body.iceServers.length).toBeGreaterThan(0);
    expect(resGet.body.iceServers[0].urls).toBeDefined();

    const resPost = await ctx.api.post('/api/turn').set({ Authorization: `Bearer ${access}` });
    expect(resPost.status).toBe(200);
    expect(Array.isArray(resPost.body.iceServers)).toBe(true);
  });
});
