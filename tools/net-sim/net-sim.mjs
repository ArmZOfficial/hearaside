// A bad internet line between a browser and the Hub: delay, random jitter, TCP-style stalls (everything held,
// then let through in one burst), and a kbps cap. A TCP proxy, so the page, its WebSocket and everything else go
// through it. No dependencies.
//   node tools/net-sim --delay 30 --jitter 20 --kbps 800
//   node tools/net-sim --target 47810 --listen 47900 --kbps-down 300 --stall-every 8 --stall-ms 300
// Open the friend link with the proxy's port instead of the Hub's. tests/web-friend/friend-page.test.mjs does it for you
// when NET_SIM="--delay 30 ..." is set. A capped line stops reading from the Hub when its queue is full, but the
// operating system still buffers some (a few hundred KB on loopback) before the Hub's own socket reports "full".
//   --target <port>      the Hub (default 47810)         --host <addr>   where the Hub is (default 127.0.0.1)
//   --listen <port>      this proxy (default 47900)
//   --delay <ms>         one way, both directions        --jitter <ms>   0..ms more per chunk, order kept (TCP)
//   --kbps <n>           cap, both directions            --kbps-down / --kbps-up   Hub to page / page to Hub
//   --stall-every <s>    on average every s seconds the line freezes ...   --stall-ms <ms>   ... for this long
//   --queue-kb <n>       what a capped line holds before it stops reading from the sender (default 16)
//   --seed <n>           the same jitter and the same stall times every run (default 0: random). For comparing two Hubs on one line.
import net from 'node:net';
import { pathToFileURL } from 'node:url';

export function parseArgs(argv) {
  const o = { target: 47810, host: '127.0.0.1', listen: 47900, delay: 0, jitter: 0, kbps: 0, kbpsDown: 0, kbpsUp: 0, stallEvery: 0, stallMs: 0, queueKb: 16, seed: 0 };
  const key = { '--target': 'target', '--host': 'host', '--listen': 'listen', '--delay': 'delay', '--jitter': 'jitter', '--kbps': 'kbps', '--kbps-down': 'kbpsDown',
                '--kbps-up': 'kbpsUp', '--stall-every': 'stallEvery', '--stall-ms': 'stallMs', '--queue-kb': 'queueKb', '--seed': 'seed' };
  for (let i = 0; i < argv.length; i += 2) {
    const k = key[argv[i]];
    if (!k || argv[i + 1] === undefined) throw new Error(`unknown or incomplete option ${argv[i]}`);
    o[k] = k === 'host' ? argv[i + 1] : Number(argv[i + 1]);
    if (k !== 'host' && !Number.isFinite(o[k])) throw new Error(`${argv[i]} wants a number`);
  }
  return o;
}

