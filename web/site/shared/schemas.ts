// zod schemas used by both the React forms and the Express API, so the rules can't drift apart.
import { z } from 'zod';

/** a–z, 0–9 and _, 3 to 20 characters (prompt 5B A1). Reserved names are checked by the server. */
export const HANDLE_RE = /^[a-z0-9_]{3,20}$/;
/** 8 characters without the look-alikes 0 O 1 I; shown and typed as XXXX-XXXX. */
export const USER_CODE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
export const USER_CODE_RE = /^[A-HJ-NP-Z2-9]{8}$/;

export const handle = z.string().trim().toLowerCase().regex(HANDLE_RE);
export const displayName = z.string().trim().min(1).max(40);
export const about = z.string().max(160);
export const email = z.string().trim().email().max(254);
export const password = z.string().min(10).max(128);

/** "kq7m 2txp", "KQ7M-2TXP" -> "KQ7M2TXP"; anything else -> null */
export function normalizeUserCode(raw: string): string | null {
  const c = raw.toUpperCase().replace(/[\s-]/g, '');
  return USER_CODE_RE.test(c) ? c : null;
}
export const formatUserCode = (c: string) => (c.length > 4 ? `${c.slice(0, 4)}-${c.slice(4)}` : c);

export const signupForm = z.object({
  displayName,
  email,
  password,
  agree: z.literal(true),
});
export type SignupForm = z.infer<typeof signupForm>;

export const loginForm = z.object({ email, password: z.string().min(1).max(128) });
export type LoginForm = z.infer<typeof loginForm>;

export const profilePatch = z
  .object({
    displayName: displayName.optional(),
    handle: handle.optional(),
    about: about.optional(),
    locale: z.enum(['en', 'th']).optional(),
    hideEmail: z.boolean().optional(),
  })
  .strict();
export type ProfilePatch = z.infer<typeof profilePatch>;

/** what a plug-in tells about itself when it asks for a code: never tracks, names or audio */
export const clientInfo = z
  .object({
    plugin: z.enum(['hub', 'track', 'app']),
    version: z.string().max(20),
    os: z.string().max(40),
    daw: z.string().max(40),
    computer: z.string().max(64),
  })
  .strict();
export type ClientInfo = z.infer<typeof clientInfo>;

export const deviceStart = z.object({ client_info: clientInfo }).strict();
export const deviceToken = z.object({ device_code: z.string().regex(/^[A-Za-z0-9_-]{43}$/) }).strict();
export const deviceLookup = z.object({ user_code: z.string().max(12) }).strict();
export const deviceApprove = z.object({ user_code: z.string().max(12), allow: z.boolean() }).strict();
export const tokenRefresh = z.object({ refresh_token: z.string().regex(/^[A-Za-z0-9_-]{43}$/) }).strict();
export const deviceRename = z.object({ name: z.string().trim().min(1).max(64) }).strict();

/** Appearance + stem names only: settings that change the sound never follow the account */
export const syncedSettings = z
  .object({
    appearance: z
      .object({
        language: z.enum(['en', 'th']).optional(),
        theme: z.enum(['system', 'light', 'dark']).optional(),
        uiScale: z.number().min(0.75).max(2).optional(),
        glassAlpha: z.number().min(0).max(1).optional(),
        hostColours: z.boolean().optional(),
        reduceMotion: z.boolean().optional(),
      })
      .strict(),
    stemNames: z.array(z.string().max(32)).max(8),
  })
  .strict();
export type SyncedSettings = z.infer<typeof syncedSettings>;

export const deleteAccount = z.object({ confirm: z.string().max(20) }).strict();
export const savedFriend = z.object({ name: z.string().trim().min(1).max(40) }).strict();

/** password strength 0..3 for the bar under the field (length first, then variety) */
export function passwordScore(pw: string): { score: 0 | 1 | 2 | 3; missing: number } {
  const missing = Math.max(0, 10 - pw.length);
  if (pw.length === 0) return { score: 0, missing };
  let s = 0;
  if (pw.length >= 10) s++;
  if (pw.length >= 14) s++;
  const kinds = [/[a-z]/, /[A-Z]/, /[0-9]/, /[^A-Za-z0-9]/, /\s/].filter(r => r.test(pw)).length;
  if (kinds >= 3 || pw.length >= 20) s++;
  if (missing > 0) s = 0;
  return { score: Math.min(3, s) as 0 | 1 | 2 | 3, missing };
}

// --- API shapes --------------------------------------------------------------------------------

export interface Me {
  id: string;
  email: string;
  emailConfirmed: boolean;
  handle: string;
  displayName: string;
  about: string;
  avatarUrl: string | null;
  locale: 'en' | 'th';
  hideEmail: boolean;
  plan: 'free';
  providers: string[];            // "email", "google", "discord"
  passwordChangedAt: string | null;
  createdAt: string;
}

export interface Device {
  id: string;
  name: string;
  os: string;
  daw: string;
  plugin: string;
  pluginVersion: string;
  createdAt: string;
  lastSeenAt: string;
  current?: boolean;
}

export interface ShareLink {
  id: string;
  kind: 'listen' | 'send';
  label: string;
  url: string | null;
  createdAt: string;
}

export interface SavedFriend {
  id: string;
  name: string;
  createdAt: string;
}

export interface PendingDevice {
  userCode: string;
  clientInfo: ClientInfo;
  createdAt: string;
}

/** email shown as a•••@example.com (streamers share their screen) */
export function maskEmail(e: string): string {
  const at = e.indexOf('@');
  if (at < 1) return '•••';
  return `${e[0]}•••${e.slice(at)}`;
}
