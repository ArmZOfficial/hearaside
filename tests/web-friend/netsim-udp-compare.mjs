// R3.7: real Chrome DataChannel over a seeded UDP relay. Signalling stays on TCP.
// Impairment applies to all UDP datagrams, including ICE/DTLS/SCTP control traffic.
// node tests/web-friend/netsim-udp-compare.mjs --cpp <pure-cpp.exe> [--rust <exe>] [--seconds 60] [--runs 2]
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import dgram from 'node:dgram';
import { existsSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { setTimeout as sleep } from 'node:timers/promises';
import { playout } from './netsim-compare.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const profiles = { clean: { delay: 5, jitter: 0, loss: 0 }, bad: { delay: 25, jitter: 30, loss: 0.03 } };

export function candidate(line, port, side = 'browser', proxyAddress = '127.0.0.1') {
  const parts = line.split(' ');
  if (parts.length < 8 || parts[2]?.toLowerCase() !== 'udp' || !/^\d+\.\d+\.\d+\.\d+$/.test(parts[4])) return null;
  if (parts[7] !== 'host' && !(side === 'hub' && parts[7] === 'srflx' && Number(parts[5]) >= 47830 && Number(parts[5]) <= 47999)) return null;
  const endpoint = { address: parts[7] === 'srflx' ? '127.0.0.1' : parts[4], port: Number(parts[5]) };
  if (!Number.isInteger(endpoint.port) || endpoint.port < 1 || endpoint.port > 65535) return null;
  parts[4] = proxyAddress; parts[5] = String(port);
  return { endpoint, line: parts.join(' ') };
}

export async function relay(profile, seed, proxyAddress = '127.0.0.1') {
  const socket = dgram.createSocket('udp4');
  await new Promise((resolve, reject) => { socket.once('error', reject); socket.bind(0, '0.0.0.0', resolve); });
  const port = socket.address().port, peers = {}, attempts = [], setupSources = [], pending = new Set();
  const counts = { received: 0, forwarded: 0, dropped: 0, reordered: 0, unknown: 0 };
  let enabled = false, srflxHub = false, state = seed >>> 0, serial = 0, last = { hub: 0, browser: 0 };
  const random = () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return state / 4294967296; };
  socket.on('message', (data, from) => {
    if (!enabled && setupSources.length < 20) setupSources.push({ address: from.address, port: from.port, header: data.subarray(0, 20).toString('hex') });
    const side = Object.keys(peers).find(k => peers[k].port === from.port);
    if (!side || !peers[side === 'hub' ? 'browser' : 'hub']) { counts.unknown++; return; }
    const target = peers[side === 'hub' ? 'browser' : 'hub'];
    const id = ++serial;
    const impaired = enabled;
    if (impaired) counts.received++;
    if (impaired && random() < profile.loss) { counts.dropped++; return; }
    const delay = impaired ? Math.max(0, profile.delay + (random() * 2 - 1) * profile.jitter) : 0;
    const timer = setTimeout(() => {
      pending.delete(timer);
      if (impaired) { counts.forwarded++; if (id < last[side]) counts.reordered++; last[side] = Math.max(last[side], id); }
      socket.send(data, target.port, target.address);
    }, delay);
    pending.add(timer);
  });
  return {
    port, counts, peers, attempts, setupSources,
    map(side, line) {
      attempts.push({ side, line });
      const parsed = candidate(line, port, side, proxyAddress);
      if (!parsed) return null;
      const existing = peers[side];
      if (existing && (existing.address !== parsed.endpoint.address || existing.port !== parsed.endpoint.port)) return null;
      if (Object.entries(peers).some(([key, peer]) => key !== side && peer.port === parsed.endpoint.port)) return null;
      if (side === 'browser' && srflxHub && peers.hub) peers.hub.address = parsed.endpoint.address;
      if (side === 'hub' && /\btyp srflx(?:\s|$)/.test(line)) { srflxHub = true; if (peers.browser) parsed.endpoint.address = peers.browser.address; }
      peers[side] = parsed.endpoint;
      return parsed.line;
    },
    enable() { enabled = true; },
    close() { for (const timer of pending) clearTimeout(timer); socket.close(); },
  };
}

