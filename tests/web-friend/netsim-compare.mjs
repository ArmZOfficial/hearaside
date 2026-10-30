// R3.7: the C++ Hub and the Rust Hub on the same bad lines. One friend, WebSocket path (HRA1 microphone up, HRA2 PCM or
// HRA3 Opus down), through tools/net-sim; the same scenario and the same seeded line for both binaries, runs alternating.
//   node tests/web-friend/netsim-compare.mjs [--cpp <ui-snapshot.exe>] [--rust <hs-testkit.exe>] [--seconds 60] [--runs 2]
//        [--profiles clean,home,mobile,starved] [--codecs pcm,opus] [--seed 7] [--prime 60]
//        [--trace-dir <existing directory>]  save timestamped packet positions for diagnosis; existing files are never overwritten
// Exit 0 when Rust is not worse (rule below) on every profile / codec, 1 when it is, 2 on a usage error or a failed run.
//
// What is measured (the same code for both binaries):
//  Down (Hub -> friend), at the friend: a playout model with a fixed jitter buffer (--prime, 60 ms like the page). Audio is
//  placed by the `pos` field of every HRA2 / HRA3 packet (the Stream Mix timeline), played at a fixed 48 kHz once the buffer
//  holds `prime` ms; when it runs dry the model waits for `prime` ms again.
//   gap = a hole of 10 ms or more in the audio the friend plays, either
//     starve: the buffer ran dry while playing (the silence lasts until it holds `prime` ms again), or
//     jump:   a packet whose pos is 10 ms or more ahead of the end of the previous one (the Hub skipped ahead because the
//             line was blocked), when the friend was still playing. A jump while starved belongs to that starve.
//   gaps/min = gaps * 60 / measured seconds (the fixed interval from 2 s after scheduled start to scheduled stop).
//   holes % = (starve silence + skipped audio) / measured time. delivered % = audio received / audio the Hub's timeline covered.
//  Up (friend -> Hub): 10 ms mono HRA1 packets at an absolute 10 ms schedule. A relay between net-sim and the Hub timestamps
//  every packet as it is handed to the Hub's socket; the same model (60 ms) gives the line-level gaps/min. What the Hub itself
//  saw comes from its RESULT line: micGap (longest pause between microphone packets), writePos (audio that reached the
//  friend's ring) against what the relay delivered, skips (times it jumped ahead because the line down was blocked), lag
//  (the longest wait of audio for the line), rtt (from the pongs).
// The plan's criterion is the observed mean down gaps/min, without an allowance for measurement noise.
// Two runs give descriptive evidence only, not statistical proof of equivalence.
// Not covered: WebRTC / UDP (net-sim is a TCP proxy; real loss and reordering need a UDP path), a real internet, a phone.
// This synthetic fixed-buffer model is not the page's adaptive jitter buffer. TCP stalls and bursts are not UDP loss/reorder.
// The seed repeats random draws, not identical packet conditions: TCP batching can differ between Hubs.
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import { existsSync, statSync, writeFileSync } from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { start as startNetSim } from '../../tools/net-sim/net-sim.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..');
const sleep = ms => new Promise(r => setTimeout(r, ms));

// the lines: net-sim options. clean / home are the recorded traces' profiles (compat/golden/netsim); mobile adds TCP stalls (a
// lossy mobile link) and caps the line at 800 kbps down; starved is below even the Opus stream (128 kbps) and the PCM one.
export const profiles = {
  clean: { delay: 10, jitter: 2 },
  home: { delay: 25, jitter: 10, stallEvery: 8, stallMs: 300 },
  mobile: { delay: 40, jitter: 25, kbpsDown: 800, kbpsUp: 1000, stallEvery: 6, stallMs: 400 },
  starved: { delay: 30, jitter: 10, kbpsDown: 100, kbpsUp: 1000 },
};

