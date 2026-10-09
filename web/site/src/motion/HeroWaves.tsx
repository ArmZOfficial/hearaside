// Two-way sound under the headline (3.11.1 #1): one waveform flows in from the left and splits
// into "You" and "Viewers". Pure SVG + a CSS animation (stroke dash offset), so it runs before
// the page's JavaScript and costs nothing to hydrate; reduced motion = a still drawing.
import { useT } from '../i18n';

function wave(x0: number, x1: number, y: number, amp: number, step = 8) {
  let d = `M${x0} ${y}`;
  for (let x = x0, i = 0; x < x1; x += step, i++) {
    const a = amp * (0.55 + 0.45 * Math.sin(i * 0.9)) * (i % 2 ? -1 : 1);
    d += ` Q${x + step / 2} ${y + a} ${x + step} ${y}`;
  }
  return d;
}

export function HeroWaves() {
  const { t } = useT();
  const inbound = wave(0, 176, 46, 14);
  const you = `M176 46 C 214 46, 222 20, 262 20 ${wave(262, 470, 20, 9).slice(`M262 20`.length)}`;
  const viewers = `M176 46 C 214 46, 222 72, 262 72 ${wave(262, 470, 72, 9).slice(`M262 72`.length)}`;
  return (
    <svg className="hero-waves" viewBox="0 0 560 92" width="100%" height="92" role="img"
         aria-label={`${t('home.wave.you')} / ${t('home.wave.viewers')}`}>
      <path d={inbound} className="w w-in" />
      <path d={you} className="w w-you" />
      <path d={viewers} className="w w-view" />
      <text x="482" y="24" className="wl">{t('home.wave.you')}</text>
      <text x="482" y="76" className="wl">{t('home.wave.viewers')}</text>
    </svg>
  );
}
