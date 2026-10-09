// @ts-check
// HEARASIDE share directory: token -> the Hub's current tunnel address. The audio never passes
// through here; the browser connects to the Hub through cloudflared (docs/ux-roadmap.md, 8).
//
// Keys (Redis or any Store):
//   hs:t:<token>      {"kind":"l"|"s","tunnel":"https://x.trycloudflare.com","updatedAt":ms}  TTL 90 s
//   hs:owner:<token>  sha-256 of the owner secret (hex)                                      TTL 180 days
//   hs:rl:<what>:<ip>:<minute>  request counter (rate limit)                                  TTL 60 s

import { createHash } from "node:crypto";

export const ENTRY_TTL_SEC = 90;
export const OWNER_TTL_SEC = 180 * 24 * 3600;
export const TOKEN_RE = /^[a-z0-9]{20,64}$/;
export const SECRET_RE = /^[0-9a-f]{64}$/;
const QUICK_TUNNEL_RE = /^https:\/\/[a-z0-9-]+\.trycloudflare\.com$/;
export const LIMITS = { register: 30, resolve: 240 };   // requests per IP per minute

/**
 * @typedef {{ get(key: string): Promise<string | null>,
 *             set(key: string, value: string, ttlSec: number): Promise<void>,
 *             del(key: string): Promise<void>,
 *             incr(key: string, ttlSec: number): Promise<number> }} Store
 * @typedef {{ status: number, body: Record<string, unknown> }} Result
 */

/** A tunnel address the directory accepts: a cloudflared quick tunnel, or a host listed in
 *  HEARASIDE_TUNNEL_HOSTS (comma separated, e.g. "live.example.com"). Nothing else, so a link
 *  can never send listeners to an arbitrary site. */
export function tunnelAllowed(url, extraHosts = process.env.HEARASIDE_TUNNEL_HOSTS || "") {
    if (typeof url !== "string" || url.length > 200) return false;
    if (QUICK_TUNNEL_RE.test(url)) return true;
    const hosts = extraHosts.split(",").map(h => h.trim().toLowerCase()).filter(Boolean);
    const m = /^https:\/\/([a-z0-9.-]+)$/.exec(url);
    return !!m && hosts.includes(m[1]);
}

const sha256 = (/** @type {string} */ s) => createHash("sha256").update(s).digest("hex");

/** Every miss looks the same: unknown token, expired entry, bad input. */
const offline = () => ({ status: 200, body: { online: false } });

/** @param {Store} store @param {string} what @param {string} ip @param {number} now */
export async function overLimit(store, what, ip, now = Date.now()) {
    const limit = LIMITS[/** @type {keyof typeof LIMITS} */ (what)] ?? 60;
    const n = await store.incr(`hs:rl:${what}:${ip}:${Math.floor(now / 60000)}`, 60);
    return n > limit;
}

/** POST /api/register {tunnel, secret, tokens: {l, s}} (also the heartbeat, every 30 s)
 * @param {Store} store @param {any} body @param {number} now @returns {Promise<Result>} */
export async function register(store, body, now = Date.now()) {
    const tokens = body && typeof body.tokens === "object" && body.tokens ? body.tokens : {};
    const list = /** @type {[string, string][]} */ (Object.entries(tokens).filter(([k]) => k === "l" || k === "s"));
    if (!body || !SECRET_RE.test(body.secret ?? "") || list.length === 0 || !list.every(([, t]) => TOKEN_RE.test(t)))
        return { status: 400, body: { error: "bad request" } };
    if (!tunnelAllowed(body.tunnel)) return { status: 400, body: { error: "tunnel not allowed" } };
    if (new Set(list.map(([, t]) => t)).size !== list.length) return { status: 400, body: { error: "bad request" } };
    const hash = sha256(body.secret);
    // every token must be free or ours before anything is written
    for (const [, token] of list) {
        const owner = await store.get(`hs:owner:${token}`);
        if (owner !== null && owner !== hash) return { status: 403, body: { error: "not the owner" } };
    }
    for (const [kind, token] of list) {
        await store.set(`hs:owner:${token}`, hash, OWNER_TTL_SEC);
        await store.set(`hs:t:${token}`, JSON.stringify({ kind, tunnel: body.tunnel, updatedAt: now }), ENTRY_TTL_SEC);
    }
    return { status: 200, body: { ok: true, ttl: ENTRY_TTL_SEC } };
}

/** POST /api/unregister {secret, tokens: {l, s}} - sharing was switched off
 * @param {Store} store @param {any} body @returns {Promise<Result>} */
export async function unregister(store, body) {
    const tokens = body && typeof body.tokens === "object" && body.tokens ? Object.values(body.tokens) : [];
    if (!body || !SECRET_RE.test(body.secret ?? "") || tokens.length === 0 || !tokens.every(t => TOKEN_RE.test(String(t))))
        return { status: 400, body: { error: "bad request" } };
    const hash = sha256(body.secret);
    for (const token of tokens) {
        const owner = await store.get(`hs:owner:${token}`);
        if (owner !== hash) return { status: 403, body: { error: "not the owner" } };
    }
    for (const token of tokens) await store.del(`hs:t:${token}`);
    return { status: 200, body: { ok: true } };
}

/** GET /api/resolve/<token> -> {online: true, ws: "wss://.../ws/l/<token>"} | {online: false}
 * @param {Store} store @param {string} token @returns {Promise<Result>} */
export async function resolve(store, token) {
    if (!TOKEN_RE.test(token ?? "")) return offline();
    const raw = await store.get(`hs:t:${token}`);
    if (!raw) return offline();
    let entry;
    try { entry = JSON.parse(raw); } catch { return offline(); }
    if (!tunnelAllowed(entry.tunnel) || (entry.kind !== "l" && entry.kind !== "s")) return offline();
    const ws = entry.tunnel.replace(/^https:/, "wss:") + `/ws/${entry.kind}/${token}`;
    return { status: 200, body: { online: true, ws } };
}

/** In-memory Store (tests, local development). */
export function memoryStore() {
    /** @type {Map<string, {v: string, exp: number}>} */
    const m = new Map();
    const live = (/** @type {string} */ k) => {
        const e = m.get(k);
        if (e && e.exp <= Date.now()) { m.delete(k); return undefined; }
        return e;
    };
    return {
        map: m,
        async get(/** @type {string} */ k) { return live(k)?.v ?? null; },
        async set(/** @type {string} */ k, /** @type {string} */ v, /** @type {number} */ ttl) { m.set(k, { v, exp: Date.now() + ttl * 1000 }); },
        async del(/** @type {string} */ k) { m.delete(k); },
        async incr(/** @type {string} */ k, /** @type {number} */ ttl) {
            const e = live(k);
            const n = (e ? Number(e.v) : 0) + 1;
            m.set(k, { v: String(n), exp: e ? e.exp : Date.now() + ttl * 1000 });
            return n;
        },
    };
}
