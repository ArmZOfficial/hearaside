// After `vite build` + `vite build --ssr`: writes the marketing pages as real HTML (SEO, fast first
// paint), in English and Thai. The browser then hydrates them. Account pages stay a plain SPA
// (dist/index.html with an empty #root).
import { mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const dist = join(root, 'dist');
const template = readFileSync(join(dist, 'index.html'), 'utf8');
const ssrEntry = readdirSync(join(root, 'dist-ssr')).find(f => /^entry-server\.(m?js)$/.test(f));
const { render, PRERENDER, guideSlugs } = await import(pathToFileURL(join(root, 'dist-ssr', ssrEntry)).href);

// preload the Anuphan files the first screen uses (Latin + Thai, 400/500/600)
const fonts = readdirSync(join(dist, 'assets'))
  .filter(f => /^anuphan-(latin|thai)-(400|500|600)-normal-.*\.woff2$/.test(f))
  .map(f => `<link rel="preload" href="/assets/${f}" as="font" type="font/woff2" crossorigin>`)
  .join('\n    ');

const pages = [...PRERENDER, ...guideSlugs.map(s => `/guides/${s}`)];
const urls = pages.flatMap(p => [p, p === '/' ? '/th' : `/th${p}`]);

const HEAD_TAGS = /<(title|meta|link)\b[^>]*?(?:\/>|>(?:[^<]*<\/title>)?)/g;

for (const url of urls) {
  let html = await render(url);
  // React 19 renders <title>/<meta>/<link> where the page put them: lift them into <head>
  const head = [];
  html = html.replace(HEAD_TAGS, tag => {
    if (/^<link\b/.test(tag) && !/rel="(canonical|alternate)"/.test(tag)) return tag;
    head.push(tag);
    return '';
  });
  const lang = url === '/th' || url.startsWith('/th/') ? 'th' : 'en';
  let page = template
    .replace('<html lang="en">', `<html lang="${lang}">`)
    .replace('<!--app-html-->', html);
  // the page's own title / description replace the defaults in index.html
  if (head.some(t => t.startsWith('<title'))) page = page.replace(/<title>[\s\S]*?<\/title>/, '');
  if (head.some(t => /name="description"/.test(t))) page = page.replace(/<meta name="description"[^>]*>/, '');
  if (head.some(t => /property="og:description"/.test(t))) page = page.replace(/<meta property="og:description"[^>]*>/, '');
  if (head.some(t => /property="og:title"/.test(t))) page = page.replace(/<meta property="og:title"[^>]*>/, '');
  page = page.replace('</head>', `    ${[...head, fonts].join('\n    ')}\n  </head>`);
  const out = url === '/' ? join(dist, 'index.html') : join(dist, url.slice(1), 'index.html');
  mkdirSync(dirname(out), { recursive: true });
  writeFileSync(out, page);
}
// the SPA shell for account pages (not prerendered): keep an untouched copy
writeFileSync(join(dist, 'app.html'), template.replace('<!--app-html-->', ''));
console.log(`prerendered ${urls.length} pages`);
