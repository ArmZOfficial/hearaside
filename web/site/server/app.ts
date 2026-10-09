// The account API as one Express app. On Vercel it runs as a single function (api/index.ts); in
// development `server/dev.ts` listens on :3001 and Vite proxies /api to it.
//   STORE=memory  accounts in memory (development, tests, plug-in mock server)
//   otherwise     Supabase (SUPABASE_URL + SUPABASE_SECRET_KEY)
// Audio never passes through here: listen / send-in links go Hub -> cloudflared -> browser.
import express, { type ErrorRequestHandler } from 'express';
import helmet from 'helmet';
import { MemoryStore } from './lib/memoryStore.js';
import { SupabaseStore } from './lib/supabaseStore.js';
import type { Store } from './lib/store.js';
import { deviceRoutes } from './routes/device.js';
import { meRoutes } from './routes/me.js';
import { devRoutes } from './routes/dev.js';

export function createApp(store: Store = process.env.STORE === 'memory' ? new MemoryStore() : new SupabaseStore()) {
  const app = express();
  app.disable('x-powered-by');
  app.set('trust proxy', 1);   // Vercel's proxy sets X-Forwarded-For
  app.use(helmet({ contentSecurityPolicy: { directives: { defaultSrc: ["'none'"], frameAncestors: ["'none'"] } }, crossOriginResourcePolicy: { policy: 'same-origin' } }));
  app.use((_req, res, next) => { res.setHeader('Cache-Control', 'no-store'); next(); });
  app.use(express.json({ limit: '16kb' }));

  const v1 = express.Router();
  v1.get('/health', (_req, res) => { res.json({ ok: true }); });
  v1.use(deviceRoutes(store));
  v1.use(meRoutes(store));
  if (store instanceof MemoryStore) v1.use(devRoutes(store));
  app.use('/api/v1', v1);

  app.use('/api', (_req, res) => { res.status(404).json({ error: 'not_found' }); });

  // errors are logged without bodies, tokens or emails, and never swallowed
  const onError: ErrorRequestHandler = (err, req, res, _next) => {
    if (err?.type === 'entity.parse.failed' || err?.type === 'entity.too.large') {
      res.status(400).json({ error: 'bad_request' });
      return;
    }
    console.error(`api ${req.method} ${req.path}:`, err instanceof Error ? err.message : err);
    res.status(500).json({ error: 'server' });
  };
  app.use(onError);
  return app;
}

let app: ReturnType<typeof createApp> | null = null;
/** the app Vercel imports (created on first use, so a missing env var fails a request, not the import) */
export default function handler(req: express.Request, res: express.Response) {
  app ??= createApp();
  return app(req, res);
}
