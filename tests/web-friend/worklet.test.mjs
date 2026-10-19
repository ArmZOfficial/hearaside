// The friend page's audio worklet (plugins/hub/web/send.html) run in plain Node with a fake AudioWorklet
// environment: no browser needed. Checks what leaves for the Hub (mono / stereo packets) and how the music
// from the Hub is played back (jitter buffer).
//   node tests/web-friend/worklet.test.mjs
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const html = readFileSync(join(here, '..', '..', 'plugins', 'hub', 'web', 'send.html'), 'utf8');
const source = /const worklet = `([\s\S]*?)`;/.exec(html)[1];
// the listen page's player (plugins/hub/web/listen.html): the same jitter buffer for people who only listen
const listenHtml = readFileSync(join(here, '..', '..', 'plugins', 'hub', 'web', 'listen.html'), 'utf8');
const Core = new Function(/const core = `([\s\S]*?)`;/.exec(listenHtml)[1] + '; return Core;')();

let failures = 0;
const expect = (what, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${extra ? ' ' + extra : ''}`); if (!ok) failures++; };

// a processor as the browser would make it; `posted` = what it sends to the page
function makeProcessor() {
  let Cls = null;
  const posted = [];
  class Base { constructor() { this.port = { postMessage: m => posted.push(m), onmessage: null }; } }
  new Function('AudioWorkletProcessor', 'registerProcessor', 'sampleRate', source)(Base, (n, c) => { Cls = c; }, 48000);
  const p = new Cls();
  return { p, posted, send: data => p.port.onmessage({ data }) };
}

// ---- 1.8: the microphone goes out mono unless the page asks for stereo --------------------------
{
  const { p, posted, send } = makeProcessor();
  const L = new Float32Array(128), R = new Float32Array(128), out = [[new Float32Array(128), new Float32Array(128)]];
  const run = (blocks, chans) => { for (let b = 0; b < blocks; b++) { for (let i = 0; i < 128; i++) { L[i] = 0.25; R[i] = -0.5; } p.process([chans === 1 ? [L] : [L, R]], out); } };
  run(8, 1);
  const mono = posted.filter(m => m.pcm);
  expect('mono by default: a 10 ms packet is 480 samples, chans = 1', mono.length > 0 && mono.every(m => m.pcm.length === 480 && m.chans === 1));
  posted.length = 0;
  send({ chans: 2 });
  run(8, 2);
  const st = posted.filter(m => m.pcm);
  expect('stereo on: 960 samples, chans = 2, left and right kept apart', st.length > 0 && st.every(m => m.pcm.length === 960 && m.chans === 2) && st[0].pcm[0] === 0.25 && st[0].pcm[1] === -0.5);
}

// ---- 1.6: the music from the Hub through a line with jitter --------------------------------------
// The Hub sends 10 ms packets of a constant 0.5; a packet shows up at its nominal time + a random delay: mostly
// 0-4 ms, one in ten up to jitterMs (never before the one ahead of it, like TCP). The player runs in 128-frame quanta. Silence = a gap.
function friendPlayer() {
  const { p, send } = makeProcessor();
  const out = [new Float32Array(128), new Float32Array(128)];
  return { out, p, push: (pcm, pos) => send({ rate: 48000, pcm, pos }), run: () => p.process([[new Float32Array(128)]], [out]) };
}
function listenPlayer(base) {
  const c = new Core(() => {});
  c.message({ target: base });
  const out = [new Float32Array(128), new Float32Array(128)];
  return { out, p: c, push: pcm => c.message({ rate: 48000, ch: 2, pcm }), run: () => c.render(out[0], out[1], 48000, 1e9) };
}
function playThrough({ seconds, jitterMs, stalls = [], pktFrames = 480, seed = 7, player = friendPlayer() }) {
  let x = seed;
  const rnd = () => (x = (x * 1664525 + 1013904223) >>> 0) / 4294967296;   // same "random" every run
  const { p, out } = player;
  const nom = pktFrames / 48000;
  const arrivals = [];
  let last = 0;
  for (let i = 0; i * nom < seconds; i++) {
    let a = i * nom + (rnd() < 0.1 ? rnd() * jitterMs : rnd() * 4) / 1000;
    for (const [at, len] of stalls) if (a >= at && a < at + len) a = at + len + rnd() * 0.002;   // held up, released together
    a = Math.max(a, last); last = a;
    arrivals.push(a);
  }
  const pcm = new Int16Array(pktFrames * 2).fill(16384);
  let next = 0, silent = 0, started = false, maxStep = 0, prev = 0, fillSum = 0, fillN = 0;
  const gaps = [];   // length (ms) of every run of silence after the music had started
  for (let q = 0; q * 128 / 48000 < seconds; q++) {
    const now = q * 128 / 48000;
    while (next < arrivals.length && arrivals[next] <= now) { player.push(pcm, next * pktFrames); next++; }
    player.run();
    if (now > 10) { fillSum += p.w - p.rd; fillN++; }
    for (let i = 0; i < 128; i++) {
      const v = out[0][i];
      if (started && Math.abs(v - prev) > maxStep) maxStep = Math.abs(v - prev);
      prev = v;
      if (Math.abs(v) > 0.4) started = true;
      if (started && now > 10) {
        if (Math.abs(v) < 1e-4) silent++;
        else { if (silent) gaps.push(silent / 48); silent = 0; }
      }
    }
  }
  const late = arrivals.map((a, i) => a - i * nom), ref = Math.min(...late);
  const sorted = late.map(v => v - ref).sort((a, b) => a - b);
  return { gaps, maxStep, meanFillMs: fillSum / fillN / 48, target: p.target, p95Ms: sorted[Math.floor(sorted.length * 0.95)] * 1000 };
}
{
  const r = playThrough({ seconds: 600, jitterMs: 40 });
  const long = r.gaps.filter(g => g >= 10).length;
  console.log(`     jitter up to 40 ms, 10 min: ${r.gaps.length} gaps, ${long} of them >= 10 ms, jitter p95 ${r.p95Ms.toFixed(1)} ms, buffer ${r.meanFillMs.toFixed(1)} ms, target ${(r.target * 1000).toFixed(0)} ms, biggest step ${r.maxStep.toFixed(3)}`);
  expect('jitter up to 40 ms for 10 minutes: gaps of 10 ms or more at most once a minute', long <= 10);
  expect('the buffer follows the jitter: its 95th percentile + 15 ms at most', r.meanFillMs <= r.p95Ms + 15);
  expect('no click: the signal never jumps', r.maxStep < 0.05);
}
{
  const r = playThrough({ seconds: 60, jitterMs: 4, stalls: [[30, 0.06]] });
  const worst = Math.max(0, ...r.gaps);
  console.log(`     one 60 ms stall: gaps ${r.gaps.map(g => g.toFixed(0)).join(', ') || 'none'} ms, biggest step ${r.maxStep.toFixed(3)}`);
  expect('a 60 ms stall: a short fade and on, the silence is not longer than the stall', worst <= 60);
  expect('a 60 ms stall: no click', r.maxStep < 0.05);
}
{
  const r = playThrough({ seconds: 60, jitterMs: 4, stalls: [[30, 0.5]] });
  console.log(`     one 500 ms stall: gaps ${r.gaps.map(g => g.toFixed(0)).join(', ') || 'none'} ms`);
  expect('a 500 ms stall: one gap, music is back after it', r.gaps.length >= 1 && r.gaps.length <= 2);
}

{   // the listen page: its chosen latency is the least it keeps
  const r = playThrough({ seconds: 120, jitterMs: 40, player: listenPlayer(0.12) });
  console.log(`     listen page, 120 ms mode, jitter up to 40 ms: ${r.gaps.length} gaps, buffer ${r.meanFillMs.toFixed(1)} ms, biggest step ${r.maxStep.toFixed(3)}`);
  expect('listen page: a line with jitter makes no gap in the 120 ms mode', r.gaps.length === 0);
  expect('listen page: the buffer stays at the chosen latency', Math.abs(r.meanFillMs - 120) < 15);
  const g = playThrough({ seconds: 60, jitterMs: 4, stalls: [[30, 0.2]], player: listenPlayer(0.12) });
  console.log(`     listen page, 120 ms mode, one 200 ms stall: gaps ${g.gaps.map(v => v.toFixed(0)).join(', ') || 'none'} ms, biggest step ${g.maxStep.toFixed(3)}`);
  expect('listen page: a stall is one gap, no longer than the stall, without a click', g.gaps.length === 1 && g.gaps[0] <= 200 && g.maxStep < 0.05);
}

console.log(failures === 0 ? 'worklet: all passed' : 'worklet: FAILED');
process.exit(failures === 0 ? 0 : 1);
