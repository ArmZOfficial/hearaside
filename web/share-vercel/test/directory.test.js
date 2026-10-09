// node --test test/   (docs/ux-roadmap.md 8.9)
import { test } from "node:test";
import assert from "node:assert/strict";
import { ENTRY_TTL_SEC, LIMITS, memoryStore, overLimit, register, resolve, tunnelAllowed, unregister } from "../lib/directory.js";
import { handleRegister, handleResolve } from "../lib/handlers.js";

const secret = "a".repeat(64), other = "b".repeat(64);
const L = "listen0123456789abcdefgh", S = "send0123456789abcdefghij";
const tunnel = "https://able-dog-cat.trycloudflare.com";
const body = (extra = {}) => ({ tunnel, secret, tokens: { l: L, s: S }, ...extra });

test("register, then resolve gives the WebSocket address on the tunnel", async () => {
    const s = memoryStore();
    assert.equal((await register(s, body())).status, 200);
    assert.deepEqual((await resolve(s, L)).body, { online: true, ws: "wss://able-dog-cat.trycloudflare.com/ws/l/" + L });
    assert.deepEqual((await resolve(s, S)).body, { online: true, ws: "wss://able-dog-cat.trycloudflare.com/ws/s/" + S });
});

test("a new tunnel after restarting the DAW replaces the old one (same link)", async () => {
    const s = memoryStore();
    await register(s, body());
    await register(s, body({ tunnel: "https://new-name-here.trycloudflare.com" }));
    assert.equal((await resolve(s, L)).body.ws, "wss://new-name-here.trycloudflare.com/ws/l/" + L);
});

test("someone else's secret cannot take a link over -> 403", async () => {
    const s = memoryStore();
    await register(s, body());
    const r = await register(s, body({ secret: other, tunnel: "https://evil-site.trycloudflare.com" }));
    assert.equal(r.status, 403);
    assert.equal((await resolve(s, L)).body.ws, "wss://able-dog-cat.trycloudflare.com/ws/l/" + L);
    assert.equal((await unregister(s, { secret: other, tokens: { l: L } })).status, 403);
});

test("only cloudflared quick tunnels (or listed hosts) are accepted -> 400", async () => {
    const s = memoryStore();
    for (const bad of ["https://example.com", "http://x.trycloudflare.com", "https://x.trycloudflare.com/path",
                       "https://x.trycloudflare.com.evil.com", "javascript:alert(1)", 42])
        assert.equal((await register(s, body({ tunnel: bad }))).status, 400, String(bad));
    assert.equal(tunnelAllowed("https://live.example.com", "live.example.com"), true);
    assert.equal(tunnelAllowed("https://evil.example.com", "live.example.com"), false);
});

test("malformed requests -> 400", async () => {
    const s = memoryStore();
    for (const b of [null, {}, body({ secret: "short" }), body({ tokens: {} }), body({ tokens: { l: "UPPER_case_not_ok_123" } }),
                     body({ tokens: { l: L, s: L } }), body({ tokens: { x: L } })])
        assert.equal((await register(s, b)).status, 400, JSON.stringify(b));
});

test("unknown token, expired entry and bad token all answer the same", async () => {
    const s = memoryStore();
    await register(s, body());
    const expired = s.map.get("hs:t:" + L);
    expired.exp = Date.now() - 1;   // TTL ran out: the Hub stopped heart-beating
    const answers = await Promise.all([resolve(s, L), resolve(s, "nobodyhasthistoken000000"), resolve(s, "../../etc"), resolve(s, "")]);
    for (const a of answers) assert.deepEqual(a, { status: 200, body: { online: false } });
});

test("unregister takes the link offline; the owner keeps the token", async () => {
    const s = memoryStore();
    await register(s, body());
    assert.equal((await unregister(s, { secret, tokens: { l: L, s: S } })).status, 200);
    assert.equal((await resolve(s, L)).body.online, false);
    assert.equal((await register(s, body({ secret: other }))).status, 403);
    assert.equal((await register(s, body())).status, 200);
});

test("entries live for ENTRY_TTL_SEC, a heartbeat renews them", async () => {
    const s = memoryStore();
    await register(s, body());
    const e = s.map.get("hs:t:" + L);
    assert.ok(e.exp - Date.now() <= ENTRY_TTL_SEC * 1000 && e.exp - Date.now() > (ENTRY_TTL_SEC - 5) * 1000);
});

test("rate limit per IP and minute", async () => {
    const s = memoryStore();
    const now = Date.UTC(2026, 9, 9, 12, 0, 0);
    for (let i = 0; i < LIMITS.register; i++) assert.equal(await overLimit(s, "register", "1.2.3.4", now), false);
    assert.equal(await overLimit(s, "register", "1.2.3.4", now), true);
    assert.equal(await overLimit(s, "register", "5.6.7.8", now), false);
    assert.equal(await overLimit(s, "register", "1.2.3.4", now + 60000), false);
});

test("HTTP handlers: JSON in and out, 429 when flooded, no secret in the answer", async () => {
    const req = b => new Request("https://hearaside.example/api/register", {
        method: "POST", body: JSON.stringify(b), headers: { "x-forwarded-for": "9.9.9.9", "content-type": "application/json" } });
    const ok = await handleRegister(req(body()));
    assert.equal(ok.status, 200);
    assert.ok(!(await ok.text()).includes(secret));
    const r = await handleResolve(new Request("https://hearaside.example/api/resolve/" + L, { headers: { "x-forwarded-for": "9.9.9.9" } }));
    assert.equal((await r.json()).online, true);
    assert.equal((await handleRegister(new Request("https://hearaside.example/api/register"))).status, 405);
    let last;
    for (let i = 0; i <= LIMITS.register; i++) last = await handleRegister(req(body()));
    assert.equal(last.status, 429);
});
