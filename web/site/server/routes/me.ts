// The signed-in user's own things (prompt A1 item 8, A3, A5, A7). Website and plug-ins both use
// these; deleting the account, revoking links and signing other computers out are website only.
import { Router } from 'express';
import multer from 'multer';
import sharp from 'sharp';
import { deleteAccount, handle as handleSchema, profilePatch, savedFriend, syncedSettings, type Device, type ShareLink } from '../../shared/schemas.js';
import { StoreError, type Store } from '../lib/store.js';
import { authenticate } from '../lib/context.js';
import { limit, over } from '../lib/limit.js';
import { body, meOf } from '../lib/http.js';

const upload = multer({ storage: multer.memoryStorage(), limits: { fileSize: 5 * 1024 * 1024, files: 1 } });

/** JPG, PNG or WebP by its content (not its name) -> 512 x 512 WebP without EXIF / location */
export async function toAvatar(buf: Buffer): Promise<Buffer | null> {
  try {
    const meta = await sharp(buf, { limitInputPixels: 40_000_000 }).metadata();
    if (!meta.format || !['jpeg', 'png', 'webp'].includes(meta.format)) return null;
    return await sharp(buf, { limitInputPixels: 40_000_000 }).rotate().resize(512, 512, { fit: 'cover' }).webp({ quality: 84 }).toBuffer();
  } catch {
    return null;
  }
}

export function meRoutes(store: Store) {
  const r = Router();
  const any = authenticate(store);
  const web = authenticate(store, { webOnly: true });

  const me = async (userId: string) => {
    const [p, u] = await Promise.all([store.getProfile(userId), store.getUser(userId)]);
    return p && u ? meOf(store, p, u) : null;
  };

  r.get('/me', any, async (req, res) => {
    const m = await me(req.caller!.userId);
    m ? res.json(m) : res.status(404).json({ error: 'not_found' });
  });

  r.patch('/me', any, async (req, res) => {
    const b = body(profilePatch, req, res);
    if (!b) return;
    if (b.handle !== undefined && (await over('handle-change', req.caller!.userId, 10))) return res.status(429).json({ error: 'slow_down' });
    try {
      await store.updateProfile(req.caller!.userId, b);
    } catch (e) {
      if (e instanceof StoreError && (e.code === 'handle_taken' || e.code === 'handle_reserved')) return res.status(409).json({ error: 'handle_taken' });
      throw e;
    }
    res.json(await me(req.caller!.userId));
  });

  r.get('/handle/available', limit('handle-check', 60), any, async (req, res) => {
    const h = handleSchema.safeParse(String(req.query.h ?? ''));
    if (!h.success) return res.json({ available: false });
    res.json({ available: await store.handleAvailable(h.data, req.caller!.userId) });
  });

  r.post('/me/avatar', limit('avatar', 10), any, (req, res, next) => {
    upload.single('photo')(req, res, err => {
      if (err) return res.status(413).json({ error: 'too_big' });
      next();
    });
  }, async (req, res) => {
    if (!req.file) return res.status(400).json({ error: 'bad_image' });
    const webp = await toAvatar(req.file.buffer);
    if (!webp) return res.status(400).json({ error: 'bad_image' });
    await store.setAvatar(req.caller!.userId, webp);
    res.json(await me(req.caller!.userId));
  });

  r.delete('/me/avatar', any, async (req, res) => {
    await store.setAvatar(req.caller!.userId, null);
    res.json(await me(req.caller!.userId));
  });

  r.get('/me/settings', any, async (req, res) => {
    res.json((await store.getSettings(req.caller!.userId)) ?? { appearance: {}, stemNames: [], updatedAt: null });
  });
  r.put('/me/settings', any, async (req, res) => {
    const b = body(syncedSettings, req, res);   // strict: anything that changes the sound is refused
    if (!b) return;
    res.json(await store.putSettings(req.caller!.userId, b));
  });

  r.get('/me/devices', any, async (req, res) => {
    const list = await store.listDevices(req.caller!.userId);
    const devices: Device[] = list.map(d => ({
      id: d.id, name: d.name, os: d.os, daw: d.daw, plugin: d.plugin, pluginVersion: d.pluginVersion,
      createdAt: d.createdAt, lastSeenAt: d.lastSeenAt, current: d.id === req.caller!.deviceId || undefined,
    }));
    res.json({ devices });
  });
  r.delete('/me/devices/:id', web, async (req, res) => {
    const d = await store.deviceById(String(req.params.id));
    if (!d || d.userId !== req.caller!.userId) return res.status(404).json({ error: 'not_found' });
    await store.updateDevice(d.id, { revokedAt: new Date().toISOString() });
    res.json({ ok: true });
  });
  r.post('/me/devices/revoke-all', web, async (req, res) => {
    await store.revokeAllDevices(req.caller!.userId);
    res.json({ ok: true });
  });

  r.get('/me/links', any, async (req, res) => {
    const links: ShareLink[] = (await store.listLinks(req.caller!.userId)).map(l => ({ id: l.id, kind: l.kind, label: l.label, url: null, createdAt: l.createdAt }));
    res.json({ links });
  });
  r.delete('/me/links/:id', web, async (req, res) => {
    (await store.revokeLink(req.caller!.userId, String(req.params.id))) ? res.json({ ok: true }) : res.status(404).json({ error: 'not_found' });
  });

  r.get('/me/friends', any, async (req, res) => {
    res.json({ friends: (await store.listFriends(req.caller!.userId)).map(f => ({ id: f.id, name: f.name, createdAt: f.createdAt })) });
  });
  r.post('/me/friends', any, async (req, res) => {
    const b = body(savedFriend, req, res);
    if (!b) return;
    if ((await store.listFriends(req.caller!.userId)).length >= 50) return res.status(409).json({ error: 'too_many' });
    const f = await store.addFriend(req.caller!.userId, b.name);
    res.json({ id: f.id, name: f.name, createdAt: f.createdAt });
  });
  r.delete('/me/friends/:id', any, async (req, res) => {
    (await store.removeFriend(req.caller!.userId, String(req.params.id))) ? res.json({ ok: true }) : res.status(404).json({ error: 'not_found' });
  });

  // "Download my data" (PDPA): everything we keep about this person, nothing about anyone else
  r.get('/me/export', limit('export', 5), web, async (req, res) => {
    const id = req.caller!.userId;
    const [m, devices, settings, links, friends] = await Promise.all([
      me(id), store.listDevices(id), store.getSettings(id), store.listLinks(id), store.listFriends(id),
    ]);
    res.setHeader('Content-Disposition', 'attachment; filename="hearaside-account.json"');
    res.json({
      exportedAt: new Date().toISOString(),
      account: m,
      devices: devices.map(d => ({ name: d.name, os: d.os, daw: d.daw, plugin: d.plugin, pluginVersion: d.pluginVersion, createdAt: d.createdAt, lastSeenAt: d.lastSeenAt })),
      settings,
      links: links.map(l => ({ kind: l.kind, label: l.label, createdAt: l.createdAt })),
      savedFriends: friends.map(f => ({ name: f.name, createdAt: f.createdAt })),
      audio: 'HEARASIDE never stores audio.',
    });
  });

  r.post('/me/delete', limit('delete', 5), web, async (req, res) => {
    const b = body(deleteAccount, req, res);
    if (!b) return;
    const p = await store.getProfile(req.caller!.userId);
    if (!p || b.confirm.trim().toLowerCase() !== p.handle) return res.status(400).json({ error: 'confirm' });
    await store.deleteAccount(req.caller!.userId);
    res.json({ ok: true });
  });

  return r;
}
