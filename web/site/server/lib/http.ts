import type { Request, Response } from 'express';
import type { ZodType } from 'zod';
import type { Me } from '../../shared/schemas.js';
import type { AuthUser, Profile, Store } from './store.js';

export const siteUrl = () => (process.env.SITE_URL ?? 'https://hearaside.vercel.app').replace(/\/$/, '');

/** parses the JSON body with a shared zod schema; answers 400 itself when it doesn't fit */
export function body<T>(schema: ZodType<T>, req: Request, res: Response): T | null {
  const r = schema.safeParse(req.body ?? {});
  if (!r.success) {
    res.status(400).json({ error: 'bad_request', fields: r.error.issues.map(i => i.path.join('.')) });
    return null;
  }
  return r.data;
}

export function meOf(store: Store, p: Profile, u: AuthUser): Me {
  return {
    id: p.id, email: u.email, emailConfirmed: u.emailConfirmed, handle: p.handle, displayName: p.displayName, about: p.about,
    avatarUrl: store.avatarUrl(p.avatarPath), locale: p.locale, hideEmail: p.hideEmail, plan: 'free',
    providers: u.providers, passwordChangedAt: u.passwordChangedAt, createdAt: u.createdAt,
  };
}

