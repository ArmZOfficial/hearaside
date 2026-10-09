// The listen / send pages have one source: plugins/hub/web (the Hub embeds them too).
//   node scripts/sync-web.mjs           copy them into public/
//   node scripts/sync-web.mjs --check   exit 1 if public/ is out of date (CTest, Vercel build)
import { copyFileSync, existsSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const source = join(here, "..", "..", "..", "plugins", "hub", "web");
const target = join(here, "..", "public");
const pages = ["listen.html", "send.html"];
const check = process.argv.includes("--check");

let stale = 0;
for (const page of pages) {
    const from = join(source, page), to = join(target, page);
    if (!existsSync(from)) {
        // a deployment that only uploaded this folder: public/ is what was committed
        if (check && existsSync(to)) continue;
        console.error(`missing ${from}`);
        process.exit(1);
    }
    const same = existsSync(to) && readFileSync(from).equals(readFileSync(to));
    if (same) continue;
    if (check) { console.error(`public/${page} differs from plugins/hub/web/${page} - run npm run sync`); stale++; }
    else { copyFileSync(from, to); console.log(`copied ${page}`); }
}
if (check && stale) process.exit(1);
if (check) console.log("public/ matches plugins/hub/web");
