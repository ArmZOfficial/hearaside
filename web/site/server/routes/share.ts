// Permanent share links on the account website (A6 item 2): the same directory as web/share-vercel
// (token -> the Hub's tunnel; audio never passes through here), so a signed-in Hub needs no website
// of its own. A register call with the plug-in's access token also records the links on the account,
// where Account › Links and friends lists and revokes them.
//   POST /api/register   {tunnel, secret, tokens: {l, s, f}}   the Hub, every 30 s
//   POST /api/unregister {secret, tokens}                      sharing switched off
//   GET  /api/resolve/:token                                   the listen / send-in page
import { Router, type Request } from 'express';
import { register, resolve, unregister, sha256, LIMITS, type DirStore } from '../share/directory.js';
import { store as redisStore } from '../share/redis.js';
import { clientIp, over } from '../lib/limit.js';
import { verifyAccess } from '../lib/tokens.js';
import type { Store } from '../lib/store.js';

export function shareRoutes(store: Store, dir: () => DirStore = redisStore) {
  const r = Router();

  const signedInHub = async (req: Request) => {
    const m = /^Bearer\s+(\S{10,4096})$/.exec(req.headers.authorization ?? '');
    const p = m ? await verifyAccess(m[1]) : null;
    if (!p) return null;
    const d = await store.deviceById(p.deviceId);
    return d && !d.revokedAt && d.userId === p.userId ? p.userId : null;
  };

  r.post('/register', async (req, res, next) => {
    try {
      if (await over('register', clientIp(req), LIMITS.register)) return res.status(429).json({ error: 'slow down' });
      const out = await register(dir(), req.body);
      if (out.status === 200) {
        const userId = await signedInHub(req).catch(() => null);
        const tokens = (out.body.tokens as { kind: string; token: string }[] | undefined) ?? [];
        // recording on the account never decides whether the link works
        if (userId) await store.recordLinks(userId, tokens.map(t => ({ kind: t.kind === 'l' ? 'listen' as const : 'send' as const, tokenHash: sha256(t.token) })))
                               .catch(e => console.error('share links:', e instanceof Error ? e.message : e));
      }
      res.status(out.status).json(out.body);
    } catch (e) { next(e); }
  });

  r.post('/unregister', async (req, res, next) => {
    try {
      if (await over('register', clientIp(req), LIMITS.register)) return res.status(429).json({ error: 'slow down' });
      const out = await unregister(dir(), req.body);
      res.status(out.status).json(out.body);
    } catch (e) { next(e); }
  });

  r.get('/resolve/:token', async (req, res) => {
    try {
      if (await over('resolve', clientIp(req), LIMITS.resolve)) return res.status(429).json({ online: false, retry: true });
      const out = await resolve(dir(), String(req.params.token));
      res.status(out.status).json(out.body);
    } catch (e) {
      console.error('share directory:', e instanceof Error ? e.message : e);
      res.status(503).json({ online: false, retry: true });   // the page keeps trying
    }
  });

  const handleTurn = async (req: Request, res: any, next: any) => {
    try {
      if (await over('turn', clientIp(req), { perMinute: 30, perDay: 500 })) {
        return res.status(429).json({ error: 'slow down' });
      }
      const userId = await signedInHub(req);
      if (!userId) {
        return res.status(401).json({ error: 'unauthorized' });
      }

      const keyId = process.env.CLOUDFLARE_TURN_KEY_ID;
      const apiToken = process.env.CLOUDFLARE_TURN_API_TOKEN;

      if (keyId && apiToken) {
        try {
          const cfRes = await fetch(
            `https://rtc.live.cloudflare.com/v1/turn/keys/${keyId}/credentials/generate-ice-servers`,
            {
              method: 'POST',
              headers: {
                'Authorization': `Bearer ${apiToken}`,
                'Content-Type': 'application/json',
              },
              body: JSON.stringify({ ttl: 86400 }),
            }
          );
          if (cfRes.ok) {
            const data = await cfRes.json();
            return res.status(200).json(data);
          }
        } catch (err) {
          console.error('turn credential generation error:', err instanceof Error ? err.message : err);
        }
      }

      return res.status(200).json({
        iceServers: [
          { urls: ['stun:stun.cloudflare.com:3478'] },
          { urls: ['stun:stun.l.google.com:19302'] }
        ]
      });
    } catch (e) { next(e); }
  };

  r.get('/turn', handleTurn);
  r.post('/turn', handleTurn);

  return r;
}
