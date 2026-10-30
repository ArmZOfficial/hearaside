// Local API for development only: `npm run dev` (with Vite) or `npm run mock:plugin-api` (the
// plug-ins' mock server, A4 item 7). `--memory` keeps accounts in memory, no Supabase needed.
import { createApp } from './app.js';
import { MemoryStore } from './lib/memoryStore.js';

if (process.argv.includes('--memory')) process.env.STORE = 'memory';
const port = Number(process.env.API_PORT ?? 3001);   // not PORT: that one belongs to Vite
const demo = process.argv.includes('--demo') && process.env.STORE === 'memory' ? new MemoryStore() : undefined;
if (demo) {
  process.env.ADMIN_EMAILS = 'admin@example.test';
  demo.signUp('admin@example.test', 'HearasideLocal123!', 'Local Admin', 'th');
  demo.signUp('member@example.test', 'HearasideLocal123!', 'Local Member', 'th');
  console.log('Local test accounts: admin@example.test / member@example.test; password: HearasideLocal123!');
}
createApp(demo).listen(port, '127.0.0.1', () => {
  console.log(`HEARASIDE API on http://127.0.0.1:${port}/api/v1 (${process.env.STORE === 'memory' ? 'memory' : 'Supabase'})`);
});
