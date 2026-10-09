// Rate limits that work across serverless instances: counters in Upstash Redis (A7 item 4), or in
// memory for development and tests. Fixed one-minute windows, like web/share-vercel.
import type { NextFunction, Request, Response } from 'express';
import { Redis } from '@upstash/redis';

interface Counter { incr(key: string, ttlSec: number): Promise<number> }

class MemoryCounter implements Counter {
  private m = new Map<string, { n: number; until: number }>();
  async incr(key: string, ttlSec: number) {
    const now = Date.now();
    const e = this.m.get(key);
    if (!e || e.until < now) { this.m.set(key, { n: 1, until: now + ttlSec * 1000 }); return 1; }
    return ++e.n;
  }
}

class RedisCounter implements Counter {
  constructor(private r: Redis) {}
  async incr(key: string, ttlSec: number) {
    const n = await this.r.incr(key);
    if (n === 1) await this.r.expire(key, ttlSec);
    return n;
  }
}

let counter: Counter | null = null;
function get(): Counter {
  if (counter) return counter;
  const url = process.env.UPSTASH_REDIS_REST_URL, token = process.env.UPSTASH_REDIS_REST_TOKEN;
  counter = url && token ? new RedisCounter(new Redis({ url, token })) : new MemoryCounter();
  return counter;
}
/** tests start each case with fresh counters */
export function resetLimits() { counter = new MemoryCounter(); }

export function clientIp(req: Request): string {
  const fwd = String(req.headers['x-forwarded-for'] ?? '');
  return (fwd.split(',')[0] || String(req.headers['x-real-ip'] ?? '') || req.socket.remoteAddress || 'unknown').trim();
}

/** true when `who` went over `perMinute` for `what` (counts this call) */
export async function over(what: string, who: string, perMinute: number): Promise<boolean> {
  try {
    const n = await get().incr(`hs:rl:${what}:${who}:${Math.floor(Date.now() / 60000)}`, 60);
    return n > perMinute;
  } catch {
    return false;   // a limiter outage must not lock everyone out
  }
}

/** per-IP limit as middleware */
export const limit = (what: string, perMinute: number) => async (req: Request, res: Response, next: NextFunction) => {
  if (await over(what, clientIp(req), perMinute)) return res.status(429).json({ error: 'slow_down' });
  next();
};
