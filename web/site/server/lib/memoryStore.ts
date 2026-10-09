// Accounts kept in memory: `npm run dev` without Supabase, the tests, and the plug-in mock server
// (A4 item 7). Includes tiny email + password sign-in (scrypt) so the website can be clicked
// through end to end. Never used in production (STORE=memory only).
import { randomBytes, randomUUID, scryptSync, timingSafeEqual } from 'node:crypto';
import { HANDLE_RE, type SyncedSettings } from '../../shared/schemas.js';
import { StoreError, type AuthUser, type DeviceCode, type DeviceRow, type FriendRow, type LinkRow, type Profile, type Store } from './store.js';

const RESERVED = new Set(['admin', 'administrator', 'hearaside', 'support', 'help', 'api', 'link', 'login', 'logout', 'signup', 'signin',
  'account', 'accounts', 'download', 'downloads', 'guides', 'guide', 'privacy', 'terms', 'settings', 'me', 'root', 'system', 'staff',
  'team', 'official', 'www', 'mail', 'l', 's', 'th', 'en', 'verify', 'reset', 'forgot', 'null', 'undefined']);
export const isReserved = (h: string) => RESERVED.has(h.toLowerCase());

interface MemUser extends AuthUser { passwordHash: string }

const now = () => new Date().toISOString();

function hashPassword(pw: string) {
  const salt = randomBytes(16);
  return `${salt.toString('hex')}:${scryptSync(pw, salt, 32).toString('hex')}`;
}
function checkPassword(pw: string, stored: string) {
  const [salt, hash] = stored.split(':');
  const a = scryptSync(pw, Buffer.from(salt, 'hex'), 32);
  return timingSafeEqual(a, Buffer.from(hash, 'hex'));
}

export class MemoryStore implements Store {
  users = new Map<string, MemUser>();
  sessions = new Map<string, string>();           // session token -> user id
  profiles = new Map<string, Profile>();
  avatars = new Map<string, Buffer>();            // path -> webp
  codes = new Map<string, DeviceCode>();
  devices = new Map<string, DeviceRow>();
  settings = new Map<string, SyncedSettings & { updatedAt: string }>();
  links = new Map<string, LinkRow>();
  friends = new Map<string, FriendRow>();

  // --- the development sign-in (website, memory mode) ----------------------------------------
  signUp(email: string, password: string, displayName: string, locale: 'en' | 'th'): string {
    const key = email.trim().toLowerCase();
    const existing = [...this.users.values()].find(u => u.email === key);
    if (existing) return existing.id;   // same answer either way: never tell who has an account
    const id = randomUUID();
    this.users.set(id, { id, email: key, emailConfirmed: true, providers: ['email'], passwordChangedAt: now(), createdAt: now(), passwordHash: hashPassword(password) });
    let base = key.split('@')[0].replace(/[^a-z0-9_]/g, '').slice(0, 14);
    if (base.length < 3) base = 'user';
    let handle = '';
    do handle = base + String(Math.floor(Math.random() * 10000)).padStart(4, '0'); while ([...this.profiles.values()].some(p => p.handle === handle));
    this.profiles.set(id, { id, handle, displayName: displayName.slice(0, 40), about: '', avatarPath: null, locale, hideEmail: true, plan: 'free', deletedAt: null });
    return id;
  }
  signIn(email: string, password: string): string | null {
    const u = [...this.users.values()].find(x => x.email === email.trim().toLowerCase());
    if (!u || !checkPassword(password, u.passwordHash) || this.profiles.get(u.id)?.deletedAt) return null;
    const t = randomBytes(24).toString('base64url');
    this.sessions.set(t, u.id);
    return t;
  }
  signOut(token: string) { this.sessions.delete(token); }
  signOutEverywhere(userId: string) { for (const [t, id] of this.sessions) if (id === userId) this.sessions.delete(t); }
  setPassword(userId: string, pw: string) {
    const u = this.users.get(userId);
    if (u) { u.passwordHash = hashPassword(pw); u.passwordChangedAt = now(); }
  }
  setEmail(userId: string, email: string) {
    const u = this.users.get(userId);
    if (u) u.email = email.trim().toLowerCase();
  }