// a small seeded generator (mulberry32) so that a run can be repeated; seed 0 is Math.random
export function rng(seed) {
  if (!seed) return Math.random;
  let a = seed >>> 0;
  return () => { a = (a + 0x6D2B79F5) >>> 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
}

// One direction of one connection: bytes go in, come out later, in the same order.
class Pipe {
  constructor(from, to, o, kbps, stalls, stats, rand = Math.random) {
    Object.assign(this, { from, to, o, kbps, stalls, stats, rand });
    this.q = [];            // { at, buf }
    this.queued = 0;        // bytes waiting (what a real router would hold)
    this.lastDue = 0;       // order: nothing leaves before what came earlier
    this.txEnd = 0;         // when the cap has finished sending what is ahead
    this.timer = null;
    from.on('data', d => this.push(d));
    from.on('end', () => this.finish());
    from.on('close', () => this.finish());
    this.ended = false;
  }
  push(buf) {
    // A TCP read can contain many packets; a capped link releases those bytes progressively, not as one giant burst.
    if (this.kbps > 0 && buf.length > 1460) {
      for (let i = 0; i < buf.length; i += 1460) this.push(buf.subarray(i, i + 1460));
      return;
    }
    const now = performance.now();
    let due = now + this.o.delay + this.rand() * this.o.jitter;
    if (this.kbps > 0) {   // serialisation: bytes * 8 / rate after the cap is free
      this.txEnd = Math.max(this.txEnd, due) + buf.length * 8 / this.kbps;
      due = this.txEnd;
    }
    due = Math.max(due, this.lastDue);
    for (const [s, e] of this.stalls.windows(due)) if (due >= s && due < e) due = e + this.rand() * 2;   // held up, released together
    this.lastDue = due;
    this.q.push({ due, buf, at: now });
    this.queued += buf.length;
    if (this.kbps > 0 && this.queued > this.o.queueKb * 1024) this.from.pause();   // like TCP: the sender has to wait
    this.arm();
  }
  arm() {
    if (this.timer || !this.q.length) return;
    this.timer = setTimeout(() => this.pump(), Math.max(0, this.q[0].due - performance.now()));
  }
  pump() {
    this.timer = null;
    const now = performance.now();
    while (this.q.length && this.q[0].due <= now + 0.5) {
      const c = this.q.shift();
      this.queued -= c.buf.length;
      this.stats.bytes += c.buf.length;
      this.stats.maxDelayMs = Math.max(this.stats.maxDelayMs, now - c.at);
      if (!this.to.destroyed) this.to.write(c.buf);
    }
    if (this.kbps > 0 && this.queued <= this.o.queueKb * 1024 / 2) this.from.resume();
    if (this.ended && !this.q.length) this.to.end();
    this.arm();
  }
  finish() { this.ended = true; if (!this.q.length && !this.to.destroyed) this.to.end(); }
  cancel() { clearTimeout(this.timer); this.timer = null; this.q = []; }
}

// freezes of the whole line, the same for every connection, planned ahead as needed
class Stalls {
  constructor(every, ms, rand = Math.random) { this.every = every; this.ms = ms; this.rand = rand; this.list = []; this.next = performance.now() + this.gap(); }
  gap() { return -Math.log(1 - this.rand()) * this.every * 1000; }   // exponential: any time, on average every `every` s
  windows(upTo) {
    if (!(this.every > 0 && this.ms > 0)) return [];
    while (this.next < upTo + this.ms) { this.list.push([this.next, this.next + this.ms]); this.next += this.ms + this.gap(); }
    const now = performance.now();
    this.list = this.list.filter(w => w[1] > now - 1000);
    return this.list;
  }
}

export function start(opts) {
  const o = { ...parseArgs([]), ...opts };
  const stalls = new Stalls(o.stallEvery, o.stallMs, rng(o.seed ? o.seed * 3 + 1 : 0));
  const stats = { down: { bytes: 0, maxDelayMs: 0 }, up: { bytes: 0, maxDelayMs: 0 }, connections: 0 };
  const active = new Set();
  const server = net.createServer(client => {
    stats.connections++;
    const hub = net.connect(o.target, o.host);
    const kill = () => { client.destroy(); hub.destroy(); };
    client.on('error', kill); hub.on('error', kill);
    // no Nagle on our side: the line's timing is the simulated one
    client.setNoDelay(true); hub.setNoDelay(true);
    const up = new Pipe(client, hub, o, o.kbpsUp || o.kbps, stalls, stats.up, rng(o.seed ? o.seed * 3 + 2 : 0));
    const down = new Pipe(hub, client, o, o.kbpsDown || o.kbps, stalls, stats.down, rng(o.seed ? o.seed * 3 + 3 : 0));
    const close = () => { up.cancel(); down.cancel(); client.destroy(); hub.destroy(); active.delete(close); };
    active.add(close);
    client.on('close', close);
    hub.on('close', close);
  });
  return new Promise(resolve => server.listen(o.listen, '127.0.0.1', () => resolve({ server, stats, port: server.address().port, close: () => { for (const close of active) close(); server.close(); } })));
}

if (import.meta.url === pathToFileURL(process.argv[1]).href || process.argv[1].replace(/\\/g, '/').endsWith('/tools/net-sim')) {
  let o;
  try { o = parseArgs(process.argv.slice(2)); } catch (e) { console.error(e.message + '\nsee the top of tools/net-sim/net-sim.mjs'); process.exit(2); }
  const sim = await start(o);
  console.log(`net-sim: 127.0.0.1:${sim.port} -> ${o.host}:${o.target}  delay ${o.delay} ms, jitter ${o.jitter} ms, cap ${o.kbpsDown || o.kbps}/${o.kbpsUp || o.kbps} kbps (down/up), stall ${o.stallMs} ms every ~${o.stallEvery} s`);
  const report = () => { console.log(`net-sim: Hub to page ${sim.stats.down.bytes} B (worst wait ${sim.stats.down.maxDelayMs.toFixed(0)} ms), page to Hub ${sim.stats.up.bytes} B (worst wait ${sim.stats.up.maxDelayMs.toFixed(0)} ms)`); };
  process.on('SIGINT', () => { report(); process.exit(0); });
  setInterval(report, 10000).unref();
}
