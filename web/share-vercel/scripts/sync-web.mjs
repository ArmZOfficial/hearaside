// One source for what the share site and the account website both serve:
//   plugins/hub/web/{listen,send}.html  -> web/share-vercel/public/, web/site/public/  (the Hub embeds them too)
//   web/share-vercel/lib/{directory,redis}.js -> web/site/server/share/  (the account website hosts the directory)
//   node scripts/sync-web.mjs           copy
//   node scripts/sync-web.mjs --check   exit 1 if a copy is out of date (CTest, Vercel build)
import { copyFileSync, existsSync, mkdirSync, readFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, "..", "..", "..");
const pages = join(repo, "plugins", "hub", "web"), lib = join(here, "..", "lib");
const copies = [
    ...["listen.html", "send.html"].flatMap(p => [
        [join(pages, p), join(here, "..", "public", p)],
        [join(pages, p), join(repo, "web", "site", "public", p)],
    ]),
    ...["directory.js", "redis.js"].map(f => [join(lib, f), join(repo, "web", "site", "server", "share", f)]),
];
const check = process.argv.includes("--check");

let stale = 0;
for (const [from, to] of copies) {
    if (!existsSync(from)) {
        // a deployment that only uploaded one folder: what was committed there is what it serves
        if (check && existsSync(to)) continue;
        if (!existsSync(dirname(dirname(to)))) continue;   // that project isn't here at all
        console.error(`missing ${from}`);
        process.exit(1);
    }
    if (!existsSync(dirname(dirname(to)))) continue;
    const same = existsSync(to) && readFileSync(from).equals(readFileSync(to));
    if (same) continue;
    if (check) { console.error(`${relative(repo, to)} differs from ${relative(repo, from)} - run npm run sync in web/share-vercel`); stale++; }
    else { mkdirSync(dirname(to), { recursive: true }); copyFileSync(from, to); console.log(`copied ${relative(repo, to)}`); }
}
if (check && stale) process.exit(1);
if (check) console.log("every copy matches its source");
