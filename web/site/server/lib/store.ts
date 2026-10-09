// The account data the API needs, behind one interface with two back ends:
//   memoryStore    development, tests and the plug-in mock server (STORE=memory)
//   supabaseStore  the real site: PostgreSQL + Auth + Storage on Supabase, through the secret key
// Routes never talk to Supabase directly, so the back end can move (prompt 5B.0 item 2).
import type { ClientInfo, SyncedSettings } from '../../shared/schemas.js';

export interface AuthUser {
  id: string;
  email: string;
  emailConfirmed: boolean;
  providers: string[];
  passwordChangedAt: string | null;
  createdAt: string;
}

export interface Profile {
  id: string;
  handle: string;
  displayName: string;
  about: string;
  avatarPath: string | null;
  locale: 'en' | 'th';
  hideEmail: boolean;
  plan: 'free';
  deletedAt: string | null;
}

export interface DeviceCode {
  deviceCodeHash: string;
  userCode: string;
  clientInfo: ClientInfo;
  status: 'pending' | 'approved' | 'denied' | 'used';
  userId: string | null;
  createdAt: string;
  expiresAt: string;
  lastPollAt: string | null;
}

export interface DeviceRow {
  id: string;
  userId: string;
  name: string;
  os: string;
  daw: string;
  plugin: string;
  pluginVersion: string;
  refreshTokenHash: string;
  prevRefreshHash: string | null;
  createdAt: string;
  lastSeenAt: string;
  revokedAt: string | null;
}

export interface LinkRow { id: string; userId: string; kind: 'listen' | 'send'; label: string; createdAt: string; revokedAt: string | null }
export interface FriendRow { id: string; ownerId: string; name: string; createdAt: string }

export class StoreError extends Error {
  constructor(public code: 'handle_taken' | 'handle_reserved' | 'not_found' | 'conflict') {
    super(code);
  }
}

export interface Store {
  /** the signed-in website user from a session token (Supabase access token), or null */
  userFromSession(token: string): Promise<AuthUser | null>;
  getUser(id: string): Promise<AuthUser | null>;

  getProfile(id: string): Promise<Profile | null>;
  /** throws StoreError('handle_taken' | 'handle_reserved') */
  updateProfile(id: string, patch: Partial<Pick<Profile, 'handle' | 'displayName' | 'about' | 'locale' | 'hideEmail'>>): Promise<Profile>;
  handleAvailable(handle: string, exceptUserId?: string): Promise<boolean>;
  /** stores a finished 512 x 512 WebP (or removes the photo with null); returns the new path */
  setAvatar(id: string, webp: Buffer | null): Promise<string | null>;
  avatarUrl(path: string | null): string | null;

  createDeviceCode(c: DeviceCode): Promise<void>;
  deviceCodeByUserCode(userCode: string): Promise<DeviceCode | null>;
  deviceCodeByHash(hash: string): Promise<DeviceCode | null>;
  updateDeviceCode(hash: string, patch: Partial<Pick<DeviceCode, 'status' | 'userId' | 'lastPollAt'>>): Promise<void>;

  createDevice(d: DeviceRow): Promise<void>;
  deviceById(id: string): Promise<DeviceRow | null>;
  deviceByRefreshHash(hash: string): Promise<DeviceRow | null>;
  deviceByPrevRefreshHash(hash: string): Promise<DeviceRow | null>;
  updateDevice(id: string, patch: Partial<Pick<DeviceRow, 'name' | 'refreshTokenHash' | 'prevRefreshHash' | 'lastSeenAt' | 'revokedAt' | 'pluginVersion' | 'daw' | 'os'>>): Promise<void>;
  listDevices(userId: string): Promise<DeviceRow[]>;
  revokeAllDevices(userId: string): Promise<void>;

  getSettings(userId: string): Promise<(SyncedSettings & { updatedAt: string }) | null>;
  putSettings(userId: string, s: SyncedSettings): Promise<SyncedSettings & { updatedAt: string }>;

  listLinks(userId: string): Promise<LinkRow[]>;
  revokeLink(userId: string, id: string): Promise<boolean>;

  listFriends(userId: string): Promise<FriendRow[]>;
  addFriend(userId: string, name: string): Promise<FriendRow>;
  removeFriend(userId: string, id: string): Promise<boolean>;

  /** "Delete account": revoke devices + links and remove the photo now; the rest within 30 days */
  deleteAccount(userId: string): Promise<void>;
}
