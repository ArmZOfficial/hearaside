// Local stand-in for Vercel: the same API handlers, rewrites and headers (vercel.json), an
// in-memory store unless UPSTASH_* / KV_* is set.  node scripts/dev-server.mjs [port]
// Point a Hub at it with Settings > "Share web address" = http://127.0.0.1:<port>.
import { createServer } from "node:http";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { handleRegister, handleResolve, handleUnregister } from "../lib/handlers.js";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const config = JSON.parse(readFileSync(join(root, "vercel.json"), "utf8"));
const port = Number(process.argv[2] || process.env.PORT || 3000);

/** headers from vercel.json whose source pattern matches the path */
function headersFor(path) {
    const out = {};
    for (const rule of config.headers) {
        const re = new RegExp("^" + rule.source.replace(/:\w+/g, "[^/]+") + "$");
        if (re.test(path)) for (const h of rule.headers) out[h.key] = h.value;
    }
    return out;
}

const server = createServer(async (req, res) => {
    const url = new URL(req.url, `http://${req.headers.host}`);
    let path = url.pathname;
    let handler = null;
    if (path === "/api/register") handler = handleRegister;
    else if (path === "/api/unregister") handler = handleUnregister;
    else if (/^\/api\/resolve\/[^/]+$/.test(path)) handler = handleResolve;
    if (handler) {
        const chunks = [];
        for await (const c of req) chunks.push(c);
        const body = req.method === "POST" ? Buffer.concat(chunks) : undefined;
        const headers = new Headers(Object.entries(req.headers).map(([k, v]) => [k, String(v)]));
        headers.set("x-forwarded-for", req.socket.remoteAddress || "local");
        const response = await handler(new Request(url, { method: req.method, headers, body }));
        res.writeHead(response.status, { ...headersFor(path), ...Object.fromEntries(response.headers) });
        res.end(Buffer.from(await response.arrayBuffer()));
        return;
    }
    for (const rw of config.rewrites) {
        const re = new RegExp("^" + rw.source.replace(/:\w+/g, "[^/]+") + "$");
        if (re.test(path)) { path = rw.destination; break; }
    }
    try {
        const file = readFileSync(join(root, "public", path.replace(/^\/+/, "")));
        res.writeHead(200, { ...headersFor(url.pathname), "Content-Type": path.endsWith(".html") ? "text/html; charset=utf-8" : "application/octet-stream" });
        res.end(file);
    } catch {
        res.writeHead(404, { "Content-Type": "text/plain" });
        res.end("not found");
    }
});
server.listen(port, "127.0.0.1", () => console.log(`HEARASIDE share (local) on http://127.0.0.1:${port}`));