// ---- the playout model (pure: arrivals in, counts out) ----
// arrivals: [{ t (ms), pos (frames), frames }] in the order they came; rate in frames per second.
export function playout(arrivals, rate, { primeMs = 60, warmupMs = 2000, windowStart, endT, gapMs = 10, trimMs = 600 } = {}) {
  const r = { gaps: 0, starves: 0, jumps: 0, trims: 0, starveMs: 0, skippedMs: 0, receivedMs: 0, windowMs: 0, maxBufferMs: 0, staleMaxMs: 0, staleMeanMs: 0, clock: 1, packets: arrivals.length, started: false, startedAt: null };
  if (!arrivals.length) return r;
  const ms = f => f * 1000 / rate;
  const winStart = windowStart ?? arrivals[0].t + warmupMs;
  r.windowMs = Math.max(0, endT - winStart);
  let staleSum = 0, staleN = 0, level = 0, playing = false, started = false, tl = arrivals[0].t, expect = arrivals[0].pos, starveAt = 0;
  const advance = t => {
    if (playing) {
      const used = (t - tl) * r.clock;   // ms of audio played
      if (level <= used) { playing = false; starveAt = tl + level / r.clock; level = 0; }   // ran dry
      else level -= used;
    }
    tl = t;
  };
  const endStarve = t => {   // the silence lasted from starveAt to t
    const measured = t - Math.max(starveAt, winStart);
    if (measured < gapMs) return;
    r.gaps++; r.starves++; r.starveMs += measured;
  };
  for (const a of arrivals) {
    if (a.t >= endT) break;
    advance(a.t);
    const hole = a.pos > expect ? ms(a.pos - expect) : 0;
    if (a.t >= winStart) r.skippedMs += hole;
    if (hole >= gapMs && playing && a.t >= winStart) { r.gaps++; r.jumps++; }   // a jump while starved belongs to the starve
    if (a.pos < expect) continue;   // old or repeated audio: dropped
    expect = a.pos + a.frames;
    level += ms(a.frames);
    if (level > primeMs + trimMs && playing) { level = primeMs; if (a.t >= winStart) r.trims++; }   // the page cuts back to its target
    if (a.t >= winStart) {
      r.receivedMs += ms(a.frames); r.maxBufferMs = Math.max(r.maxBufferMs, level);
      const stale = Math.max(0, (a.t - arrivals[0].t) - ms(expect - arrivals[0].pos));   // how old the audio is: real time since the first packet minus the timeline it has reached
      r.staleMaxMs = Math.max(r.staleMaxMs, stale); staleSum += stale; staleN++;
    }
    if (!playing && level >= primeMs) {
      if (started) endStarve(a.t);
      playing = true;
      if (!started) r.startedAt = a.t;
      started = true;
      r.started = true;
    }
  }
  advance(endT);
  if (!playing && started) endStarve(endT);
  r.staleMeanMs = staleN ? staleSum / staleN : 0;
  return r;
}

// ---- WebSocket client bits ----
const maskedFrame = (op, payload) => {
  const m = crypto.randomBytes(4), body = Buffer.from(payload), n = body.length;
  for (let i = 0; i < n; i++) body[i] ^= m[i & 3];
  return Buffer.concat([n < 126 ? Buffer.from([0x80 | op, 0x80 | n]) : Buffer.from([0x80 | op, 0x80 | 126, n >> 8, n & 255]), m, body]);
};

// the relay between net-sim and the Hub: timestamps each complete masked microphone packet at the moment it goes to the Hub
function startRelay(hubPort, onMic) {
  const sockets = new Set();
  const server = net.createServer(client => {
    const hub = net.connect(hubPort, '127.0.0.1');
    sockets.add(client); sockets.add(hub);
    client.setNoDelay(true); hub.setNoDelay(true);
    let buf = Buffer.alloc(0), upgraded = false;
    client.on('data', d => {
      const now = performance.now();
      if (!hub.write(d)) { client.pause(); hub.once('drain', () => client.resume()); }
      buf = Buffer.concat([buf, d]);
      if (!upgraded) { const i = buf.indexOf('\r\n\r\n'); if (i < 0) return; buf = buf.subarray(i + 4); upgraded = true; }
      for (;;) {
        if (buf.length < 6) return;
        let len = buf[1] & 127, off = 2;
        if (len === 126) { if (buf.length < 8) return; len = buf.readUInt16BE(2); off = 4; }
        const total = off + 4 + len;
        if (buf.length < total) return;
        if ((buf[0] & 15) === 2) onMic(now, len);
        buf = buf.subarray(total);
      }
    });
    hub.pipe(client);   // with back pressure: a line that cannot take the audio must be seen by the Hub as a full socket
    client.on('close', () => { sockets.delete(client); hub.destroy(); }); hub.on('close', () => { sockets.delete(hub); client.destroy(); });
    client.on('error', () => hub.destroy()); hub.on('error', () => client.destroy());
  });
  return new Promise(r => server.listen(0, '127.0.0.1', () => r({ port: server.address().port, close: () => { for (const socket of sockets) socket.destroy(); server.close(); } })));
}