async function run(browser, exe, profile, seed, seconds, label) {
  const route = dgram.createSocket('udp4');
  await new Promise((resolve, reject) => { route.once('error', reject); route.connect(9, '8.8.8.8', resolve); });
  const proxyAddress = route.address().address;
  route.close();
  const proxy = await relay(profile, seed, proxyAddress);
  const hub = spawn(exe, ['--serve-friend', String(seconds + 25)], { env: { ...process.env, HEARASIDE_NO_TUNNEL: '1', HEARASIDE_COMPARE_CLOCK: '1' }, windowsHide: true });
  let output = '', childError;
  hub.stdout.on('data', d => { output += d; }); hub.stderr.on('data', d => { output += d; }); hub.on('error', e => { childError = e; });
  const exited = new Promise(resolve => { hub.once('exit', resolve); hub.once('error', resolve); });
  const waitFor = async (regex, timeout) => {
    const until = Date.now() + timeout;
    while (Date.now() < until) { if (childError) throw childError; const m = regex.exec(output); if (m) return m; if (hub.exitCode !== null) throw new Error(`Hub exited: ${output}`); await sleep(50); }
    throw new Error(`timeout ${regex}: ${output}`);
  };
  let context;
  try {
    const url = (await waitFor(/FRIEND_URL=(\S+)/, 25000))[1];
    context = await browser.newContext({ permissions: ['microphone'] });
    await context.exposeBinding('mapCandidate', (_, side, line) => proxy.map(side, line));
    await context.addInitScript(() => {
      window.capture = { packets: [], channels: [], errors: [], fallback: 0, disconnects: 0, decoded: 0, invalid: 0, measuring: false };
      const NativeSocket = WebSocket;
      window.WebSocket = class extends NativeSocket {
        constructor(...args) { super(...args); this.addEventListener('message', e => { if (window.capture.measuring && typeof e.data !== 'string') window.capture.fallback++; }); }
      };
      const Decoder = AudioDecoder;
      window.AudioDecoder = class extends Decoder {
        constructor(options) { super({ ...options, output: audio => { if (window.capture.measuring) window.capture.decoded++; options.output(audio); } }); }
      };
      const Native = RTCPeerConnection;
      window.RTCPeerConnection = class extends Native {
        constructor(config) {
          super({ ...config, iceServers: [] }); window.capture.peer = this;
          this.addEventListener('connectionstatechange', () => { if (window.capture.measuring && this.connectionState !== 'connected') window.capture.disconnects++; });
          this.addEventListener('datachannel', ({ channel }) => {
            window.capture.channels.push({ label: channel.label, ordered: channel.ordered, maxRetransmits: channel.maxRetransmits });
            channel.addEventListener('message', e => {
              if (!(e.data instanceof ArrayBuffer)) return;
              const v = new DataView(e.data);
              if (v.byteLength > 22 && v.getUint32(0) === 0x48524133 && v.getUint8(4) === 1) window.capture.packets.push({ t: performance.now(), pos: Number(v.getBigUint64(12, true)), frames: v.getUint16(20, true) });
              else if (window.capture.measuring) window.capture.invalid++;
            });
          });
        }
        setRemoteDescription(description) {
          this.remoteReady = (async () => {
            const lines = await Promise.all(description.sdp.split('\n').map(async line => {
              if (!line.startsWith('a=candidate:')) return line;
              const mapped = await window.mapCandidate('hub', line.slice(2).trim());
              return mapped ? 'a=' + mapped + '\r' : '';
            }));
            return super.setRemoteDescription({ type: description.type, sdp: lines.filter(Boolean).join('\n') + '\n' });
          })();
          return this.remoteReady;
        }
        async addIceCandidate(c) {
          await this.remoteReady;
          const mapped = await window.mapCandidate('hub', c.candidate);
          if (mapped) return super.addIceCandidate({ ...c, candidate: mapped });
        }
      };
      const send = WebSocket.prototype.send;
      WebSocket.prototype.send = function(data) {
        if (typeof data !== 'string') { if (window.capture.measuring) window.capture.fallback++; return send.call(this, data); }
        let message; try { message = JSON.parse(data); } catch { return send.call(this, data); }
        if (message.type === 'rtc_candidate') {
          window.mapCandidate('browser', message.candidate).then(mapped => { if (mapped) { message.candidate = mapped; send.call(this, JSON.stringify(message)); } }).catch(e => window.capture.errors.push(String(e)));
          return;
        }
        if (message.type === 'rtc_answer') message.sdp = message.sdp.split('\n').filter(line => !line.startsWith('a=candidate:')).join('\n');
        return send.call(this, JSON.stringify(message));
      };
    });
    const page = await context.newPage();
    const errors = [], warnings = []; page.on('pageerror', e => errors.push(String(e)));
    page.on('console', m => { if (m.type() === 'warning' || m.type() === 'error') warnings.push(m.text()); });
    await page.route('https://fonts.googleapis.com/**', r => r.abort());
    await page.route('https://fonts.gstatic.com/**', r => r.abort());
    await page.goto(url); await page.click('#mic');
    try { await page.waitForFunction(() => rtcActive && useOpus && window.capture.packets.length >= 10, null, { timeout: 18000 }); }
    catch (e) {
      const state = await page.evaluate(() => ({ active: rtcActive, opus: useOpus, pc: window.capture.peer?.connectionState, ice: window.capture.peer?.iceConnectionState, packets: window.capture.packets.length, errors: window.capture.errors }));
      throw new Error(`${e.message}: ${JSON.stringify({ state, peers: proxy.peers, attempts: proxy.attempts, counts: proxy.counts, setupSources: proxy.setupSources, warnings, output })}`);
    }
    const selected = await page.evaluate(async () => {
      const stats = await window.capture.peer.getStats();
      const pair = [...stats.values()].find(s => s.type === 'candidate-pair' && s.nominated && s.state === 'succeeded');
      return pair && { pair, remote: stats.get(pair.remoteCandidateId), local: stats.get(pair.localCandidateId) };
    });
    assert.equal(selected?.remote?.port, proxy.port, 'selected UDP candidate must be the proxy');
    assert.equal(selected?.remote?.protocol, 'udp');
    assert.ok(proxy.peers.hub && proxy.peers.browser, 'both actual ICE endpoints known');
    proxy.enable();
    const start = await page.evaluate(() => { window.capture.packets = []; window.capture.measuring = true; return performance.now(); });
    await sleep(seconds * 1000);
    const data = await page.evaluate(() => ({ end: performance.now(), active: rtcActive, ...window.capture, peer: undefined }));
    const counts = { ...proxy.counts };
    assert.ok(data.active, 'WebRTC must remain active; WebSocket fallback is invalid');
    assert.deepEqual(data.channels, [{ label: 'audio', ordered: false, maxRetransmits: 0 }]);
    assert.deepEqual([...errors, ...data.errors], []);
    assert.equal(data.fallback, 0, 'no WebSocket audio during measurement');
    assert.equal(data.disconnects, 0, 'WebRTC must stay connected throughout measurement');
    assert.equal(data.invalid, 0, 'all DataChannel messages are valid Opus audio');
    assert.ok(data.packets.length > seconds * 10, 'received enough Opus audio');
    assert.ok(data.decoded > seconds * 10, 'Opus audio was actually decoded');
    assert.ok(data.packets.every(p => p.frames > 0 && Number.isSafeInteger(p.pos) && p.pos >= 0), 'valid audio timeline');
    assert.ok(Math.max(...data.packets.map(p => p.pos)) - Math.min(...data.packets.map(p => p.pos)) > seconds * 24000, 'audio timeline advances throughout the run');
    assert.ok(counts.received > seconds * 10 && counts.forwarded > 0, 'audio crossed the UDP proxy');
    if (profile.loss) assert.ok(counts.dropped > 0, 'loss profile dropped packets');
    if (profile.jitter) assert.ok(counts.reordered > 0, 'jitter caused packet reordering');
    const endT = start + seconds * 1000, windowStart = start + 2000;
    const measured = playout(data.packets.filter(p => p.t < endT), 48000, { endT, windowStart });
    assert.ok(measured.started && measured.startedAt <= windowStart && measured.windowMs > 0 && data.end >= endT, 'playout started before the fixed measurement window');
    await context.close(); context = null;
    const result = (await waitFor(/RESULT (.*)/, 40000))[1];
    if (hub.exitCode === null) await Promise.race([exited, sleep(5000)]);
    assert.equal(hub.exitCode, 0, `Hub did not exit successfully: ${output}`);
    const value = key => Number(new RegExp(`\\b${key}=(-?[0-9.]+)`).exec(result)?.[1]);
    assert.equal(value('live'), 1); assert.ok(value('writePos') > 48000 && value('peak') > 0.01, 'microphone reached Hub');
    const windowMs = (seconds - 2) * 1000;
    return { label, seed, gaps: measured.gaps, windowMs, gapsMin: measured.gaps * 60000 / windowMs, packets: data.packets.length, proxy: counts, result };
  } finally {
    try { if (context) await context.close(); }
    finally {
      try { proxy.close(); }
      finally { if (hub.exitCode === null && hub.signalCode === null) hub.kill(); await Promise.race([exited, sleep(5000)]); }
    }
  }
}

