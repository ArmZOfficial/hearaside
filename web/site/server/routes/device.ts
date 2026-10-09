// Signing a plug-in in with a code (device authorization flow, RFC 8628 style; prompt A4):
//   plug-in  POST device/start    -> device_code (secret, stays in the plug-in) + user_code "KQ7M2TXP"
//   browser  POST device/peek     -> which plug-in / computer is waiting (for the sign-in banner)
//   browser  POST device/lookup   -> the details for "Sign in HEARASIDE on this computer?" (signed in)
//   browser  POST device/approve  -> Allow / Don't allow (signed in)
//   plug-in  POST device/token    -> every `interval` s: authorization_pending ... then the tokens
//   plug-in  POST token/refresh   -> new access + refresh token (the old refresh token stops working)
//   plug-in  POST device/revoke   -> "Sign out" in the plug-in
//   plug-in  PATCH device         -> "This computer" name
import { Router } from 'express';
import { randomUUID } from 'node:crypto';
import { deviceApprove, deviceLookup, deviceRename, deviceStart, deviceToken, normalizeUserCode, tokenRefresh } from '../../shared/schemas.js';
import type { Store } from '../lib/store.js';
import { authenticate } from '../lib/context.js';
import { limit, over } from '../lib/limit.js';
import { ACCESS_TTL_SEC, DEVICE_CODE_TTL_SEC, POLL_INTERVAL_SEC, newUserCode, randomToken, sha256, signAccess } from '../lib/tokens.js';
import { body, meOf, siteUrl } from '../lib/http.js';

