// The real back end: Supabase PostgreSQL + Auth + Storage through the secret key (server only).
// Tables and rules: supabase/migrations/20261009000000_accounts.sql.
import { randomBytes } from 'node:crypto';
import { createClient, type SupabaseClient, type User } from '@supabase/supabase-js';
import type { SyncedSettings } from '../../shared/schemas.js';
import { StoreError, type AuthUser, type DeviceCode, type DeviceRow, type FriendRow, type LinkRow, type Profile, type Store } from './store.js';

type Row = Record<string, unknown>;

const profileOf = (r: Row): Profile => ({
  id: r.id as string,
  handle: r.handle as string,
  displayName: (r.display_name as string) ?? '',
  about: (r.about as string) ?? '',
  avatarPath: (r.avatar_path as string | null) ?? null,
  locale: r.locale === 'th' ? 'th' : 'en',
  hideEmail: r.hide_email !== false,
  plan: 'free',
  deletedAt: (r.deleted_at as string | null) ?? null,
});
const codeOf = (r: Row): DeviceCode => ({
  deviceCodeHash: r.device_code_hash as string,
  userCode: r.user_code as string,
  clientInfo: r.client_info as DeviceCode['clientInfo'],
  status: r.status as DeviceCode['status'],
  userId: (r.user_id as string | null) ?? null,
  createdAt: r.created_at as string,
  expiresAt: r.expires_at as string,
  lastPollAt: (r.last_poll_at as string | null) ?? null,
});
const deviceOf = (r: Row): DeviceRow => ({
  id: r.id as string,
  userId: r.user_id as string,
  name: r.name as string,
  os: r.os as string,
  daw: r.daw as string,
  plugin: r.plugin as string,
  pluginVersion: r.plugin_version as string,
  refreshTokenHash: r.refresh_token_hash as string,
  prevRefreshHash: (r.prev_refresh_hash as string | null) ?? null,
  createdAt: r.created_at as string,
  lastSeenAt: r.last_seen_at as string,
  revokedAt: (r.revoked_at as string | null) ?? null,
});
const userOf = (u: User): AuthUser => ({
  id: u.id,
  email: u.email ?? '',
  emailConfirmed: !!u.email_confirmed_at,
  providers: (u.app_metadata?.providers as string[] | undefined) ?? (u.app_metadata?.provider ? [u.app_metadata.provider as string] : []),
  passwordChangedAt: (u.user_metadata?.password_changed_at as string | undefined) ?? null,
  createdAt: u.created_at,
});

const snake: Record<string, string> = {
  displayName: 'display_name', hideEmail: 'hide_email', avatarPath: 'avatar_path',
  refreshTokenHash: 'refresh_token_hash', prevRefreshHash: 'prev_refresh_hash', lastSeenAt: 'last_seen_at', revokedAt: 'revoked_at',
  pluginVersion: 'plugin_version', userId: 'user_id', lastPollAt: 'last_poll_at', createdAt: 'created_at',
};
const toRow = (o: Record<string, unknown>) =>
  Object.fromEntries(Object.entries(o).filter(([, v]) => v !== undefined).map(([k, v]) => [snake[k] ?? k, v]));

function check<T>(r: { data: T; error: { message: string; code?: string } | null }): T {
  if (r.error) {
    if (r.error.code === '23505') throw new StoreError('handle_taken');
    if (r.error.code === '23514') throw new StoreError('handle_reserved');
    throw new Error(`database: ${r.error.message}`);
  }
  return r.data;
}
/** a select / update that returns rows: never null */
const list = <T,>(r: { data: T[] | null; error: { message: string; code?: string } | null }): T[] => check(r) ?? [];

export class SupabaseStore implements Store {
  private db: SupabaseClient;
  constructor(private url = process.env.SUPABASE_URL ?? process.env.VITE_SUPABASE_URL ?? '', key = process.env.SUPABASE_SECRET_KEY ?? '') {
    if (!url || !key) throw new Error('SUPABASE_URL and SUPABASE_SECRET_KEY must be set (or STORE=memory)');
    this.db = createClient(url, key, { auth: { persistSession: false, autoRefreshToken: false } });
  }

  async userFromSession(token: string) {
    const { data, error } = await this.db.auth.getUser(token);
    return error || !data.user ? null : userOf(data.user);
  }
  async getUser(id: string) {
    const { data, error } = await this.db.auth.admin.getUserById(id);
    return error || !data.user ? null : userOf(data.user);
  }

  async getProfile(id: string) {
    const r = check(await this.db.from('profiles').select('*').eq('id', id).maybeSingle());
    return r ? profileOf(r) : null;
  }
  async updateProfile(id: string, patch: Partial<Profile>) {
    const r = check(await this.db.from('profiles').update(toRow(patch)).eq('id', id).select('*').single());
    return profileOf(r);
  }
  async handleAvailable(handle: string, exceptUserId?: string) {
    const h = handle.toLowerCase();
    const reserved = check(await this.db.from('reserved_handles').select('handle').eq('handle', h).maybeSingle());
    if (reserved) return false;
    let q = this.db.from('profiles').select('id').eq('handle', h);
    if (exceptUserId) q = q.neq('id', exceptUserId);
    const rows = list(await q.limit(1));
    return rows.length === 0;
  }
  async setAvatar(id: string, webp: Buffer | null) {
    const old = await this.getProfile(id);
    let path: string | null = null;
    if (webp) {
      path = `${id}/${randomBytes(8).toString('hex')}.webp`;
      const up = await this.db.storage.from('avatars').upload(path, webp, { contentType: 'image/webp', cacheControl: '31536000', upsert: false });
      if (up.error) throw new Error(`storage: ${up.error.message}`);
    }
    check(await this.db.from('profiles').update({ avatar_path: path }).eq('id', id));
    if (old?.avatarPath) await this.db.storage.from('avatars').remove([old.avatarPath]);
    return path;
  }
  avatarUrl(path: string | null) {
    return path ? `${this.url}/storage/v1/object/public/avatars/${path}` : null;
  }

