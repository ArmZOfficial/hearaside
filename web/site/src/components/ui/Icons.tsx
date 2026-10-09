// The plug-ins' own stroke icons (design export, 24 x 24, stroke 1.8), so the site and the
// plug-ins look the same. Generic icons come from lucide-react (also 2 px strokes).
import type { SVGProps } from 'react';
import { motion } from 'motion/react';

type P = SVGProps<SVGSVGElement> & { size?: number };

function Svg({ size = 18, children, ...rest }: P) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8}
         strokeLinecap="round" strokeLinejoin="round" aria-hidden="true" {...rest}>
      {children}
    </svg>
  );
}

/** The struck-through line of an "off" toggle; draws itself in (pathLength) when it appears. */
function Strike({ on }: { on: boolean }) {
  return (
    <motion.path d="M3 3l18 18" initial={false} animate={{ pathLength: on ? 0 : 1, opacity: on ? 0 : 1 }}
                 transition={{ duration: 0.18, ease: 'easeOut' }} />
  );
}

export function HeadphonesIcon({ off = false, ...p }: P & { off?: boolean }) {
  return (
    <Svg {...p}>
      <path d="M4 17v-5a8 8 0 0 1 16 0v5" />
      <rect x="3" y="14" width="4.5" height="7" rx="1.8" />
      <rect x="16.5" y="14" width="4.5" height="7" rx="1.8" />
      <Strike on={!off} />
    </Svg>
  );
}

export function BroadcastIcon({ off = false, ...p }: P & { off?: boolean }) {
  return (
    <Svg {...p}>
      <circle cx="12" cy="12" r="2" />
      <path d="M15.5 8.5a5 5 0 0 1 0 7M8.5 15.5a5 5 0 0 1 0-7M18.4 5.6a9 9 0 0 1 0 12.8M5.6 18.4a9 9 0 0 1 0-12.8" />
      <Strike on={!off} />
    </Svg>
  );
}

export function WindowsIcon(p: P) {
  return (
    <Svg {...p}>
      <path d="M3 5.5 10.5 4.5v7H3zM13 4.2 21 3v8.5h-8zM3 13h7.5v7L3 19zM13 13h8v8l-8-1.2z" fill="currentColor" stroke="none" />
    </Svg>
  );
}

export function AppleIcon(p: P) {
  return (
    <Svg {...p}>
      <path d="M16.5 12.6c0-2.4 2-3.5 2.1-3.6-1.1-1.7-2.9-1.9-3.5-1.9-1.5-.2-2.9.9-3.7.9-.8 0-1.9-.9-3.2-.8-1.6 0-3.2 1-4 2.5-1.7 3-.4 7.4 1.2 9.8.8 1.2 1.8 2.5 3 2.4 1.2 0 1.7-.8 3.2-.8s1.9.8 3.2.7c1.3 0 2.2-1.2 3-2.4.9-1.4 1.3-2.7 1.3-2.8 0 0-2.6-1-2.6-4z" />
      <path d="M14.2 5.4c.7-.8 1.1-1.9 1-3-1 .1-2.1.7-2.8 1.5-.6.7-1.2 1.8-1 2.9 1.1.1 2.1-.6 2.8-1.4z" />
    </Svg>
  );
}

export function DiscordLogo({ size = 20 }: { size?: number }) {
  // Discord's mark (logo file supplied with the design), one colour: follows the button text colour
  return (
    <svg width={size} height={size} viewBox="0 0 48 48" aria-hidden="true" fill="none" stroke="currentColor" strokeWidth={3} strokeLinecap="round" strokeLinejoin="round">
      <path d="M17.59,34.1733c-.89,1.3069-1.8944,2.6152-2.91,3.8267C7.3,37.79,4.5,33,4.5,33A44.83,44.83,0,0,1,9.31,13.48,16.47,16.47,0,0,1,18.69,10l1,2.31A32.6875,32.6875,0,0,1,24,12a32.9643,32.9643,0,0,1,4.33.3l1-2.31a16.47,16.47,0,0,1,9.38,3.51A44.8292,44.8292,0,0,1,43.5,33s-2.8,4.79-10.18,5a47.4193,47.4193,0,0,1-2.86-3.81m6.46-2.9c-3.84,1.9454-7.5555,3.89-12.92,3.89s-9.08-1.9446-12.92-3.89" />
      <circle cx="17.847" cy="26.23" r="3.35" />
      <circle cx="30.153" cy="26.23" r="3.35" />
    </svg>
  );
}

export function GoogleLogo({ size = 20 }: { size?: number }) {
  // only used when the Google Identity Services button can't load (it draws its own logo)
  return <img src="/logos/google.svg" width={size} height={size} alt="" aria-hidden="true" style={{ display: 'block' }} />;
}
