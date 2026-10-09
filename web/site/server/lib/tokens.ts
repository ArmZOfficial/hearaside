// Plug-in tokens (prompt A4 item 2) — our own, never the website's Supabase session:
//   refresh token  256 random bits (base64url, 43 chars); only its SHA-256 is stored; rotated on
//                  every refresh; an old one coming back = stolen -> the whole device is revoked
//   access token   JWT, 15 minutes, HS256 with PLUGIN_TOKEN_SECRET, aud=plugin, sub=user, did=device
import { createHash, randomBytes, randomInt } from 'node:crypto';
import { SignJWT, jwtVerify } from 'jose';
import { USER_CODE_ALPHABET } from '../../shared/schemas.js';

export const ACCESS_TTL_SEC = 15 * 60;
export const DEVICE_CODE_TTL_SEC = 10 * 60;
export const POLL_INTERVAL_SEC = 5;

export const sha256 = (s: string) => createHash('sha256').update(s).digest('hex');
export const randomToken = () => randomBytes(32).toString('base64url');

/** 8 characters from the alphabet without 0 O 1 I */
export function newUserCode(): string {
  let c = '';
  for (let i = 0; i < 8; i++) c += USER_CODE_ALPHABET[randomInt(USER_CODE_ALPHABET.length)];
  return c;
}

function secret(): Uint8Array {
  const s = process.env.PLUGIN_TOKEN_SECRET;
  if (!s || s.length < 32) {
    if (process.env.STORE === 'memory' || process.env.NODE_ENV === 'test') return new TextEncoder().encode('dev-only-plugin-token-secret-not-for-production');
    throw new Error('PLUGIN_TOKEN_SECRET is not set (32+ characters)');
  }
  return new TextEncoder().encode(s);
}

export async function signAccess(userId: string, deviceId: string): Promise<string> {
  return new SignJWT({ did: deviceId })
    .setProtectedHeader({ alg: 'HS256' })
    .setSubject(userId)
    .setAudience('plugin')
    .setIssuer('hearaside')
    .setIssuedAt()
    .setExpirationTime(`${ACCESS_TTL_SEC}s`)
    .sign(secret());
}

export async function verifyAccess(token: string): Promise<{ userId: string; deviceId: string } | null> {
  try {
    const { payload } = await jwtVerify(token, secret(), { audience: 'plugin', issuer: 'hearaside', algorithms: ['HS256'] });
    if (typeof payload.sub !== 'string' || typeof payload.did !== 'string') return null;
    return { userId: payload.sub, deviceId: payload.did };
  } catch {
    return null;
  }
}