export function deviceRoutes(store: Store) {
  const r = Router();
  const web = authenticate(store, { webOnly: true });
  const any = authenticate(store);

  r.post('/device/start', limit('device-start', 10), async (req, res) => {
    const b = body(deviceStart, req, res);
    if (!b) return;
    const deviceCode = randomToken();
    let userCode = newUserCode();
    for (let i = 0; i < 5 && (await store.deviceCodeByUserCode(userCode)); i++) userCode = newUserCode();
    const now = Date.now();
    await store.createDeviceCode({
      deviceCodeHash: sha256(deviceCode), userCode, clientInfo: b.client_info, status: 'pending', userId: null,
      createdAt: new Date(now).toISOString(), expiresAt: new Date(now + DEVICE_CODE_TTL_SEC * 1000).toISOString(), lastPollAt: null,
    });
    const pretty = `${userCode.slice(0, 4)}-${userCode.slice(4)}`;
    res.json({
      device_code: deviceCode, user_code: pretty,
      verification_uri: `${siteUrl()}/link`, verification_uri_complete: `${siteUrl()}/link?code=${pretty}`,
      expires_in: DEVICE_CODE_TTL_SEC, interval: POLL_INTERVAL_SEC,
    });
  });

  const pendingCode = async (raw: string) => {
    const code = normalizeUserCode(raw);
    if (!code) return null;
    const c = await store.deviceCodeByUserCode(code);
    if (!c || c.status !== 'pending' || Date.parse(c.expiresAt) < Date.now()) return null;
    return c;
  };

  r.post('/device/peek', limit('device-peek', 20), async (req, res) => {
    const b = body(deviceLookup, req, res);
    if (!b) return;
    const c = await pendingCode(b.user_code);
    if (!c) return res.status(404).json({ error: 'no_code' });
    res.json({ plugin: c.clientInfo.plugin, computer: c.clientInfo.computer });
  });

  r.post('/device/lookup', limit('device-lookup', 30), web, async (req, res) => {
    const b = body(deviceLookup, req, res);
    if (!b) return;
    if (await over('device-lookup-user', req.caller!.userId, 15)) return res.status(429).json({ error: 'slow_down' });
    const c = await pendingCode(b.user_code);
    if (!c) return res.status(404).json({ error: 'no_code' });
    res.json({ userCode: c.userCode, clientInfo: c.clientInfo, createdAt: c.createdAt });
  });

  r.post('/device/approve', limit('device-lookup', 30), web, async (req, res) => {
    const b = body(deviceApprove, req, res);
    if (!b) return;
    const c = await pendingCode(b.user_code);
    if (!c) return res.status(404).json({ error: 'no_code' });
    await store.updateDeviceCode(c.deviceCodeHash, { status: b.allow ? 'approved' : 'denied', userId: req.caller!.userId });
    res.json({ ok: true });
  });

  r.post('/device/token', limit('device-token', 30), async (req, res) => {
    const b = body(deviceToken, req, res);
    if (!b) return;
    const hash = sha256(b.device_code);
    const c = await store.deviceCodeByHash(hash);
    if (!c || c.status === 'used') return res.status(400).json({ error: 'invalid_grant' });
    if (Date.parse(c.expiresAt) < Date.now()) return res.status(400).json({ error: 'expired_token' });
    if (c.status === 'denied') return res.status(400).json({ error: 'access_denied' });
    const now = Date.now();
    const tooSoon = c.lastPollAt && now - Date.parse(c.lastPollAt) < (POLL_INTERVAL_SEC - 1) * 1000;
    await store.updateDeviceCode(hash, { lastPollAt: new Date(now).toISOString() });
    if (c.status === 'pending') return res.status(400).json({ error: tooSoon ? 'slow_down' : 'authorization_pending' });

    // approved: one device row per sign-in; the code can't be used again
    await store.updateDeviceCode(hash, { status: 'used' });
    const refresh = randomToken();
    const id = randomUUID();
    const info = c.clientInfo;
    await store.createDevice({
      id, userId: c.userId!, name: info.daw ? `${info.computer} · ${info.daw}` : info.computer, os: info.os, daw: info.daw, plugin: info.plugin,
      pluginVersion: info.version, refreshTokenHash: sha256(refresh), prevRefreshHash: null,
      createdAt: new Date(now).toISOString(), lastSeenAt: new Date(now).toISOString(), revokedAt: null,
    });
    const profile = await store.getProfile(c.userId!);
    const user = await store.getUser(c.userId!);
    res.json({
      token_type: 'Bearer', access_token: await signAccess(c.userId!, id), expires_in: ACCESS_TTL_SEC, refresh_token: refresh,
      device_id: id, user: profile && user ? meOf(store, profile, user) : null,
    });
  });

  r.post('/token/refresh', limit('token-refresh', 30), async (req, res) => {
    const b = body(tokenRefresh, req, res);
    if (!b) return;
    const hash = sha256(b.refresh_token);
    const d = await store.deviceByRefreshHash(hash);
    if (!d) {
      // an old refresh token came back: someone copied it. Sign that computer out.
      const stolen = await store.deviceByPrevRefreshHash(hash);
      if (stolen && !stolen.revokedAt) await store.updateDevice(stolen.id, { revokedAt: new Date().toISOString() });
      return res.status(401).json({ error: 'invalid_grant' });
    }
    if (d.revokedAt) return res.status(401).json({ error: 'invalid_grant' });
    const p = await store.getProfile(d.userId);
    if (!p || p.deletedAt) return res.status(401).json({ error: 'invalid_grant' });
    const next = randomToken();
    await store.updateDevice(d.id, { refreshTokenHash: sha256(next), prevRefreshHash: hash, lastSeenAt: new Date().toISOString() });
    res.json({ token_type: 'Bearer', access_token: await signAccess(d.userId, d.id), expires_in: ACCESS_TTL_SEC, refresh_token: next });
  });

  r.post('/device/revoke', any, async (req, res) => {
    if (req.caller!.via !== 'plugin') return res.status(400).json({ error: 'plugin_only' });
    await store.updateDevice(req.caller!.deviceId!, { revokedAt: new Date().toISOString() });
    res.json({ ok: true });
  });

  r.patch('/device', any, async (req, res) => {
    if (req.caller!.via !== 'plugin') return res.status(400).json({ error: 'plugin_only' });
    const b = body(deviceRename, req, res);
    if (!b) return;
    await store.updateDevice(req.caller!.deviceId!, { name: b.name });
    res.json({ ok: true });
  });

  return r;
}

