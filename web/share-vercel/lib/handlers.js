// @ts-check
// Web-standard request handlers (Request -> Response) shared by the Vercel functions in api/
// and the local development server (scripts/dev-server.mjs).

import { overLimit, register, resolve, unregister } from "./directory.js";
import { store } from "./redis.js";

const json = (/** @type {number} */ status, /** @type {unknown} */ body) =>
    new Response(JSON.stringify(body), {
        status,
        headers: { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" },
    });

/** The caller's address as Vercel's proxy reports it. */
function clientIp(/** @type {Request} */ req) {
    const fwd = req.headers.get("x-forwarded-for") || "";
    return (fwd.split(",")[0] || req.headers.get("x-real-ip") || "unknown").trim();
}

async function readBody(/** @type {Request} */ req) {
    if (Number(req.headers.get("content-length") || 0) > 4096) return null;
    try {
        const text = await req.text();
        return text.length > 4096 ? null : JSON.parse(text);
    } catch {
        return null;
    }
}

/** @param {(s: import("./directory.js").Store, body: any) => Promise<import("./directory.js").Result>} fn */
const post = fn => async (/** @type {Request} */ req) => {
    if (req.method !== "POST") return json(405, { error: "POST only" });
    try {
        const s = store();
        if (await overLimit(s, "register", clientIp(req))) return json(429, { error: "slow down" });
        const r = await fn(s, await readBody(req));
        return json(r.status, r.body);
    } catch (e) {
        console.error("share directory:", e instanceof Error ? e.message : e);   // never the request body (secret)
        return json(503, { error: "unavailable" });
    }
};

export const handleRegister = post(register);
export const handleUnregister = post(unregister);

/** GET /api/resolve/<token>: always JSON with "online", so the page can tell it apart from a Hub. */
export async function handleResolve(/** @type {Request} */ req) {
    if (req.method !== "GET") return json(405, { online: false });
    const token = new URL(req.url).pathname.split("/").filter(Boolean).pop() || "";
    try {
        const s = store();
        if (await overLimit(s, "resolve", clientIp(req))) return json(429, { online: false, retry: true });
        const r = await resolve(s, token);
        return json(r.status, r.body);
    } catch (e) {
        console.error("share directory:", e instanceof Error ? e.message : e);
        return json(503, { online: false, retry: true });
    }
}
