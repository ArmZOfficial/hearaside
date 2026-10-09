// Numbers count up when they come into view (3.11.1 #5). The prerendered HTML already holds the
// final number, so search engines and reduced motion see the real value.
import { useEffect, useRef, useState } from 'react';
import { useInView, useReducedMotion } from 'motion/react';

export function CountUp({ to, decimals = 0, ms = 900 }: { to: number; decimals?: number; ms?: number }) {
  const ref = useRef<HTMLSpanElement>(null);
  const inView = useInView(ref, { once: true });
  const reduce = useReducedMotion();
  const [v, setV] = useState(to);
  const started = useRef(false);

  useEffect(() => {
    if (!inView || reduce || started.current) return;
    started.current = true;
    const t0 = performance.now();
    let raf = 0;
    const step = (now: number) => {
      const p = Math.min(1, (now - t0) / ms);
      setV(to * (1 - Math.pow(1 - p, 3)));
      if (p < 1) raf = requestAnimationFrame(step);
    };
    setV(0);
    raf = requestAnimationFrame(step);
    return () => cancelAnimationFrame(raf);
  }, [inView, reduce, to, ms]);

  return <span ref={ref} style={{ fontVariantNumeric: 'tabular-nums' }}>{v.toFixed(decimals)}</span>;
}
