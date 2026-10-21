// GET /api/v1/download/<version>: counts the download, then sends the browser on to the installer
// (/downloads/HEARASIDE-Setup-<version>.exe). Counting never stands in the way: if the counter is
// down the download still starts. Links that point at the file directly (GitHub, a copied URL) aren't counted.
import { Router } from 'express';
import type { Store } from '../lib/store.js';
import { clientIp, over } from '../lib/limit.js';
import release from '../../src/content/release.json' with { type: 'json' };

const known = () => new Set([release.version, ...release.notes.map(n => n.version)]);

export function downloadRoutes(store: Store) {
  const r = Router();
  r.get('/download/:version', async (req, res) => {
    const v = String(req.params.version);
    if (!known().has(v)) return res.status(404).json({ error: 'not_found' });
    try {
      // a browser asking for the file twice in a row, or a script hammering it, counts as one
      if (req.method === 'GET' && !(await over('download', `${clientIp(req)}:${v}`, 2))) await store.bumpDownload(v);
    } catch (e) {
      console.error('download count:', e instanceof Error ? e.message : e);
    }
    res.redirect(302, `/downloads/HEARASIDE-Setup-${v}.exe`);
  });
  return r;
}
