// net-sim itself: delay, jitter with the order kept, the kbps cap and the stalls do what they say.
//   node tools/net-sim/net-sim.test.mjs
import net from 'node:net';
import { rng, start } from './net-sim.mjs';

let failures = 0;
const expect = (what, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${extra ? ' ' + extra : ''}`); if (!ok) failures++; };
{
  const a = rng(7), b = rng(7), c = rng(8);
  const aa = Array.from({ length: 10 }, a), bb = Array.from({ length: 10 }, b), cc = Array.from({ length: 10 }, c);
  expect('seed repeats the random sequence', aa.every((v, i) => v === bb[i]) && aa.some((v, i) => v !== cc[i]));
  expect('zero seed uses unseeded randomness', rng(0) === Math.random);
}

// the "Hub": answers every byte it gets with the same byte, and can also just send what it is told to
const hub = net.createServer(s => { s.on('data', d => s.write(d)); s.on('error', () => {}); });
await new Promise(r => hub.listen(0, '127.0.0.1', r));
const target = hub.address().port;
const open = async sim => {
  const c = net.connect(sim.port, '127.0.0.1');
  await new Promise(r => c.on('connect', r));
  c.setNoDelay(true);
  return c;
};

{   // delay: 40 ms each way, a round trip is 80 ms or a bit more
  const sim = await start({ target, listen: 0, delay: 40 });
  const c = await open(sim);
  const t0 = performance.now();
  c.write('x');
  await new Promise(r => c.once('data', r));
  const rtt = performance.now() - t0;
  expect('delay 40 ms: a round trip takes 80..120 ms', rtt >= 78 && rtt < 120, `(${rtt.toFixed(0)} ms)`);
  c.destroy(); sim.close();
}
{   // jitter: random, but what was sent first comes first
  const sim = await start({ target, listen: 0, delay: 5, jitter: 60 });
  const c = await open(sim);
  const got = [];
  c.on('data', d => got.push(...d));
  for (let i = 0; i < 100; i++) { c.write(Buffer.from([i])); await new Promise(r => setTimeout(r, 2)); }
  await new Promise(r => setTimeout(r, 500));
  expect('jitter 60 ms: all 100 bytes arrive in order', got.length === 100 && got.every((v, i) => v === i));
  c.destroy(); sim.close();
}
{   // 50 KB takes about a second at 400 kbps; echoing overlaps both directions as bytes stream through.
  const sim = await start({ target, listen: 0, kbps: 400 });
  const c = await open(sim);
  let got = 0, firstAt = 0;
  c.on('data', d => { if (!got) firstAt = performance.now(); got += d.length; });
  const t0 = performance.now();
  c.write(Buffer.alloc(50 * 1024));
  while (got < 50 * 1024 && performance.now() - t0 < 6000) await new Promise(r => setTimeout(r, 20));
  const secs = (performance.now() - t0) / 1000;
  expect('cap 400 kbps: streamed echo takes 0.95..1.5 s', got === 50 * 1024 && secs > 0.95 && secs < 1.5, `(${secs.toFixed(2)} s)`);
  c.destroy(); sim.close();
}
{
  const measure = async chunked => {
    const sim = await start({ target, listen: 0, kbpsDown: 800 });
    const c = await open(sim);
    const t0 = performance.now(), data = Buffer.alloc(24000);
    let got = 0, first = 0, last = 0;
    c.on('data', d => { if (!got) first = performance.now() - t0; got += d.length; last = performance.now() - t0; });
    if (chunked) for (let i = 0; i < data.length; i += 1000) c.write(data.subarray(i, i + 1000));
    else c.write(data);
    while (got < data.length && performance.now() - t0 < 2000) await new Promise(r => setTimeout(r, 5));
    c.destroy(); sim.close();
    return { got, first, last };
  };
  const a = await measure(false), b = await measure(true);
  expect('capped line releases large and small TCP writes progressively',
    [a, b].every(x => x.got === 24000 && x.first < 120 && x.last - x.first > 150) && Math.abs(a.last - b.last) < 150,
    JSON.stringify({ a, b }));
}
{   // stalls: the line freezes for 300 ms every ~0.3 s on average, so some wait is at least that long
  const sim = await start({ target, listen: 0, stallEvery: 0.3, stallMs: 300 });
  const c = await open(sim);
  let last = performance.now(), worst = 0;
  c.on('data', () => { const n = performance.now(); worst = Math.max(worst, n - last); last = n; });
  for (let i = 0; i < 200; i++) { c.write('x'); await new Promise(r => setTimeout(r, 10)); }
  await new Promise(r => setTimeout(r, 600));
  expect('stall 300 ms: the bytes arrive in a burst after a pause of about 300 ms', worst >= 250, `(longest pause ${worst.toFixed(0)} ms)`);
  c.destroy(); sim.close();
}

hub.close();
console.log(failures === 0 ? 'net-sim: all passed' : 'net-sim: FAILED');
process.exit(failures === 0 ? 0 : 1);
