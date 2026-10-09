// A faint light that follows the mouse over the background grid (3.11.1 #12).
// Lazy loaded after the page; off for touch screens and reduced motion.
import { useEffect, useRef } from 'react';

export default function CursorGlow() {
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const fine = window.matchMedia('(pointer: fine)').matches;
    const reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    if (!fine || reduce) return;
    let raf = 0;
    let x = -999, y = -999;
    const paint = () => {
      raf = 0;
      ref.current?.style.setProperty('--gx', `${x}px`);
      ref.current?.style.setProperty('--gy', `${y}px`);
    };
    const move = (e: PointerEvent) => {
      x = e.clientX; y = e.clientY;
      if (!raf) raf = requestAnimationFrame(paint);
    };
    window.addEventListener('pointermove', move, { passive: true });
    return () => { window.removeEventListener('pointermove', move); cancelAnimationFrame(raf); };
  }, []);
  return <div ref={ref} className="glow" aria-hidden="true" />;
}