async function main() {
  const args = process.argv.slice(2), opt = (name, fallback) => { const i = args.indexOf('--' + name); return i < 0 ? fallback : args[i + 1]; };
  const cpp = opt('cpp'), rust = opt('rust', path.join(root, 'rust/target/release/hs-testkit.exe'));
  const seconds = Number(opt('seconds', 60)), runs = Number(opt('runs', 2)), seed = Number(opt('seed', 7));
  const names = opt('profiles', 'clean,bad').split(',');
  if (!cpp || !existsSync(cpp) || !existsSync(rust) || !Number.isFinite(seconds) || seconds < 5 || !Number.isInteger(runs) || runs < 1 || !Number.isInteger(seed) || seed < 0 || names.some(name => !profiles[name])) throw new Error('usage: --cpp <pure C++ executable> [--rust <exe>] [--seconds >=5] [--runs >=1] [--seed >=0] [--profiles clean,bad]');
  const { chromium } = createRequire(path.join(root, 'web/site/package.json'))('@playwright/test');
  const browser = await chromium.launch({ channel: 'chrome', args: ['--disable-features=WebRtcHideLocalIpsWithMdns', '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required'] });
  let failed = false;
  try {
    for (const name of names) {
      const profile = profiles[name];
      const results = { cpp: [], rust: [] };
      for (let i = 0; i < runs; i++) for (const type of (i % 2 ? ['rust', 'cpp'] : ['cpp', 'rust'])) {
        const sample = await run(browser, type === 'cpp' ? cpp : rust, profile, seed + i, seconds, `${name}/${type}/${i + 1}`);
        results[type].push(sample); console.log('SAMPLE ' + JSON.stringify(sample));
      }
      const mean = type => results[type].reduce((sum, x) => sum + x.gapsMin, 0) / runs;
      const c = mean('cpp'), r = mean('rust');
      console.log(`RESULT ${name}: ${r <= c ? 'PASS' : 'FAIL'} C++ ${c} Rust ${r} gaps/min`);
      if (r > c) failed = true;
    }
  } finally { await browser.close(); }
  if (failed) process.exitCode = 1;
}
if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) main().catch(e => { console.error(e.stack); process.exitCode = 2; });
