// cargo build -p hs-net --example rtc-peer, then node tests/web-friend/rtc.test.mjs rust/target/debug/examples/rtc-peer.exe
// Exercises the unchanged pages against the Rust peer; the HTTP/WebSocket fixture is not ShareServer (R3.3).
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { dirname, join, resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';
import { setTimeout as sleep } from 'node:timers/promises';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const require = createRequire(join(root, 'web/site/package.json'));
const { chromium } = require('@playwright/test');
// Reuse Playwright's bundled WebSocket server instead of adding a test dependency.
const { wsServer: WebSocketServer } = require(join(dirname(require.resolve('playwright-core/package.json')), 'lib/utilsBundle.js'));
const exe = process.argv[2];
assert.ok(exe, 'usage: rtc.test.mjs <rtc-peer executable>');

async function until(check, description, ms = 15000) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    if (await check()) return;
    await sleep(25);
  }
  throw new Error(`timed out: ${description}`);
}

const golden = await readFile(join(root, 'compat/golden/net/opus_audio_stereo.pkt'));
const packets = [];
for (let i = 0; i < golden.length;) {
  const n = golden.readUInt16LE(i);
  assert.ok(n > 0 && i + 2 + n <= golden.length, 'valid C++ golden packet');
  packets.push(golden.subarray(i + 2, i + 2 + n));
  i += n + 2;
}
function hra3(seq) {
  const payload = packets[seq % packets.length];
  const frame = Buffer.alloc(22 + payload.length);
  frame.write('HRA3');
  frame[4] = 1;
  frame[5] = 2;
  frame.writeUInt32LE(seq, 8);
  frame.writeBigUInt64LE(BigInt(seq * 960), 12);
  frame.writeUInt16LE(960, 20);
  payload.copy(frame, 22);
  return [...frame];
}

