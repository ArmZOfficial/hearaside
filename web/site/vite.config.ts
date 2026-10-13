import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwind from '@tailwindcss/vite';
import mdx from '@mdx-js/rollup';

// Vite builds the static site (Vercel serves it from the CDN); the Express API is one Vercel
// Function (api/index.ts). In development /api is proxied to `tsx watch server/dev.ts` (:3001).
// which build this is: an open tab compares it with /version.json and reloads when a newer one is live
const BUILD_ID = process.env.VERCEL_DEPLOYMENT_ID || process.env.VERCEL_GIT_COMMIT_SHA?.slice(0, 12) || String(Date.now());
const versionFile = {
  name: 'version-file',
  apply: 'build' as const,
  generateBundle(this: { emitFile(f: object): void }, opts: { dir?: string }) {
    if (opts.dir?.endsWith('dist')) this.emitFile({ type: 'asset', fileName: 'version.json', source: JSON.stringify({ id: BUILD_ID }) });
  },
};

export default defineConfig({
  define: { __BUILD_ID__: JSON.stringify(BUILD_ID) },
  plugins: [versionFile, { enforce: 'pre', ...mdx({ providerImportSource: '@mdx-js/react' }) }, react({ include: /\.(mdx|tsx?)$/ }), tailwind()],
  server: {
    port: 5173,
    proxy: { '/api': 'http://127.0.0.1:3001' },
  },
  build: {
    target: 'es2022',
    sourcemap: false,
    chunkSizeWarningLimit: 600,
  },
  test: {
    // API tests run in Node; page tests say `// @vitest-environment jsdom` at the top
    environment: 'node',
    setupFiles: ['tests/setup.ts'],
    include: ['tests/server/**/*.test.ts', 'tests/web/**/*.test.{ts,tsx}'],
    // a developer's .env.local (real Turnstile key, Supabase URL) must not change what the tests see
    env: { STORE: 'memory', NODE_ENV: 'test', VITE_TURNSTILE_SITE_KEY: '' },
  },
} as any);
