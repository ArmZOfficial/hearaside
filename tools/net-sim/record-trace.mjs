// Records when 10 ms friend packets arrive through the real net-sim line, for the FriendReader golden
// (tools/compat-golden, compat/golden/netsim). The line is random, so a recording is a fixture: re-running
// writes a new, different trace. No dependencies.
//   node tools/net-sim/record-trace.mjs <directory> [--seconds 20]   (records only the profiles it has no trace for)
// One trace per profile of docs/PLAN-v2-realtime-karaoke.md (good, home, mobile; TCP has no loss). Each line:
//   <packet index> <arrival µs> <sent µs>   both after the sender's planned start; packet i is due at i * 10 ms
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { start as startNetSim } from './net-sim.mjs';

export const profiles = {
  good: { delay: 10, jitter: 2 },
  home: { delay: 25, jitter: 10, stallEvery: 8, stallMs: 300 },
  mobile: { delay: 40, jitter: 25, kbps: 800 },
};
const packetMs = 10;
const packetBytes = 96;   // about a 64 kbps 10 ms Opus frame and its header: what the cap has to carry
const maxSendLagMs = 5;   // a later send means this machine stalled (timers, GC): that is not the line

export async function record(name, seconds) {
  const count = Math.round(seconds * 1000 / packetMs);
  let t0 = 0;
  const sent = new Array(count).fill(-1), arrived = new Array(count).fill(-1);
  const sender = net.createServer(socket => {
    socket.setNoDelay(true);
    socket.on('error', () => {});
    t0 = performance.now() + 100;
    let next = 0;
    const tick = () => {   // absolute schedule: a late timer sends what is due, it never shifts later packets
      const now = performance.now();
      while (next < count && t0 + next * packetMs <= now) {
        const packet = Buffer.alloc(packetBytes);
        packet.writeUInt32LE(next, 0);
        sent[next] = performance.now() - t0;
        socket.write(packet);
        ++next;
      }
      if (next < count) setTimeout(tick, Math.max(0, t0 + next * packetMs - performance.now()));
    };
    setTimeout(tick, 100);
  });
  await new Promise(resolve => sender.listen(0, '127.0.0.1', resolve));
  const sim = await startNetSim({ ...profiles[name], target: sender.address().port, listen: 0 });
  await new Promise((resolve, reject) => {
    const client = net.connect(sim.port, '127.0.0.1');
    client.setNoDelay(true);
    let pending = Buffer.alloc(0), received = 0;
    client.on('data', chunk => {
      const now = performance.now() - t0;
      pending = Buffer.concat([pending, chunk]);
      while (pending.length >= packetBytes) {
        const index = pending.readUInt32LE(0);
        if (index >= count || arrived[index] >= 0) reject(new Error(`${name}: unexpected packet ${index}`));
        arrived[index] = now;
        pending = pending.subarray(packetBytes);
        if (++received === count) { client.destroy(); resolve(); }
      }
    });
    client.on('error', reject);
    setTimeout(() => reject(new Error(`${name}: only ${received}/${count} packets arrived`)), (seconds + 10) * 1000).unref();
  });
  sim.close(); sender.close();
  for (let i = 1; i < count; ++i) if (arrived[i] < arrived[i - 1]) throw new Error(`${name}: TCP reordered packet ${i}`);
  const sendLag = Math.max(...sent.map((t, i) => t - i * packetMs));
  if (sendLag > maxSendLagMs) return { sendLag };
  const us = v => Math.round(v * 1000);
  const options = Object.entries(profiles[name]).map(([k, v]) => `${k}=${v}`).join(' ');
  const lines = [
    `# net-sim trace ${name}: ${options}`,
    `# ${count} packets of ${packetBytes} bytes every ${packetMs} ms through tools/net-sim/net-sim.mjs on 127.0.0.1`,
    `# node ${process.version}, ${os.platform()} ${os.arch()}; sender at most ${sendLag.toFixed(1)} ms behind schedule`,
    '# index arrival_us sent_us (after the planned start of packet 0)',
    ...arrived.map((a, i) => `${i} ${us(a)} ${us(sent[i])}`),
  ];
  return { sendLag, text: lines.join('\n') + '\n' };
}

if (import.meta.url === new URL(`file://${path.resolve(process.argv[1])}`).href) {
  const args = process.argv.slice(2);
  const out = args[0];
  const seconds = args[1] === '--seconds' ? Number(args[2]) : 20;
  if (!out || !(seconds > 0)) { console.error('usage: node tools/net-sim/record-trace.mjs <directory> [--seconds 20]'); process.exit(2); }
  fs.mkdirSync(out, { recursive: true });
  for (const name of Object.keys(profiles)) {   // one at a time: a busy machine is what the lag check rejects
    const file = path.join(out, `${name}.trace`);
    if (fs.existsSync(file)) { console.log(`${name}: ${file} exists; recorded traces are never overwritten`); continue; }
    let result;
    for (let attempt = 1; attempt <= 20 && !result?.text; ++attempt) {
      result = await record(name, seconds);
      if (!result.text) console.log(`${name}: sender ${result.sendLag.toFixed(1)} ms behind schedule, recording again`);
    }
    if (!result.text) { console.error(`${name}: this machine keeps stalling the sender; nothing written`); process.exit(1); }
    const { text } = result;
    fs.writeFileSync(file, text, { flag: 'wx' });
    const rows = text.split('\n').filter(l => l && !l.startsWith('#')).map(l => l.split(' ').map(Number));
    const late = rows.map(([i, a]) => a / 1000 - i * packetMs);
    console.log(`${name}: ${rows.length} packets, one-way ${Math.min(...late).toFixed(1)}..${Math.max(...late).toFixed(1)} ms`);
  }
}
