// Small building blocks with the plug-ins' look (3.2): AudibleToggle, Switch, Field, SubmitButton,
// Avatar, Screenshot, Toasts. Colours always come from the tokens through app.css.
import { createContext, forwardRef, useCallback, useContext, useEffect, useRef, useState, type InputHTMLAttributes, type ReactNode } from 'react';
import { AnimatePresence, motion, useReducedMotion } from 'motion/react';
import { BroadcastIcon, HeadphonesIcon } from './Icons';

export function AudibleToggle({ kind, on, onChange, label, title }: {
  kind: 'you' | 'viewers'; on: boolean; onChange(v: boolean): void; label: string; title: string;
}) {
  const Icon = kind === 'you' ? HeadphonesIcon : BroadcastIcon;
  return (
    <motion.button type="button" className={`tg ${on ? 'on' : ''}`} aria-label={label} aria-pressed={on} title={title}
                   whileTap={{ scale: 0.94 }} onClick={() => onChange(!on)}>
      <Icon off={!on} />
    </motion.button>
  );
}

export function Switch({ on, onChange, label, disabled }: { on: boolean; onChange(v: boolean): void; label: string; disabled?: boolean }) {
  return (
    <button type="button" role="switch" aria-checked={on} aria-label={label} disabled={disabled}
            className={`sw ${on ? 'on' : ''}`} onClick={() => onChange(!on)}>
      <motion.i animate={{ x: on ? 18 : 0 }} transition={{ type: 'spring', stiffness: 500, damping: 32 }} />
    </button>
  );
}

/** A labelled input; `error` shows under it in red (role=alert) and shakes the field once. */
export const Field = forwardRef<HTMLInputElement, InputHTMLAttributes<HTMLInputElement> & {
  label: string; hint?: ReactNode; error?: string | null; right?: ReactNode; prefix?: string;
}>(function Field({ label, hint, error, right, prefix, id, ...input }, ref) {
  const [shake, setShake] = useState(false);
  const fid = id ?? `f-${input.name}`;
  useEffect(() => {
    if (!error) return;
    setShake(true);
    const t = window.setTimeout(() => setShake(false), 320);
    return () => window.clearTimeout(t);
  }, [error]);
  return (
    <div className="lbl">
      <span className="flex items-center justify-between gap-3">
        <label htmlFor={fid}>{label}</label>
        {right}
      </span>
      <span className={`inp-wrap ${shake ? 'shake' : ''}`}>
        {prefix && <span className="inp-prefix" aria-hidden="true">{prefix}</span>}
        <input ref={ref} id={fid} className={`inp ${prefix ? 'has-prefix' : ''}`} aria-invalid={error ? true : undefined}
               aria-describedby={error || hint ? `${fid}-d` : undefined} {...input} />
      </span>
      {error ? <span id={`${fid}-d`} role="alert" className="field-err">{error}</span>
             : hint ? <span id={`${fid}-d`} className="field-hint">{hint}</span> : null}
    </div>
  );
});

export function SubmitButton({ busy, children, className = 'solid', disabled, onClick, type = 'submit' }: {
  busy?: boolean; children: ReactNode; className?: string; disabled?: boolean; onClick?(): void; type?: 'submit' | 'button';
}) {
  return (
    <button type={type} className={`btn ${className}`} disabled={disabled || busy} aria-busy={busy || undefined} onClick={onClick}>
      {busy && <span className="spin" aria-hidden="true" />}
      {children}
    </button>
  );
}

export function Avatar({ name, url, size = 40 }: { name: string; url?: string | null; size?: number }) {
  const initial = Array.from(name.trim())[0]?.toUpperCase() ?? '?';
  return (
    <span className="avatar" style={{ width: size, height: size, fontSize: Math.round(size * 0.42) }} aria-hidden="true">
      {url ? <img src={url} alt="" width={size} height={size} /> : initial}
    </span>
  );
}

/** A plug-in screenshot from ui-snapshot in the page's language, light and dark (CSS picks one). */
export function Screenshot({ name, lang, alt, eager = false, className = 'shot', width, height }: {
  name: string; lang: 'en' | 'th'; alt: string; eager?: boolean; className?: string; width: number; height: number;
}) {
  const load = eager ? 'eager' : 'lazy';
  return (
    <>
      <img className={`${className} only-light`} src={`/screens/${name}-${lang}-light.webp`} alt={alt} width={width} height={height}
           loading={load} decoding="async" {...(eager ? { fetchPriority: 'high' as const } : {})} />
      <img className={`${className} only-dark`} src={`/screens/${name}-${lang}-dark.webp`} alt={alt} width={width} height={height}
           loading="lazy" decoding="async" />
    </>
  );
}

// --- toasts: "Saved" pops up from the bottom (3.11.1 #9) ------------------------------------------

const ToastCtx = createContext<(text: string) => void>(() => {});

export function ToastProvider({ children }: { children: ReactNode }) {
  const [msg, setMsg] = useState<{ id: number; text: string } | null>(null);
  const timer = useRef(0);
  const reduce = useReducedMotion();
  const show = useCallback((text: string) => {
    window.clearTimeout(timer.current);
    setMsg({ id: Date.now(), text });
    timer.current = window.setTimeout(() => setMsg(null), 1800);
  }, []);
  return (
    <ToastCtx.Provider value={show}>
      {children}
      <div aria-live="polite" className="sr-only">{msg?.text}</div>
      <AnimatePresence>
        {msg && (
          <motion.div key={msg.id} className="toast" aria-hidden="true"
                      initial={reduce ? { opacity: 0, x: '-50%' } : { opacity: 0, y: 24, x: '-50%' }}
                      animate={{ opacity: 1, y: 0, x: '-50%' }}
                      exit={{ opacity: 0, y: reduce ? 0 : 12, x: '-50%' }}
                      transition={{ type: 'spring', stiffness: 420, damping: 30 }}>
            {msg.text}
          </motion.div>
        )}
      </AnimatePresence>
    </ToastCtx.Provider>
  );
}

export const useToast = () => useContext(ToastCtx);

export function Wordmark() {
  return <span className="wordmark">HEARASIDE</span>;
}

export function Check({ size = 18 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={2} strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <path d="M5 12.5l4.5 4.5L19 7.5" />
    </svg>
  );
}