const browser = await chromium.launch({
  channel: 'chrome',
  args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required'],
});
try {
  for (const kind of ['listen', 'send']) {
    const child = spawn(resolve(exe), [], { stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
    const events = [], signaling = [], uplink = [], pageErrors = [], warnings = [];
    let diagnostic = '', childError;
    child.stderr.on('data', d => { diagnostic += d; });
    child.on('error', e => { childError = e; });
    const command = message => {
      assert.equal(childError, undefined, String(childError));
      assert.equal(child.exitCode, null, `Rust helper exited: ${diagnostic}`);
      child.stdin.write(JSON.stringify(message) + '\n');
    };
    child.stdin.on('error', e => { childError = e; });
    const lines = createInterface({ input: child.stdout });
    const server = createServer(async (req, res) => {
      if (req.url === '/api/turn') {
        res.writeHead(200, { 'content-type': 'application/json' });
        res.end(JSON.stringify({ iceServers: [{ urls: 'stun:127.0.0.1:9' }] }));
      } else if (req.url === `/${kind}/test`) {
        res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
        res.end(await readFile(join(root, `plugins/hub/web/${kind}.html`)));
      } else {
        res.writeHead(404).end();
      }
    });
    const sockets = new WebSocketServer({ server });
    let socket;
    lines.on('line', line => {
      try {
        const event = JSON.parse(line);
        events.push(event);
        if (event.type === 'rtc_offer' || event.type === 'rtc_candidate') {
          signaling.push(event);
          socket?.send(JSON.stringify(event));
        }
        if (event.type === 'message') uplink.push(Buffer.from(event.data));
      } catch (e) { childError = e; }
    });
    sockets.on('connection', incoming => {
      socket = incoming;
      socket.on('message', (data, binary) => {
        if (binary) return; // A microphone arriving via WebSocket cannot satisfy the RTC uplink check.
        const message = JSON.parse(data.toString());
        if (message.type === 'rtc_answer' || message.type === 'rtc_candidate') command(message);
      });
      socket.send(JSON.stringify({ v: 3, name: 'Rust R3.2', host: 'ArmZ', codecs: ['opus', 'pcm16'], webrtc: true }));
      command({ type: 'offer', label: kind === 'send' ? 'friend' : 'audio' });
    });
    const context = await browser.newContext({ permissions: ['microphone'] });
    try {
      await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
      await until(() => {
        if (childError) throw childError;
        assert.equal(child.exitCode, null, diagnostic);
        return events.some(e => e.type === 'ready');
      }, 'Rust peer ready');
      const ready = events.find(e => e.type === 'ready');
      assert.ok(ready.port >= 47830 && ready.port <= 47999, `Hub UDP port: ${ready.port}`);
      await context.addInitScript(() => {
        window.rtcTest = { decoded: 0, received: [], channels: [] };
        const NativePeer = window.RTCPeerConnection;
        window.RTCPeerConnection = class extends NativePeer {
          constructor(...args) {
            super(...args);
            this.addEventListener('datachannel', ({ channel }) => {
              window.rtcTest.channels.push({ label: channel.label, ordered: channel.ordered, maxRetransmits: channel.maxRetransmits });
              channel.addEventListener('message', e => {
                if (e.data instanceof ArrayBuffer) window.rtcTest.received.push([...new Uint8Array(e.data)]);
              });
            });
          }
        };
        const NativeDecoder = window.AudioDecoder;
        window.AudioDecoder = class extends NativeDecoder {
          constructor(options) {
            super({ ...options, output: audio => { window.rtcTest.decoded++; options.output(audio); } });
          }
        };
      });
      const page = await context.newPage();
      page.on('pageerror', e => pageErrors.push(String(e)));
      page.on('console', message => { if (message.type() === 'warning') warnings.push(message.text()); });
      await page.route('https://fonts.googleapis.com/**', route => route.abort());
      await page.route('https://fonts.gstatic.com/**', route => route.abort());
      await page.goto(`http://127.0.0.1:${server.address().port}/${kind}/test`);
      await page.click(kind === 'send' ? '#mic' : '#play');
      await page.waitForFunction(() => rtcActive && useOpus, null, { timeout: 15000 });
      await until(() => events.some(e => e.type === 'open'), 'Rust DataChannel open');
      const channels = await page.evaluate(() => window.rtcTest.channels);
      assert.deepEqual(channels, [{ label: kind === 'send' ? 'friend' : 'audio', ordered: false, maxRetransmits: 0 }]);
      assert.ok(signaling.some(e => e.type === 'rtc_offer'), 'original rtc_offer signaling');
      assert.ok(signaling.some(e => e.type === 'rtc_candidate'), 'original rtc_candidate signaling');
      for (let seq = 0; seq < 35; seq++) {
        command({ type: 'send', data: hra3(seq) });
        await sleep(20);
      }
      await page.waitForFunction(() => window.rtcTest.decoded >= 20, null, { timeout: 10000 });
      const received = await page.evaluate(() => window.rtcTest.received);
      assert.ok(received.length >= 20, 'HRA3 frames reached the original page');
      for (const frame of received) {
        const seq = Buffer.from(frame).readUInt32LE(8);
        assert.deepEqual(frame, hra3(seq), 'DataChannel preserves C++ HRA3/Opus bytes');
      }
      if (kind === 'send') {
        await until(() => uplink.length >= 10, 'microphone HRA3 over Rust DataChannel');
        for (const frame of uplink) {
          assert.equal(frame.toString('ascii', 0, 4), 'HRA3');
          assert.equal(frame[4], 1, 'microphone uses Opus');
          assert.ok(frame[5] === 1 || frame[5] === 2, 'valid microphone channels');
          assert.ok(frame.length > 22 && frame.readUInt16LE(20) > 0, 'nonempty microphone frame');
        }
      }
      command({ type: 'close' });
      await page.waitForFunction(() => !rtcActive, null, { timeout: 10000 });
      assert.deepEqual(pageErrors, []);
      assert.ok(!warnings.some(w => /RTC setup error|AudioDecoder error/.test(w)), warnings.join('\n'));
      assert.ok(!events.some(e => e.type === 'error'), JSON.stringify(events.filter(e => e.type === 'error')));
      console.log(`ok ${kind}.html: original signaling, UDP ${ready.port}, unordered/0 retries, ${received.length} HRA3 frames decoded${kind === 'send' ? `, ${uplink.length} microphone frames received by Rust` : ''}, close`);
    } finally {
      await context.close();
      for (const client of sockets.clients) client.terminate();
      await new Promise(resolve => sockets.close(resolve));
      await new Promise(resolve => server.close(resolve));
      lines.close();
      child.stdin.end();
      child.kill();
    }
  }
} finally {
  await browser.close();
}
