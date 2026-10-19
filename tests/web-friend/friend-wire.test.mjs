// The friend's connection to a real Hub, without a browser (a plain TCP client speaks the WebSocket):
// hello, ping / pong, the mix coming back, and a friend that stops reading for a while must not stop
// the microphone (the Hub reads and writes on two threads).
//   node tests/web-friend/friend-wire.test.mjs <ui-snapshot.exe>
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import net from 'node:net';
import crypto from 'node:crypto';

const exe = process.argv[2];
if (!exe || !existsSync(exe)) { console.error('usage: friend-wire.test.mjs <ui-snapshot.exe>'); process.exit(2); }
let failures = 0;
const expect = (what, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${extra ? ' ' + extra : ''}`); if (!ok) failures++; };
const sleep = ms => new Promise(r => setTimeout(r, ms));

const hub = spawn(exe, ['--serve-friend', '20'], { env: { ...process.env, HEARASIDE_NO_TUNNEL: '1' } });
let out = '';
hub.stdout.on('data', d => { out += d; });
const waitFor = async (re, ms) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { const m = re.exec(out); if (m) return m; await sleep(100); } return null; };
const url = (await waitFor(/FRIEND_URL=(\S+)/, 20000))?.[1];
if (!url) { console.error('the Hub did not print a friend link:\n' + out); hub.kill(); process.exit(1); }

const u = new URL(url);
const sock = net.connect(Number(u.port), u.hostname);
sock.write(`GET /ws/s/${u.pathname.split('/').pop()} HTTP/1.1\r\nHost: ${u.host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
let buf = Buffer.alloc(0), upgraded = false, hello = null, audio = 0, pings = 0, badFrames = 0;
const maskedFrame = (op, payload) => {   // a browser masks what it sends
  const m = crypto.randomBytes(4), body = Buffer.from(payload);
  for (let i = 0; i < body.length; i++) body[i] ^= m[i & 3];
  const n = body.length;
  return Buffer.concat([n < 126 ? Buffer.from([0x80 | op, 0x80 | n]) : Buffer.from([0x80 | op, 0x80 | 126, n >> 8, n & 255]), m, body]);
};
sock.on('data', d => {
  buf = Buffer.concat([buf, d]);
  if (!upgraded) { const i = buf.indexOf('\r\n\r\n'); if (i < 0) return; buf = buf.subarray(i + 4); upgraded = true; }
  for (;;) {
    if (buf.length < 2) return;
    const op = buf[0] & 15;
    let len = buf[1] & 127, off = 2;
    if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
    else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
    if (buf.length < off + len) return;
    const p = buf.subarray(off, off + len);
    buf = buf.subarray(off + len);
    if (op === 2) { if (p.length >= 24 && p.toString('latin1', 0, 4) === 'HRA2' && p.readUInt16LE(8) === 2) audio++; else badFrames++; }
    if (op === 1) {
      try {
        const j = JSON.parse(p.toString());
        if (j.v === 2) hello = j;
        if (j.ping !== undefined) { pings++; sock.write(maskedFrame(1, JSON.stringify({ pong: j.ping }))); }
      } catch { badFrames++; }
    }
  }
});
sock.on('error', () => {});

let sent = 0;
const mic = setInterval(() => {   // mono, 10 ms, like the page does now
  if (!upgraded) return;
  const b = Buffer.alloc(16 + 480 * 2);
  b.write('HRA1', 0, 'latin1'); b.writeUInt32LE(48000, 4); b.writeUInt16LE(1, 8); b.writeUInt32LE(480, 12);
  for (let i = 0; i < 480; i++) b.writeInt16LE(Math.round(8000 * Math.sin((sent + i) * 0.058)), 16 + i * 2);
  sent += 480;
  sock.write(maskedFrame(2, b));
}, 10);

await sleep(4000);
expect('the Hub says hello: who the friend is', hello && hello.name === 'Mint', `(${JSON.stringify(hello)})`);
expect('the mix comes back as HRA2 stereo', audio > 100 && badFrames === 0, `(${audio} packets)`);
expect('the Hub pings and the answer is taken (see rtt below)', pings >= 2, `(${pings} pings)`);
const before = audio;
sock.pause();   // the friend's line down is stuck for 6 s; the microphone goes on
await sleep(6000);
sock.resume();
await sleep(2500);
expect('after the stop the mix flows again', audio > before + 50, `(${audio - before} more packets)`);
clearInterval(mic);
sock.destroy();

const result = await waitFor(/RESULT (.*)/, 20000);
hub.kill();
console.log('     hub: ' + (result ? result[1] : 'no result'));
const num = k => parseFloat(new RegExp(k + '=(-?[0-9.]+)').exec(result?.[1] ?? '')?.[1] ?? 'NaN');
expect('the microphone kept arriving while the friend did not read (12.5 s of audio, at least 11)', num('writePos') > 48000 * 11, `(${(num('writePos') / 48000).toFixed(1)} s)`);
expect('no gap of more than half a second between microphone packets', num('micGap') < 500, `(${num('micGap')} ms)`);
expect('the round trip was measured from the pong', num('rtt') >= 0 && num('rtt') < 1000, `(${num('rtt')} ms)`);
expect('the line up is mono: 600 .. 1000 kbps', num('up') > 600 && num('up') < 1000, `(${num('up')} kbps)`);
console.log(failures === 0 ? 'friend wire: all passed' : 'friend wire: FAILED');
process.exit(failures === 0 ? 0 : 1);
