import assert from 'node:assert/strict';
import dgram from 'node:dgram';
import { setTimeout as sleep } from 'node:timers/promises';
import { candidate, relay } from './netsim-udp-compare.mjs';

const makeSocket = async () => {
  const socket = dgram.createSocket('udp4');
  await new Promise(resolve => socket.bind(0, '127.0.0.1', resolve));
  return socket;
};
const a = await makeSocket(), b = await makeSocket();
const proxy = await relay({ delay: 1, jitter: 0, loss: 1 }, 7);
try {
  const line = port => `candidate:1 1 udp 123 127.0.0.1 ${port} typ host`;
  assert.equal(candidate('candidate:1 1 tcp 123 127.0.0.1 200 typ host', proxy.port), null);
  assert.equal(candidate('candidate:1 1 udp 123 example.local 200 typ host', proxy.port), null);
  assert.deepEqual(candidate('candidate:1 1 udp 123 203.0.113.1 47841 typ srflx raddr 0.0.0.0 rport 0', proxy.port, 'hub').endpoint, { address: '127.0.0.1', port: 47841 });
  assert.equal(candidate('candidate:1 1 udp 123 203.0.113.1 47841 typ srflx raddr 0.0.0.0 rport 0', proxy.port), null);
  assert.equal(proxy.map('browser', line(a.address().port)), line(proxy.port));
  assert.equal(proxy.map('hub', line(b.address().port)), line(proxy.port));
  assert.equal(proxy.map('hub', line(a.address().port)), null);
  const message = new Promise(resolve => b.once('message', resolve));
  a.send(Buffer.from('hello'), proxy.port, '127.0.0.1');
  assert.equal((await Promise.race([message, sleep(2000).then(() => { throw new Error('relay timed out'); })])).toString(), 'hello');
  proxy.enable();
  a.send(Buffer.from('lost'), proxy.port, '127.0.0.1');
  await sleep(30);
  assert.equal(proxy.counts.received, 1);
  assert.equal(proxy.counts.dropped, 1);
  assert.equal(proxy.counts.forwarded, 0);
} finally { proxy.close(); a.close(); b.close(); }
const c = await makeSocket(), d = await makeSocket();
const noisy = await relay({ delay: 20, jitter: 20, loss: 0.1 }, 7);
try {
  const line = port => `candidate:1 1 udp 123 127.0.0.1 ${port} typ host`;
  noisy.map('browser', line(c.address().port)); noisy.map('hub', line(d.address().port));
  const received = []; d.on('message', packet => received.push(packet[0]));
  noisy.enable();
  for (let i = 0; i < 100; i++) c.send(Buffer.from([i]), noisy.port, '127.0.0.1');
  await sleep(200);
  assert.equal(noisy.counts.received, 100);
  assert.ok(noisy.counts.dropped > 0 && noisy.counts.reordered > 0);
  assert.equal(received.length, noisy.counts.forwarded);
  assert.ok(received.some((n, i) => i && n < received[i - 1]), 'the destination observes reordering');
} finally { noisy.close(); c.close(); d.close(); }
const routes = await relay({ delay: 0, jitter: 0, loss: 0 }, 7);
try {
  routes.map('hub', 'candidate:1 1 UDP 123 203.0.113.1 47841 typ srflx raddr 0.0.0.0 rport 0');
  routes.map('browser', 'candidate:1 1 udp 123 192.0.2.1 50001 typ host');
  assert.equal(routes.peers.hub.address, '192.0.2.1');
  assert.equal(routes.map('browser', 'candidate:2 1 udp 123 192.0.2.2 50002 typ host'), null);
  assert.equal(routes.peers.hub.address, '192.0.2.1', 'rejected candidates do not redirect the Hub endpoint');
} finally { routes.close(); }
console.log('UDP candidate filtering, relay and loss passed');
