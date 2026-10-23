// The real back end: Supabase PostgreSQL + Auth + Storage through the secret key (server only).
// Tables and rules: supabase/migrations/20261009000000_accounts.sql.
import { randomBytes } from 'node:crypto';
import { createClient, type SupabaseClient, type User } from '@supabase/supabase-js';
import type { SyncedSettings } from '../../shared/schemas.js';
import { BASE_DAY, StoreError, type AdminCounts, type AdminUserRow, type DownloadRow, type AuthUser, type DeviceCode, type DeviceRow, type FriendRow, type LinkRow, type Profile, type Store } from './store.js';

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

  async claimDeviceCode(hash: string) {
    const r = list(await this.db.from('device_codes').update({ status: 'used' }).eq('device_code_hash', hash).eq('status', 'approved').select('device_code_hash'));
    return r.length > 0;
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
    return r.map(x => ({ id: x.id, userId: x.user_id, kind: x.kind, label: x.label, createdAt: x.created_at, revokedAt: x.revoked_at, tokenHash: x.token_hash }));
  }
  async recordLinks(userId: string, links: { kind: 'listen' | 'send'; tokenHash: string }[]) {
    if (links.length === 0) return;
    check(await this.db.from('share_links').upsert(links.map(l => ({ user_id: userId, kind: l.kind, token_hash: l.tokenHash })),
                                                    { onConflict: 'token_hash', ignoreDuplicates: true }));
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

  async bumpDownload(version: string) {
    check(await this.db.rpc('bump_download', { p_version: version }));
  }
  async setDownloadBase(version: string, source: 'github' | 'manual', count: number) {
    check(await this.db.from('download_daily').upsert({ day: BASE_DAY, version, source, count }, { onConflict: 'day,version,source' }));
  }
  async downloadRows(): Promise<DownloadRow[]> {
    return list(await this.db.from('download_daily').select('*').limit(10000)).map(r => ({ day: r.day, version: r.version, source: r.source, count: r.count }));
  }

  async adminCounts(): Promise<AdminCounts> {
    const count = async (q: PromiseLike<{ count: number | null; error: { message: string; code?: string } | null }>) => {
      const r = await q;
      if (r.error) throw new Error(`database: ${r.error.message}`);
      return r.count ?? 0;
    };
    const head = { count: 'exact', head: true } as const;
    const week = new Date(Date.now() - 7 * 86_400_000).toISOString(), month = new Date(Date.now() - 30 * 86_400_000).toISOString();
    const [users, deleted, devices, activeDevices7d, links, recent] = await Promise.all([
      count(this.db.from('profiles').select('id', head)),
      count(this.db.from('profiles').select('id', head).not('deleted_at', 'is', null)),
      count(this.db.from('devices').select('id', head).is('revoked_at', null)),
      count(this.db.from('devices').select('id', head).is('revoked_at', null).gte('last_seen_at', week)),
      count(this.db.from('share_links').select('id', head).is('revoked_at', null)),
      this.db.from('profiles').select('created_at').gte('created_at', month).limit(20000),
    ]);
    const per = new Map<string, number>();
    for (const r of list(recent)) { const d = String(r.created_at).slice(0, 10); per.set(d, (per.get(d) ?? 0) + 1); }
    return { users, deleted, devices, activeDevices7d, links, signups: [...per].sort(([a], [b]) => a.localeCompare(b)).map(([day, c]) => ({ day, count: c })) };
  }

  async adminUsers(q: string, offset: number, limit: number) {
    const needle = q.trim().replace(/[%,()*]/g, '');
    let ids: string[] | null = null;
    if (needle.includes('@')) {
      // email lives in auth.users: walk the (small) user list for a match
      const found: string[] = [];
      for (let page = 1; page <= 20; page++) {
        const { data, error } = await this.db.auth.admin.listUsers({ page, perPage: 200 });
        if (error) throw new Error(`auth: ${error.message}`);
        found.push(...data.users.filter(u => (u.email ?? '').toLowerCase().includes(needle.toLowerCase())).map(u => u.id));
        if (data.users.length < 200) break;
      }
      ids = found;
    }
    let sel = this.db.from('profiles').select('*', { count: 'exact' });
    if (ids) sel = sel.in('id', ids.length ? ids : ['00000000-0000-0000-0000-000000000000']);
    else if (needle) sel = sel.or(`handle.ilike.%${needle}%,display_name.ilike.%${needle}%`);
    const r = await sel.order('created_at', { ascending: false }).range(offset, offset + limit - 1);
    const rows = list(r);
    const total = r.count ?? rows.length;
    const pids = rows.map(p => p.id as string);
    const [devs, lks, users] = await Promise.all([
      pids.length ? this.db.from('devices').select('user_id,last_seen_at').in('user_id', pids).is('revoked_at', null) : { data: [], error: null },
      pids.length ? this.db.from('share_links').select('user_id').in('user_id', pids).is('revoked_at', null) : { data: [], error: null },
      Promise.all(pids.map(id => this.getUser(id))),
    ]);
    const devRows = list(devs as never) as Row[], linkRows = list(lks as never) as Row[];
    const out: AdminUserRow[] = rows.map((p, i) => {
      const mine = devRows.filter(d => d.user_id === p.id);
      return {
        id: p.id, email: users[i]?.email ?? '', handle: p.handle, displayName: p.display_name ?? '', createdAt: p.created_at,
        lastSeenAt: mine.map(d => d.last_seen_at as string).sort().pop() ?? null, devices: mine.length,
        links: linkRows.filter(l => l.user_id === p.id).length, deletedAt: p.deleted_at ?? null,
      };
    });
    return { total, users: out };
  }
}
