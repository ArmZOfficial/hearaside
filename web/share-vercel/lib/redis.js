// @ts-check
// Store backed by Redis over Upstash's REST API (what the Vercel Marketplace "Upstash for Redis"
// integration sets up). No package needed: one HTTPS request per command.
//   env: UPSTASH_REDIS_REST_URL + UPSTASH_REDIS_REST_TOKEN (or the older KV_REST_API_URL + KV_REST_API_TOKEN)

import { memoryStore } from "./directory.js";

/** @returns {import("./directory.js").Store | null} */
export function redisFromEnv(env = process.env) {
    const url = env.UPSTASH_REDIS_REST_URL || env.KV_REST_API_URL;
    const token = env.UPSTASH_REDIS_REST_TOKEN || env.KV_REST_API_TOKEN;
    if (!url || !token) return null;
    /** @param {(string | number)[]} command */
    const run = async command => {
        const res = await fetch(url, {
            method: "POST",
            headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
            body: JSON.stringify(command),
        });
        const json = await res.json();
        if (!res.ok || json.error) throw new Error(`redis ${command[0]}: ${json.error || res.status}`);
        return json.result;
    };
    return {
        async get(key) { return (await run(["GET", key])) ?? null; },
        async set(key, value, ttlSec) { await run(["SET", key, value, "EX", ttlSec]); },
        async del(key) { await run(["DEL", key]); },
        async incr(key, ttlSec) {
            const n = Number(await run(["INCR", key]));
            if (n === 1) await run(["EXPIRE", key, ttlSec]);
            return n;
        },
    };
}

/** @type {import("./directory.js").Store | null} */
let shared = null;

/** Redis when configured; otherwise an in-memory store, which only makes sense for local
 *  development (`npm run dev`): on Vercel every function instance would have its own. */
export function store() {
    if (shared) return shared;
    shared = redisFromEnv();
    if (!shared) {
        if (process.env.VERCEL) throw new Error("No Redis configured: add Upstash for Redis from the Vercel Marketplace");
        shared = memoryStore();
    }
    return shared;
}