  async createDeviceCode(c: DeviceCode) {
    check(await this.db.from('device_codes').insert({
      device_code_hash: c.deviceCodeHash, user_code: c.userCode, client_info: c.clientInfo, status: c.status,
      created_at: c.createdAt, expires_at: c.expiresAt,
    }));
  }
  async deviceCodeByUserCode(userCode: string) {
    const r = check(await this.db.from('device_codes').select('*').eq('user_code', userCode).maybeSingle());
    return r ? codeOf(r) : null;
  }
  async deviceCodeByHash(hash: string) {
    const r = check(await this.db.from('device_codes').select('*').eq('device_code_hash', hash).maybeSingle());
    return r ? codeOf(r) : null;
  }
  async updateDeviceCode(hash: string, patch: Partial<DeviceCode>) {
    check(await this.db.from('device_codes').update(toRow(patch)).eq('device_code_hash', hash));
  }

  async createDevice(d: DeviceRow) {
    check(await this.db.from('devices').insert(toRow({ ...d })));
  }
  async deviceById(id: string) {
    const r = check(await this.db.from('devices').select('*').eq('id', id).maybeSingle());
    return r ? deviceOf(r) : null;
  }
  async deviceByRefreshHash(hash: string) {
    const r = check(await this.db.from('devices').select('*').eq('refresh_token_hash', hash).maybeSingle());
    return r ? deviceOf(r) : null;
  }
  async deviceByPrevRefreshHash(hash: string) {
    const r = list(await this.db.from('devices').select('*').eq('prev_refresh_hash', hash).limit(1));
    return r[0] ? deviceOf(r[0]) : null;
  }
  async updateDevice(id: string, patch: Partial<DeviceRow>) {
    check(await this.db.from('devices').update(toRow(patch)).eq('id', id));
  }
  async listDevices(userId: string) {
    const r = list(await this.db.from('devices').select('*').eq('user_id', userId).is('revoked_at', null).order('last_seen_at', { ascending: false }));
    return r.map(deviceOf);
  }
  async revokeAllDevices(userId: string) {
    check(await this.db.from('devices').update({ revoked_at: new Date().toISOString() }).eq('user_id', userId).is('revoked_at', null));
  }

  async getSettings(userId: string) {
    const r = check(await this.db.from('settings_sync').select('*').eq('user_id', userId).maybeSingle());
    return r ? { appearance: r.appearance as SyncedSettings['appearance'], stemNames: r.stem_names as string[], updatedAt: r.updated_at as string } : null;
  }
  async putSettings(userId: string, s: SyncedSettings) {
    const r = check(await this.db.from('settings_sync').upsert({ user_id: userId, appearance: s.appearance, stem_names: s.stemNames }).select('*').single());
    return { appearance: r.appearance as SyncedSettings['appearance'], stemNames: r.stem_names as string[], updatedAt: r.updated_at as string };
  }

  async listLinks(userId: string): Promise<LinkRow[]> {
    const r = list(await this.db.from('share_links').select('*').eq('user_id', userId).is('revoked_at', null).order('created_at'));
    return r.map(x => ({ id: x.id, userId: x.user_id, kind: x.kind, label: x.label, createdAt: x.created_at, revokedAt: x.revoked_at }));
  }
  async revokeLink(userId: string, id: string) {
    const r = list(await this.db.from('share_links').update({ revoked_at: new Date().toISOString() }).eq('id', id).eq('user_id', userId).is('revoked_at', null).select('id'));
    return r.length > 0;
  }

  async listFriends(userId: string): Promise<FriendRow[]> {
    const r = list(await this.db.from('saved_friends').select('*').eq('owner_id', userId).order('created_at'));
    return r.map(x => ({ id: x.id, ownerId: x.owner_id, name: x.name, createdAt: x.created_at }));
  }
  async addFriend(userId: string, name: string) {
    const x = check(await this.db.from('saved_friends').insert({ owner_id: userId, name }).select('*').single());
    return { id: x.id, ownerId: x.owner_id, name: x.name, createdAt: x.created_at };
  }
  async removeFriend(userId: string, id: string) {
    const r = list(await this.db.from('saved_friends').delete().eq('id', id).eq('owner_id', userId).select('id'));
    return r.length > 0;
  }

  async deleteAccount(userId: string) {
    await this.revokeAllDevices(userId);
    check(await this.db.from('share_links').update({ revoked_at: new Date().toISOString() }).eq('user_id', userId).is('revoked_at', null));
    await this.setAvatar(userId, null);
    check(await this.db.from('profiles').update({ deleted_at: new Date().toISOString() }).eq('id', userId));
    // no new sign-ins or session refreshes until purge_expired() removes the user after 30 days
    await this.db.auth.admin.updateUserById(userId, { ban_duration: '876000h' });
  }
}
