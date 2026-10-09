// "Play a demo" (3.11.1 #2): a short made-up song synthesised in the browser, one voice per demo
// track, so the switches can be heard. Starts only when the visitor presses the button; about
// 16 seconds; no audio files to download.
type Ids = 'backing' | 'vocal' | 'click' | 'talk' | 'friend';

const BPM = 96;
const BEAT = 60 / BPM;
const BARS = 6;

const note = (n: number) => 440 * Math.pow(2, (n - 69) / 12);
const CHORDS = [[57, 60, 64], [53, 57, 60], [48, 52, 55], [55, 59, 62]];       // Am F C G
const MELODY = [69, 72, 71, 69, 67, 69, 64, 0, 65, 69, 67, 65, 64, 62, 60, 0];   // two bars, eighths... in quarters
const HARMONY = MELODY.map(n => (n ? n - 3 : 0));

export function startDemo(on: Record<string, boolean>, onEnd: () => void) {
  const Ctx = window.AudioContext || (window as unknown as { webkitAudioContext: typeof AudioContext }).webkitAudioContext;
  const ctx = new Ctx();
  const master = ctx.createGain();
  master.gain.value = 0.22;
  master.connect(ctx.destination);
  const buses = {} as Record<Ids, GainNode>;
  for (const id of ['backing', 'vocal', 'click', 'talk', 'friend'] as Ids[]) {
    const g = ctx.createGain();
    g.gain.value = on[id] ? 1 : 0;
    g.connect(master);
    buses[id] = g;
  }
  const t0 = ctx.currentTime + 0.08;

  const tone = (bus: GainNode, type: OscillatorType, freq: number, at: number, dur: number, vol: number, vibrato = 0) => {
    const o = ctx.createOscillator();
    const g = ctx.createGain();
    o.type = type;
    o.frequency.value = freq;
    if (vibrato) {
      const lfo = ctx.createOscillator();
      const lg = ctx.createGain();
      lfo.frequency.value = 5.2;
      lg.gain.value = vibrato;
      lfo.connect(lg).connect(o.frequency);
      lfo.start(at); lfo.stop(at + dur + 0.05);
    }
    g.gain.setValueAtTime(0, at);
    g.gain.linearRampToValueAtTime(vol, at + 0.02);
    g.gain.setTargetAtTime(0, at + dur * 0.8, dur * 0.15);
    o.connect(g).connect(bus);
    o.start(at); o.stop(at + dur + 0.2);
  };

  const noise = ctx.createBuffer(1, ctx.sampleRate * 0.4, ctx.sampleRate);
  const d = noise.getChannelData(0);
  for (let i = 0; i < d.length; i++) d[i] = Math.random() * 2 - 1;
  const talkBurst = (at: number, dur: number) => {
    const src = ctx.createBufferSource();
    src.buffer = noise;
    const bp = ctx.createBiquadFilter();
    bp.type = 'bandpass'; bp.frequency.value = 900 + Math.random() * 700; bp.Q.value = 2.5;
    const g = ctx.createGain();
    g.gain.setValueAtTime(0, at); g.gain.linearRampToValueAtTime(0.5, at + 0.03); g.gain.setTargetAtTime(0, at + dur, 0.04);
    src.connect(bp).connect(g).connect(buses.talk);
    src.start(at); src.stop(at + dur + 0.2);
  };

  for (let bar = 0; bar < BARS; bar++) {
    const chord = CHORDS[bar % CHORDS.length];
    const at = t0 + bar * 4 * BEAT;
    for (const n of chord) tone(buses.backing, 'triangle', note(n), at, 4 * BEAT, 0.12);
    for (let b = 0; b < 4; b++) {
      tone(buses.backing, 'sine', note(chord[0] - 12), at + b * BEAT, BEAT * 0.9, 0.3);
      tone(buses.click, 'square', b === 0 ? 1760 : 1320, at + b * BEAT, 0.03, 0.25);
    }
    if (bar % 2 === 1) for (let k = 0; k < 3; k++) talkBurst(at + 0.4 + k * 0.32, 0.18 + Math.random() * 0.1);
  }
  MELODY.forEach((n, i) => {
    if (!n) return;
    const at = t0 + (8 + i * 1) * BEAT;           // the singers come in on bar 3
    tone(buses.vocal, 'sine', note(n), at, BEAT * 0.95, 0.32, 4);
    if (HARMONY[i]) tone(buses.friend, 'triangle', note(HARMONY[i]), at, BEAT * 0.95, 0.16, 3);
  });

  const total = BARS * 4 * BEAT;
  const timer = window.setTimeout(() => { stop(); onEnd(); }, (total + 0.6) * 1000);
  let stopped = false;
  function stop() {
    if (stopped) return;
    stopped = true;
    window.clearTimeout(timer);
    master.gain.setTargetAtTime(0, ctx.currentTime, 0.05);
    window.setTimeout(() => ctx.close(), 300);
  }
  return {
    stop,
    set(next: Record<string, boolean>) {
      for (const id of Object.keys(buses) as Ids[]) buses[id].gain.setTargetAtTime(next[id] ? 1 : 0, ctx.currentTime, 0.03);
    },
  };
}
