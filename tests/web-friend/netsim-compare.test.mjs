// The playout model of netsim-compare.mjs on hand-made arrivals (no Hub, no network).
//   node tests/web-friend/netsim-compare.test.mjs
import { playout, worse } from './netsim-compare.mjs';

let failures = 0;
const expect = (what, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${extra ? ' ' + extra : ''}`); if (!ok) failures++; };
const R = 48000, F = 480;   // 10 ms packets
// one packet every 10 ms for `n` packets from `t0`, with the extra arrival delay `late(i)`; `pos` runs on, `skip(i)` frames are cut out before packet i
const line = (n, late = () => 0, skip = () => 0, t0 = 0) => { let pos = 0; return Array.from({ length: n }, (_, i) => { pos += skip(i); const a = { t: t0 + i * 10 + late(i), pos, frames: F }; pos += F; return a; }); };
const opt = { endT: 10000, follow: false };

{   // a steady line: nothing
  const r = playout(line(1000), R, opt);
  expect('a steady line has no gaps', r.gaps === 0 && r.jumps === 0 && r.trims === 0, JSON.stringify(r));
}
{   // 300 ms stall at 5 s, the packets come in a burst afterwards: one starve of about 250 ms, then it plays on
  const r = playout(line(1000, i => (i >= 500 && i < 530 ? (530 - i) * 10 : 0)), R, opt);
  expect('a stall longer than the buffer is one gap', r.gaps === 1 && r.starves === 1 && r.jumps === 0, JSON.stringify(r));
  expect('and its silence is the stall minus the 60 ms of buffer', r.starveMs > 200 && r.starveMs < 300, `(${r.starveMs.toFixed(0)} ms)`);
}
{   // a stall shorter than the buffer (40 ms) is not heard
  const r = playout(line(1000, i => (i >= 500 && i < 504 ? (504 - i) * 10 : 0)), R, opt);
  expect('a stall shorter than the buffer is no gap', r.gaps === 0, JSON.stringify(r));
}
{   // the Hub skips 100 ms ahead while the friend is playing: a jump, counted once, and the audio it cut is not "delivered"
  const r = playout(line(1000, () => 0, i => (i === 500 ? 4800 : 0)), R, opt);
  expect('a skip of 100 ms is one jump / gap', r.gaps === 1 && r.jumps === 1 && r.starves === 0, JSON.stringify(r));
  expect('and 100 ms of the timeline never arrived', Math.abs(r.skippedMs - 100) < 1, `(${r.skippedMs} ms)`);
}
{   // a skip of 5 ms is below the 10 ms threshold: no gap, but still not delivered
  const r = playout(line(1000, () => 0, i => (i === 500 ? 240 : 0)), R, opt);
  expect('a hole under 10 ms is no gap', r.gaps === 0 && Math.abs(r.skippedMs - 5) < 0.1, JSON.stringify(r));
}
{   // a skip while starved belongs to the starve
  const a = line(1000, i => (i >= 500 && i < 530 ? (530 - i) * 10 : 0), i => (i === 502 ? 4800 : 0));
  const r = playout(a, R, opt);
  expect('a skip during a starve is not counted twice', r.gaps === 1 && r.jumps === 0, JSON.stringify(r));
}
{   // starvation begins before the measurement window and continues through it
  const r = playout(line(100), R, opt);
  expect('starvation crossing warmup is measured within the window', r.gaps === 1 && r.starveMs === 8000, JSON.stringify(r));
}
{
  expect('playout reports when priming never completes', !playout(line(100), R, { ...opt, primeMs: 1000000 }).started);
}
{
  const r = playout(line(1000, () => 0, () => 0, 150), R, { windowStart: 2000, endT: 10000 });
  expect('external window is independent of first packet time', r.windowMs === 8000 && r.startedAt < 2000);
}
{   // the rule
  expect('worse(): strictly compares mean gaps/min', worse(2, 2.01) && !worse(2, 2) && !worse(2, 1));
  let rejected = false;
  try { worse(NaN, 0); } catch { rejected = true; }
  expect('worse(): missing data is rejected', rejected);
}
console.log(failures === 0 ? 'netsim-compare model: all passed' : 'netsim-compare model: FAILED');
process.exit(failures === 0 ? 0 : 1);