// one run: the Hub for `seconds` of friend, through net-sim; returns the numbers
async function run({ exe, label, profile, codec, seconds, seed, primeMs, traceDir }) {
  const hub = spawn(exe, ['--serve-friend', String(seconds + 4)], { env: { ...process.env, HEARASIDE_NO_TUNNEL: '1', HEARASIDE_COMPARE_CLOCK: '1' } });
  let sim, relay, sock, spawnError;
  hub.on('error', e => { spawnError = e; });
  const exited = new Promise(r => { hub.on('exit', r); hub.on('error', r); });
  try {
  let out = '';
  hub.stdout.on('data', d => { out += d; });
  hub.stderr.on('data', d => { out += d; });
  const waitFor = async (re, ms) => { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (spawnError) throw spawnError; const m = re.exec(out); if (m) return m; if (hub.exitCode !== null) break; await sleep(50); } return null; };
  const urlM = await waitFor(/FRIEND_URL=(\S+)/, 25000);
  if (!urlM) { hub.kill(); throw new Error(`${label}: the Hub did not print a friend link:\n${out}`); }
  const u = new URL(urlM[1]);
  const upArr = [];
  let upFrames = 0;
  relay = await startRelay(Number(u.port), (t, len) => { if (len >= 16 + 960) { upArr.push({ t, pos: upFrames, frames: 480 }); upFrames += 480; } });
  sim = await startNetSim({ ...profiles[profile], target: relay.port, listen: 0, seed });
  sock = net.connect(sim.port, '127.0.0.1');
  sock.setNoDelay(true);
  sock.on('error', () => {});
  sock.write(`GET /ws/s/${u.pathname.split('/').pop()} HTTP/1.1\r\nHost: ${u.host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`);

  const down = [], bad = { n: 0 };
  let rate = 0, bytesDown = 0, hello = null, upgraded = false, seqGaps = 0, lastSeq = -1, opusPackets = 0, pcmPackets = 0;
  let buf = Buffer.alloc(0);
  sock.on('data', d => {
    const now = performance.now();
    bytesDown += d.length;
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
      if (op === 1) {
        try {
          const j = JSON.parse(p.toString());
          if (j.v >= 2) { hello = j; if (codec === 'opus' && j.codecs?.includes('opus')) sock.write(maskedFrame(1, JSON.stringify({ codec: 'opus' }))); }
          if (j.ping !== undefined) sock.write(maskedFrame(1, JSON.stringify({ pong: j.ping })));
        } catch { bad.n++; }
      } else if (op === 2) {
        const magic = p.toString('latin1', 0, 4);
        if (magic === 'HRA2' && p.length >= 24) {
          pcmPackets++;
          rate = p.readUInt32LE(4);
          down.push({ t: now, pos: Number(p.readBigUInt64LE(16)), frames: p.readUInt32LE(12), codec: 'pcm' });
        } else if (magic === 'HRA3' && p.length >= 22) {
          opusPackets++;
          const seq = p.readUInt32LE(8);
          if (lastSeq >= 0 && seq !== lastSeq + 1) seqGaps++;
          lastSeq = seq;
          down.push({ t: now, pos: Number(p.readBigUInt64LE(12)), frames: p.readUInt16LE(20), codec: 'opus' });
        } else bad.n++;
      }
    }
  });

  // the microphone: 10 ms mono frames on an absolute schedule (a late timer sends what is due)
  const t0 = performance.now() + 300;
  let nextMic = 0, sent = 0, maxSendLate = 0;
  const stopAt = t0 + seconds * 1000;
  await new Promise(resolve => {
    const tick = () => {
      const now = performance.now();
      while (t0 + nextMic * 10 <= now && t0 + nextMic * 10 < stopAt) {
        if (upgraded) {
          maxSendLate = Math.max(maxSendLate, now - (t0 + nextMic * 10));
          const b = Buffer.alloc(16 + 480 * 2);
          b.write('HRA1', 0, 'latin1'); b.writeUInt32LE(48000, 4); b.writeUInt16LE(1, 8); b.writeUInt32LE(480, 12);
          for (let i = 0; i < 480; i++) b.writeInt16LE(Math.round(8000 * Math.sin((sent + i) * 0.058)), 16 + i * 2);
          sent += 480;
          sock.write(maskedFrame(2, b));
        }
        nextMic++;
      }
      if (now >= stopAt) return resolve();
      setTimeout(tick, Math.max(1, t0 + nextMic * 10 - performance.now()));
    };
    setTimeout(tick, 300);
  });
  const endT = stopAt, windowStart = t0 + 2000;
  const downAtEnd = down.filter(a => a.t < endT), upAtEnd = upArr.filter(a => a.t < endT);
  const relayedFrames = upAtEnd.reduce((sum, a) => sum + a.frames, 0);
  sock.destroy();
  const m = await waitFor(/RESULT (.*)/, 20000);
  const exitCode = await Promise.race([exited, sleep(5000)]);
  if (!m) throw new Error(`${label}: no RESULT line:\n${out}`);
  if (exitCode !== 0) throw new Error(`${label}: Hub did not exit successfully: ${exitCode}\n${out}`);
  const num = k => parseFloat(new RegExp('\\b' + k + '=(-?[0-9.]+)').exec(m[1])?.[1] ?? 'NaN');
  const hz = rate || 48000;
  const d = playout(downAtEnd, hz, { primeMs, windowStart, endT });
  const up = playout(upAtEnd, 48000, { primeMs, windowStart, endT });
  const measured = downAtEnd.filter(a => a.t >= windowStart);
  if (!hello || !num('live') || bad.n || d.windowMs <= 0 || !d.started || d.startedAt > windowStart || !measured.length || measured.some(a => a.codec !== codec) || !upAtEnd.length || !sent || !Number.isFinite(num('writePos')) || !Number.isFinite(num('micGap')) ||
      (codec === 'opus' && !hello.codecs?.includes('opus'))) {
    throw new Error(`${label}: invalid run (hello=${!!hello}, live=${num('live')}, bad=${bad.n}, pcm=${pcmPackets}, opus=${opusPackets}, up=${upArr.length}, sent=${sent}): ${m[1]}`);
  }
  if (traceDir) {
    const relative = a => ({ ...a, t: a.t - t0 });
    writeFileSync(path.join(traceDir, label.replaceAll('/', '_') + '.json'), JSON.stringify({ label, seed, windowStart: 2000, endT: seconds * 1000, down: downAtEnd.map(relative), up: upAtEnd.map(relative), raw: m[1] }), { flag: 'wx' });
  }
  const windowMs = (seconds - 2) * 1000, win = windowMs / 60000;
  return {
    label, profile, codec, packets: downAtEnd.length, pcmPackets: downAtEnd.filter(a => a.codec === 'pcm').length, opusPackets: downAtEnd.filter(a => a.codec === 'opus').length, bad: bad.n, seqGaps, maxSendLate, gaps: d.gaps, windowMs,
    downGapsMin: d.gaps / win, downStarvesMin: d.starves / win, downJumpsMin: d.jumps / win, trimsMin: d.trims / win, clock: d.clock, staleMean: d.staleMeanMs, staleMax: d.staleMaxMs,
    holesPct: 100 * (d.starveMs + d.skippedMs) / Math.max(1, d.windowMs),
    delivered: 100 * d.receivedMs / Math.max(1, d.receivedMs + d.skippedMs),
    clientBufMax: d.maxBufferMs, downKbps: bytesDown * 8 / 1000 / (seconds + 0.3),
    upGapsMin: up.gaps / win,
    upDelivered: relayedFrames > 0 ? 100 * num('writePos') / relayedFrames : NaN,
    upRelayedPct: sent > 0 ? 100 * relayedFrames / sent : NaN,
    micGap: num('micGap'), skips: num('skips'), lag: num('lag'), rtt: num('rtt'), live: num('live'), delay: num('delay'),
    raw: m[1],
  };
  } finally {
    sock?.destroy(); sim?.close(); relay?.close();
    if (hub.exitCode === null) hub.kill();
    await Promise.race([exited, sleep(5000)]);
  }
}

