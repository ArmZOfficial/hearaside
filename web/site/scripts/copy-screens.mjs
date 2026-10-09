// Copies plug-in screenshots made by `ui-snapshot --audit` into public/screens as WebP, so the
// website always shows the real plug-ins (prompt W2: never redrawn by hand).
//   node scripts/copy-screens.mjs [snapshot folder]     default: ../../ui-snapshots/redesign
import { existsSync, mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import sharp from 'sharp';

const here = dirname(fileURLToPath(import.meta.url));
const src = resolve(process.argv[2] ?? join(here, '..', '..', '..', 'ui-snapshots', 'redesign'));
const out = join(here, '..', 'public', 'screens');

// website name <- ui-snapshot name
const SCREENS = { hub: 'hub-default', share: 'page-share', track: 'track-default' };

if (!existsSync(src)) {
  console.error(`No snapshots in ${src}. Run: build\\tools\\ui-snapshot\\...\\ui-snapshot.exe --audit ui-snapshots/redesign`);
  process.exit(1);
}
mkdirSync(out, { recursive: true });

let n = 0;
for (const [name, snap] of Object.entries(SCREENS)) {
  for (const lang of ['en', 'th']) {
    for (const theme of ['light', 'dark']) {
      const file = join(src, `${snap}-${lang}-${theme}.png`);
      if (!existsSync(file)) { console.warn(`missing ${file}`); continue; }
      await sharp(file).webp({ quality: 86, effort: 5 }).toFile(join(out, `${name}-${lang}-${theme}.webp`));
      n++;
    }
  }
}
// the Open Graph picture: the Hub, English, light
const og = join(src, 'hub-default-en-light.png');
if (existsSync(og)) {
  await sharp(og).resize(1200, 630, { fit: 'cover', position: 'top' }).png().toFile(join(out, 'og.png'));
  n++;
}
console.log(`screens: ${n} files -> ${out}`);
