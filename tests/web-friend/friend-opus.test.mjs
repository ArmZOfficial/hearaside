// Wire test for HRA3 Opus negotiation and streaming
//   node tests/web-friend/friend-opus.test.mjs <ui-snapshot.exe>
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import net from 'node:net';

const exe = process.argv[2];
if (!exe) { console.error('usage: friend-opus.test.mjs <ui-snapshot.exe>'); process.exit(2); }

let failures = 0;
const expect = (what, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${extra ? ' ' + extra : ''}`); if (!ok) failures++; };
const sleep = ms => new Promise(r => setTimeout(r, ms));

const hub = spawn(exe, ['--serve-friend', '14'], { env: { ...process.env, HEARASIDE_NO_TUNNEL: '1' } });
let out = '';
hub.stdout.on('data', d => { out += d; });
const waitFor = async (re, ms) => {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    const m = re.exec(out);
    if (m) return m;
    await sleep(50);
  }
  return null;
};

const urlMatch = await waitFor(/FRIEND_URL=(\S+)/, 20000);
if (!urlMatch) { console.error('the Hub did not print a friend link:\n' + out); hub.kill(); process.exit(1); }
const u = new URL(urlMatch[1]);

const sock = net.connect({ host: u.hostname, port: Number(u.port) });
sock.write(`GET /ws/s/${u.pathname.split('/').pop()} HTTP/1.1\r\nHost: ${u.host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`);

let buf = Buffer.alloc(0), upgraded = false;
let hello = null, opusPackets = 0, badFrames = 0, lastSeq = -1, seqOk = true;
let pings = 0;

const maskedFrame = (op, payload) => {
  const m = crypto.randomBytes(4), body = Buffer.from(payload);
  for (let i = 0; i < body.length; i++) body[i] ^= m[i & 3];
  const n = body.length;
  return Buffer.concat([n < 126 ? Buffer.from([0x80 | op, 0x80 | n]) : Buffer.from([0x80 | op, 0x80 | 126, n >> 8, n & 255]), m, body]);
};

sock.on('data', d => {
  buf = Buffer.concat([buf, d]);
  if (!upgraded) {
    const i = buf.indexOf('\r\n\r\n');
    if (i < 0) return;
    buf = buf.subarray(i + 4);
    upgraded = true;
  }
  for (;;) {
    if (buf.length < 2) return;
    const op = buf[0] & 15;
    let len = buf[1] & 127, off = 2;
    if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
    else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
    if (buf.length < off + len) return;
    const p = buf.subarray(off, off + len);
    buf = buf.subarray(off + len);

    if (op === 1) {   // text
      try {
        const j = JSON.parse(p.toString());
        if (j.v >= 2) {
          hello = j;
          // negotiate Opus!
          if (j.codecs && j.codecs.includes('opus')) {
            sock.write(maskedFrame(1, JSON.stringify({ codec: 'opus' })));
          }
        }
        if (j.ping !== undefined) {
          pings++;
          sock.write(maskedFrame(1, JSON.stringify({ pong: j.ping })));
        }
      } catch { badFrames++; }
    } else if (op === 2) {   // binary
      if (p.length >= 22 && p.toString('latin1', 0, 4) === 'HRA3') {
        const codec = p.readUInt8(4);
        const chans = p.readUInt8(5);
        const flags = p.readUInt16LE(6);
        const seq = p.readUInt32LE(8);
        const pos = Number(p.readBigUInt64LE(12));
        const frames = p.readUInt16LE(20);
        const payloadLen = p.length - 22;

        if (codec === 1 && chans === 2 && frames === 240 && payloadLen > 0) {   // 5 ms Opus packets
          opusPackets++;
          if (lastSeq !== -1 && seq !== lastSeq + 1) {
            seqOk = false;
          }
          lastSeq = seq;
        } else {
          badFrames++;
        }
      } else if (p.length >= 24 && p.toString('latin1', 0, 4) === 'HRA2') {
        // initial packet before codec switch
      } else {
        badFrames++;
      }
    }
  }
});

sock.on('error', () => {});

let sent = 0;
const mic = setInterval(() => {
  if (!upgraded) return;
  // Send 10ms mic frames
  const b = Buffer.alloc(16 + 480 * 2);
  b.write('HRA1', 0, 'latin1');
  b.writeUInt32LE(48000, 4);
  b.writeUInt16LE(1, 8);
  b.writeUInt32LE(480, 12);
  for (let i = 0; i < 480; i++) b.writeInt16LE(Math.round(8000 * Math.sin((sent + i) * 0.058)), 16 + i * 2);
  sent += 480;
  sock.write(maskedFrame(2, b));
}, 10);

await sleep(4000);
expect('the Hub says hello v3 with Opus support', hello && hello.v === 3 && Array.isArray(hello.codecs) && hello.codecs.includes('opus'), `(${JSON.stringify(hello)})`);
expect('the mix comes back as HRA3 Opus stereo frames', opusPackets > 50 && badFrames === 0, `(${opusPackets} Opus packets, bad: ${badFrames})`);
expect('Opus sequence numbers increment monotonically without gaps', seqOk && lastSeq > 20, `(lastSeq: ${lastSeq})`);

clearInterval(mic);
sock.destroy();
hub.kill();

console.log(failures === 0 ? 'friend opus: all passed' : 'friend opus: FAILED');
process.exit(failures === 0 ? 0 : 1);
