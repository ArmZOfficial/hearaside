import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwind from '@tailwindcss/vite';
import mdx from '@mdx-js/rollup';

// Vite builds the static site (Vercel serves it from the CDN); the Express API is one Vercel
// Function (api/index.ts). In development /api is proxied to `tsx watch server/dev.ts` (:3001).
export default defineConfig({
  plugins: [{ enforce: 'pre', ...mdx({ providerImportSource: '@mdx-js/react' }) }, react({ include: /\.(mdx|tsx?)$/ }), tailwind()],
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
    env: { STORE: 'memory', NODE_ENV: 'test' },
  },
} as any);