  // --- Store -----------------------------------------------------------------------------------
  async userFromSession(token: string) {
    const id = this.sessions.get(token);
    return id ? this.getUser(id) : null;
  }
  async getUser(id: string) {
    const u = this.users.get(id);
    if (!u) return null;
    const { passwordHash: _, ...rest } = u;
    return rest;
  }
  async getProfile(id: string) {
    const p = this.profiles.get(id);
    return p ? { ...p } : null;
  }
  async updateProfile(id: string, patch: Partial<Profile>) {
    const p = this.profiles.get(id);
    if (!p) throw new StoreError('not_found');
    if (patch.handle !== undefined && patch.handle !== p.handle) {
      if (!HANDLE_RE.test(patch.handle) || isReserved(patch.handle)) throw new StoreError('handle_reserved');
      if (!(await this.handleAvailable(patch.handle, id))) throw new StoreError('handle_taken');
    }
    Object.assign(p, Object.fromEntries(Object.entries(patch).filter(([, v]) => v !== undefined)));
    return { ...p };
  }
  async handleAvailable(handle: string, exceptUserId?: string) {
    const h = handle.toLowerCase();
    if (isReserved(h)) return false;
    return ![...this.profiles.values()].some(p => p.handle === h && p.id !== exceptUserId);
  }
  async setAvatar(id: string, webp: Buffer | null) {
    const p = this.profiles.get(id);
    if (!p) throw new StoreError('not_found');
    if (p.avatarPath) this.avatars.delete(p.avatarPath);
    p.avatarPath = webp ? `${id}/${randomBytes(8).toString('hex')}.webp` : null;
    if (webp && p.avatarPath) this.avatars.set(p.avatarPath, webp);
    return p.avatarPath;
  }
  avatarUrl(path: string | null) {
    return path ? `/api/v1/dev/avatars/${path}` : null;
  }

  async createDeviceCode(c: DeviceCode) { this.codes.set(c.deviceCodeHash, { ...c }); }
  async deviceCodeByUserCode(userCode: string) {
    const c = [...this.codes.values()].find(x => x.userCode === userCode);
    return c ? { ...c } : null;
  }
  async deviceCodeByHash(hash: string) {
    const c = this.codes.get(hash);
    return c ? { ...c } : null;
  }
  async updateDeviceCode(hash: string, patch: Partial<DeviceCode>) {
    const c = this.codes.get(hash);
    if (c) Object.assign(c, patch);
  }

  async createDevice(d: DeviceRow) { this.devices.set(d.id, { ...d }); }
  async deviceById(id: string) {
    const d = this.devices.get(id);
    return d ? { ...d } : null;
  }
  async deviceByRefreshHash(hash: string) {
    const d = [...this.devices.values()].find(x => x.refreshTokenHash === hash);
    return d ? { ...d } : null;
  }
  async deviceByPrevRefreshHash(hash: string) {
    const d = [...this.devices.values()].find(x => x.prevRefreshHash === hash);
    return d ? { ...d } : null;
  }
  async updateDevice(id: string, patch: Partial<DeviceRow>) {
    const d = this.devices.get(id);
    if (d) Object.assign(d, patch);
  }
  async listDevices(userId: string) {
    return [...this.devices.values()].filter(d => d.userId === userId && !d.revokedAt).map(d => ({ ...d }));
  }
  async revokeAllDevices(userId: string) {
    for (const d of this.devices.values()) if (d.userId === userId && !d.revokedAt) d.revokedAt = now();
  }

  async getSettings(userId: string) { return this.settings.get(userId) ?? null; }
  async putSettings(userId: string, s: SyncedSettings) {
    const v = { ...s, updatedAt: now() };
    this.settings.set(userId, v);
    return v;
  }

  async listLinks(userId: string) { return [...this.links.values()].filter(l => l.userId === userId && !l.revokedAt); }
  async revokeLink(userId: string, id: string) {
    const l = this.links.get(id);
    if (!l || l.userId !== userId || l.revokedAt) return false;
    l.revokedAt = now();
    return true;
  }

  async listFriends(userId: string) { return [...this.friends.values()].filter(f => f.ownerId === userId); }
  async addFriend(userId: string, name: string) {
    const f = { id: randomUUID(), ownerId: userId, name, createdAt: now() };
    this.friends.set(f.id, f);
    return f;
  }
  async removeFriend(userId: string, id: string) {
    const f = this.friends.get(id);
    if (!f || f.ownerId !== userId) return false;
    this.friends.delete(id);
    return true;
  }

  async deleteAccount(userId: string) {
    await this.revokeAllDevices(userId);
    for (const l of this.links.values()) if (l.userId === userId) l.revokedAt = now();
    await this.setAvatar(userId, null).catch(() => {});
    const p = this.profiles.get(userId);
    if (p) p.deletedAt = now();
    this.signOutEverywhere(userId);
  }
}
