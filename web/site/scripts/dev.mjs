// `npm run dev`: the API (tsx watch, :3001) and Vite (:5173, proxies /api) together.
// Without Supabase settings in .env.local, both run in memory mode so every page works offline.
import { spawn } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';

const env = { ...process.env };
if (existsSync('.env.local')) {
  for (const line of readFileSync('.env.local', 'utf8').split(/\r?\n/)) {
    const m = /^\s*([A-Z0-9_]+)\s*=\s*(.*)\s*$/.exec(line);
    if (m && env[m[1]] === undefined) env[m[1]] = m[2];
  }
}
if (!env.SUPABASE_SECRET_KEY) {
  env.STORE = 'memory';
  env.VITE_AUTH_MODE = 'memory';
  console.log('dev: no SUPABASE_SECRET_KEY, accounts are kept in memory (VITE_AUTH_MODE=memory)');
}

// the tools' own entry files through this Node (no shell, works the same on Windows)
const run = args => spawn(process.execPath, args, { stdio: 'inherit', env });
const api = run(['node_modules/tsx/dist/cli.mjs', 'watch', 'server/dev.ts']);
const web = run(['node_modules/vite/bin/vite.js', '--port', env.PORT ?? '5173', '--strictPort']);
const stop = () => { api.kill(); web.kill(); process.exit(); };
process.on('SIGINT', stop);
process.on('SIGTERM', stop);
api.on('exit', code => code && stop());
web.on('exit', stop);
