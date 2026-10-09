// Scroll reveal (3.11.1 #3): rises 16 px and fades in the first time it comes into view.
// Prerendered HTML stays visible (no JavaScript = no hidden content); only things still below the
// fold when the page wakes up get hidden and revealed, so nothing on screen flickers.
import { useEffect, useRef, type ElementType, type ReactNode, type CSSProperties } from 'react';
import { useReducedMotion, useScroll, useTransform, motion } from 'motion/react';

export function Reveal({ as: Tag = 'div', delay = 0, className, style, children, id, ...rest }: {
  as?: ElementType; delay?: number; className?: string; style?: CSSProperties; children: ReactNode; id?: string;
  'aria-labelledby'?: string; 'aria-label'?: string;
}) {
  const ref = useRef<HTMLElement>(null);
  useEffect(() => {
    const el = ref.current;
    if (!el || typeof IntersectionObserver === 'undefined') return;
    if (el.getBoundingClientRect().top < window.innerHeight * 0.95) return;   // already on screen
    el.classList.add('rv-hide');
    el.style.transitionDelay = `${delay}ms`;
    const io = new IntersectionObserver(entries => {
      if (entries.some(e => e.isIntersecting)) {
        el.classList.remove('rv-hide');
        io.disconnect();
      }
    }, { rootMargin: '0px 0px -8% 0px' });
    io.observe(el);
    return () => io.disconnect();
  }, [delay]);
  return <Tag ref={ref} className={`rv ${className ?? ''}`} style={style} id={id} {...rest}>{children}</Tag>;
}

/** Gentle parallax for screenshots (≤ 24 px), off with reduced motion. */
export function Parallax({ children, range = 24 }: { children: ReactNode; range?: number }) {
  const ref = useRef<HTMLDivElement>(null);
  const reduce = useReducedMotion();
  const { scrollYProgress } = useScroll({ target: ref, offset: ['start end', 'end start'] });
  const y = useTransform(scrollYProgress, [0, 1], [range, -range]);
  return (
    <motion.div ref={ref} style={reduce ? undefined : { y }}>
      {children}
    </motion.div>
  );
}
