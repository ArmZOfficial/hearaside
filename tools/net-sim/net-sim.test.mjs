// net-sim itself: delay, jitter with the order kept, the kbps cap and the stalls do what they say.
//   node tools/net-sim/net-sim.test.mjs
import net from 'node:net';
import { start } from './net-sim.mjs';

let failures = 0;
const expect = (what, ok, extra = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${extra ? ' ' + extra : ''}`); if (!ok) failures++; };

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
{   // cap: 400 kbps = 50 KB/s, so 50 KB one way takes about a second (the echo comes back at the same speed, so ~2 s in all)
  const sim = await start({ target, listen: 0, kbps: 400 });
  const c = await open(sim);
  let got = 0, firstAt = 0;
  c.on('data', d => { if (!got) firstAt = performance.now(); got += d.length; });
  const t0 = performance.now();
  c.write(Buffer.alloc(50 * 1024));
  while (got < 50 * 1024 && performance.now() - t0 < 6000) await new Promise(r => setTimeout(r, 20));
  const secs = (performance.now() - t0) / 1000;
  expect('cap 400 kbps: 50 KB out and back takes 1.6..3 s', got === 50 * 1024 && secs > 1.6 && secs < 3, `(${secs.toFixed(2)} s)`);
  c.destroy(); sim.close();
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
