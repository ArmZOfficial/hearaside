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

console.log(failures === 0 ? 'worklet: all passed' : 'worklet: FAILED');
process.exit(failures === 0 ? 0 : 1);
