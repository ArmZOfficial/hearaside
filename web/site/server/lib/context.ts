// Who is calling: a website session (Supabase access token) or a plug-in (our access JWT).
// Both arrive as "Authorization: Bearer ...". Some routes are website-only (email, password,
// deleting the account, signing other devices out) and refuse plug-in tokens.
import type { NextFunction, Request, Response } from 'express';
import type { Store } from './store.js';
import { verifyAccess } from './tokens.js';

export interface Caller { userId: string; via: 'web' | 'plugin'; deviceId?: string }

declare module 'express-serve-static-core' {
  interface Request { caller?: Caller }
}

function bearer(req: Request): string | null {
  const h = req.headers.authorization ?? '';
  const m = /^Bearer\s+([A-Za-z0-9._~+/=-]{10,4096})$/.exec(h);
  return m ? m[1] : null;
}

export function authenticate(store: Store, opts: { webOnly?: boolean } = {}) {
  return async (req: Request, res: Response, next: NextFunction) => {
    const token = bearer(req);
    if (!token) return res.status(401).json({ error: 'sign_in' });
    try {
      const plugin = await verifyAccess(token);
      if (plugin) {
        if (opts.webOnly) return res.status(403).json({ error: 'website_only' });
        const d = await store.deviceById(plugin.deviceId);
        if (!d || d.revokedAt || d.userId !== plugin.userId) return res.status(401).json({ error: 'signed_out' });
        req.caller = { userId: plugin.userId, via: 'plugin', deviceId: plugin.deviceId };
        return next();
      }
      const user = await store.userFromSession(token);
      if (!user) return res.status(401).json({ error: 'sign_in' });
      const p = await store.getProfile(user.id);
      if (!p || p.deletedAt) return res.status(401).json({ error: 'sign_in' });
      req.caller = { userId: user.id, via: 'web' };
      next();
    } catch (e) {
      next(e);
    }
  };
}
