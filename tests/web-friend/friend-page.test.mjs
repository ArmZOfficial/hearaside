// The friend's page in a real Chrome (fake microphone) against a real Hub: the page loads, the
// microphone reaches the Hub, the Hub's mix comes back, and the Hub measures the delay from the
// echo the page stamps on every packet.
//   node tests/web-friend/friend-page.test.mjs <ui-snapshot.exe>
// Needs Google Chrome (Playwright's "chrome" channel) and web/site's node_modules (@playwright/test).
// Skips (exit 0, says so) when either is missing.
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const exe = process.argv[2];
const require = createRequire(join(here, '..', '..', 'web', 'site', 'package.json'));
let chromium;
try { ({ chromium } = require('@playwright/test')); } catch { console.log('SKIP: @playwright/test is not installed (npm install in web/site)'); process.exit(0); }
if (!exe || !existsSync(exe)) { console.error('usage: friend-page.test.mjs <ui-snapshot.exe>'); process.exit(2); }

let failures = 0;
const expect = (what, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${extra ? ' ' + extra : ''}`); if (!ok) failures++; };

const hub = spawn(exe, ['--serve-friend', '14'], { env: { ...process.env, HEARASIDE_NO_TUNNEL: '1' } });
let out = '';
hub.stdout.on('data', d => { out += d; });
const waitFor = async (re, ms) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { const m = re.exec(out); if (m) return m; await new Promise(r => setTimeout(r, 100)); } return null; };
const url = (await waitFor(/FRIEND_URL=(\S+)/, 20000))?.[1];
if (!url) { console.error('the Hub did not print a friend link:\n' + out); hub.kill(); process.exit(1); }

let browser;
try {
  browser = await chromium.launch({ channel: 'chrome', args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required'] });
} catch (e) { console.log('SKIP: Google Chrome could not start (' + String(e.message).split('\n')[0] + ')'); hub.kill(); process.exit(0); }
const ctx = await browser.newContext({ permissions: ['microphone'] });
const page = await ctx.newPage();
const errors = [];
page.on('pageerror', e => errors.push(String(e)));
await page.goto(url);
expect('the page opens', (await page.title()).includes('HEARASIDE'));
await page.setViewportSize({ width: 420, height: 900 });
await page.click('#mic');
await page.waitForFunction(() => document.getElementById('status').textContent.length > 0 && document.getElementById('app').classList.contains('live'), null, { timeout: 15000 }).catch(() => {});
expect('connected and sending', await page.evaluate(() => document.getElementById('app').classList.contains('live')));
await page.waitForFunction(() => /Mint/.test(document.getElementById('title').textContent), null, { timeout: 5000 }).catch(() => {});
const title = await page.evaluate(() => document.getElementById('title').textContent);
expect('the Hub says who they are (the title names Mint)', /Mint/.test(title), `(title: "${title}")`);
expect('the "hear the music" controls appear', await page.evaluate(() => document.getElementById('hearPanel').style.display !== 'none'));
await page.waitForTimeout(6000);
const behind = await page.evaluate(() => document.getElementById('delay').textContent);
console.log('     the page says: "' + behind + '"');
expect('the page shows how late the voice is (measured by the Hub)', /\d+ ms/.test(behind));
await page.screenshot({ path: join(here, 'friend-page.png') });
await ctx.close();
await browser.close();

const result = await waitFor(/RESULT (.*)/, 20000);
hub.kill();
console.log('     hub: ' + (result ? result[1] : 'no result'));
const num = k => parseFloat(new RegExp(k + '=(-?[0-9.]+)').exec(result?.[1] ?? '')?.[1] ?? 'NaN');
expect('the Hub saw the friend live', num('live') === 1);
expect('the microphone reached the Hub (the fake mic beeps)', num('peak') > 0.01);
expect('audio was written to the friend ring', num('writePos') > 48000);
const d = num('delay');
expect('the Hub measured a plausible delay from the echo (5 .. 600 ms)', d > 5 && d < 600, `(${d} ms)`);
expect('no errors in the page', errors.length === 0, errors.join(' | '));
console.log(failures === 0 ? 'friend page: all passed' : 'friend page: FAILED');
process.exit(failures === 0 ? 0 : 1);