// ---- the table ----
const mean = a => a.reduce((x, y) => x + y, 0) / a.length;
const sd = a => a.length < 2 ? 0 : Math.sqrt(a.reduce((s, x) => s + (x - mean(a)) ** 2, 0) / (a.length - 1));
const cell = (runs, k, d = 1) => { const v = runs.map(r => r[k]).filter(Number.isFinite); return v.length ? `${mean(v).toFixed(d)} ±${sd(v).toFixed(d)}` : 'n/a'; };
const agg = (runs, k) => { const v = runs.map(r => r[k]).filter(Number.isFinite); return { m: v.length ? mean(v) : NaN, s: sd(v) }; };

export function worse(cpp, rust) {
  if (!Number.isFinite(cpp) || !Number.isFinite(rust)) throw new Error('missing comparison data');
  return rust > cpp;
}

async function main() {
  const args = process.argv.slice(2);
  const opt = (k, d) => { const i = args.indexOf('--' + k); return i >= 0 ? args[i + 1] : d; };
  const cppExe = path.resolve(opt('cpp', path.join(root, 'build/Release/plugins/hearaside_ui_snapshot_artefacts/Release/hearaside_ui_snapshot.exe')));
  const rustExe = path.resolve(opt('rust', path.join(root, 'rust/target/release/hs-testkit.exe')));
  const seconds = Number(opt('seconds', 60)), runs = Number(opt('runs', 2)), seed = Number(opt('seed', 7)), primeMs = Number(opt('prime', 60));
  const traceDir = opt('trace-dir', '');
  const names = opt('profiles', 'clean,home,mobile,starved').split(','), codecs = opt('codecs', 'pcm,opus').split(',');
  if (!Number.isFinite(seconds) || seconds <= 2 || !Number.isInteger(runs) || runs < 1 || !Number.isInteger(seed) || seed < 0 || !Number.isFinite(primeMs) || primeMs <= 0 || codecs.some(c => !['pcm', 'opus'].includes(c))) { console.error('invalid seconds, runs, seed, prime or codecs'); process.exit(2); }
  if (traceDir && (!existsSync(traceDir) || !statSync(traceDir).isDirectory())) { console.error('trace directory does not exist'); process.exit(2); }
  for (const e of [cppExe, rustExe]) if (!existsSync(e)) { console.error('no such binary: ' + e); process.exit(2); }
  for (const n of names) if (!profiles[n]) { console.error('unknown profile ' + n); process.exit(2); }
  console.log(`netsim-compare: ${seconds} s x ${runs} runs, seed ${seed}, client buffer ${primeMs} ms\n  cpp  ${cppExe}\n  rust ${rustExe}`);
  const results = {};   // `${profile}/${codec}` -> { cpp: [], rust: [] }
  for (const profile of names) for (const codec of codecs) {
    const cell_ = results[`${profile}/${codec}`] = { cpp: [], rust: [] };
    for (let i = 0; i < runs; i++) {
      const order = i % 2 === 0 ? ['cpp', 'rust'] : ['rust', 'cpp'];   // alternate, so that machine noise falls on both
      for (const which of order) {
        const label = `${profile}/${codec}/${which}#${i + 1}`;
        let r;
        try { r = await run({ exe: which === 'cpp' ? cppExe : rustExe, label, profile, codec, seconds, seed: seed + i, primeMs, traceDir }); }
        catch (e) { console.error(String(e.message ?? e)); process.exit(2); }
        cell_[which].push(r);
        console.log(`SAMPLE ${JSON.stringify(r)}`);
        console.log(`  ${label.padEnd(24)} down gaps/min ${r.downGapsMin.toFixed(1)} (jumps ${r.downJumpsMin.toFixed(1)}) delivered ${r.delivered.toFixed(1)}% | up gaps/min ${r.upGapsMin.toFixed(1)} hub ${r.upDelivered.toFixed(1)}% micGap ${r.micGap} | skips ${r.skips} lag ${r.lag} rtt ${r.rtt}${r.bad ? ' BAD=' + r.bad : ''}${r.live ? '' : ' NOT LIVE'}`);
      }
    }
  }
  // tables
  let fail = false;
  const cols = [['gaps/min down', 'downGapsMin', 1], ['(jumps/min)', 'downJumpsMin', 1], ['trims/min', 'trimsMin', 1], ['hub clock x', 'clock', 3], ['audio age mean ms', 'staleMean', 0], ['audio age max ms', 'staleMax', 0], ['holes %', 'holesPct', 2], ['delivered % down', 'delivered', 2], ['client buf max ms', 'clientBufMax', 0],
                ['gaps/min up (line)', 'upGapsMin', 1], ['delivered % up (Hub)', 'upDelivered', 2], ['micGap ms', 'micGap', 0], ['skips', 'skips', 1], ['lag ms', 'lag', 0], ['rtt ms', 'rtt', 1], ['kbps down', 'downKbps', 0]];
  for (const [key, g] of Object.entries(results)) {
    console.log(`\n### ${key}  (mean ±sd of ${runs} runs of ${seconds} s)`);
    console.log('| metric | C++ | Rust |\n|---|---|---|');
    for (const [name, k, d] of cols) console.log(`| ${name} | ${cell(g.cpp, k, d)} | ${cell(g.rust, k, d)} |`);
    const bad = [];
    if (worse(agg(g.cpp, 'downGapsMin').m, agg(g.rust, 'downGapsMin').m)) bad.push('down gaps/min');
    console.log(bad.length ? `RESULT ${key}: FAIL (${bad.join(', ')})` : `RESULT ${key}: pass`);
    if (bad.length) fail = true;
  }
  console.log(fail ? '\nnetsim-compare: observed Rust mean gaps/min is higher on at least one TCP line' : '\nnetsim-compare: observed Rust mean gaps/min is not higher on these TCP lines');
  process.exit(fail ? 1 : 0);
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) await main();
