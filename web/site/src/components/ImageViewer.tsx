import { createContext, useContext, useEffect, useRef, useState, type PointerEvent, type ReactNode } from 'react';
import { Minus, Plus, X } from 'lucide-react';
import { useT } from '../i18n';
import { useLocation } from 'react-router';

interface Picture { src: string; alt: string; width: number; height: number }
interface View { scale: number; x: number; y: number }
const ViewerContext = createContext<((picture: Picture) => void) | null>(null);

export function useImageViewer() {
  const open = useContext(ViewerContext);
  if (!open) throw new Error('useImageViewer requires ImageViewerProvider');
  return open;
}

export function ImageViewerProvider({ children }: { children: ReactNode }) {
  const [picture, setPicture] = useState<Picture | null>(null);
  const { pathname } = useLocation();
  useEffect(() => setPicture(null), [pathname]);
  return <ViewerContext.Provider value={setPicture}>{children}{picture && <ImageViewer picture={picture} close={() => setPicture(null)} />}</ViewerContext.Provider>;
}

function ImageViewer({ picture, close }: { picture: Picture; close(): void }) {
  const { t } = useT();
  const dialog = useRef<HTMLDialogElement>(null);
  const stage = useRef<HTMLDivElement>(null);
  const pointers = useRef(new Map<number, { x: number; y: number }>());
  const [fit, setFit] = useState(1);
  const [view, setView] = useState<View>({ scale: 1, x: 0, y: 0 });
  const [failed, setFailed] = useState(false);

  function constrain(next: View): View {
    const bounds = stage.current?.getBoundingClientRect();
    if (!bounds) return next;
    const maxX = Math.max(0, (picture.width * next.scale - bounds.width) / 2);
    const maxY = Math.max(0, (picture.height * next.scale - bounds.height) / 2);
    return { scale: next.scale, x: Math.max(-maxX, Math.min(maxX, next.x)), y: Math.max(-maxY, Math.min(maxY, next.y)) };
  }

  function zoom(scale: number, point?: { x: number; y: number }) {
    setView(previous => {
      const next = Math.max(fit, Math.min(4, scale));
      const ratio = next / previous.scale;
      const x = point?.x ?? 0, y = point?.y ?? 0;
      return constrain({ scale: next, x: x - (x - previous.x) * ratio, y: y - (y - previous.y) * ratio });
    });
  }

  useEffect(() => {
    const node = dialog.current!;
    const previousFocus = document.activeElement as HTMLElement | null;
    const previousOverflow = document.body.style.overflow;
    document.body.style.overflow = 'hidden';
    node.showModal();
    const resize = () => {
      const bounds = stage.current!.getBoundingClientRect();
      const scale = Math.max(.01, Math.min(1, (bounds.width - 24) / picture.width, (bounds.height - 24) / picture.height));
      setFit(scale); setView({ scale, x: 0, y: 0 });
    };
    resize();
    const observer = new ResizeObserver(resize);
    observer.observe(stage.current!);
    return () => {
      observer.disconnect(); node.close();
      document.body.style.overflow = previousOverflow;
      if (previousFocus?.isConnected) previousFocus.focus();
    };
  }, [picture]);

  useEffect(() => {
    const element = stage.current!;
    const wheel = (event: WheelEvent) => {
      event.preventDefault();
      const bounds = element.getBoundingClientRect();
      const x = event.clientX - bounds.left - bounds.width / 2;
      const y = event.clientY - bounds.top - bounds.height / 2;
      setView(previous => {
        const scale = Math.max(fit, Math.min(4, previous.scale * Math.exp(-event.deltaY * .002)));
        const ratio = scale / previous.scale;
        return constrain({ scale, x: x - (x - previous.x) * ratio, y: y - (y - previous.y) * ratio });
      });
    };
    element.addEventListener('wheel', wheel, { passive: false });
    return () => element.removeEventListener('wheel', wheel);
  }, [fit, picture]);

  const move = (event: PointerEvent<HTMLDivElement>) => {
    if (!pointers.current.has(event.pointerId)) return;
    const before = [...pointers.current.values()];
    const previous = pointers.current.get(event.pointerId)!;
    pointers.current.set(event.pointerId, { x: event.clientX, y: event.clientY });
    const after = [...pointers.current.values()];
    if (after.length === 1) {
      setView(current => constrain({ ...current, x: current.x + event.clientX - previous.x, y: current.y + event.clientY - previous.y }));
    } else if (after.length === 2) {
      const distance = (points: typeof before) => Math.hypot(points[0].x - points[1].x, points[0].y - points[1].y);
      const oldDistance = distance(before);
      if (!oldDistance) return;
      const bounds = stage.current!.getBoundingClientRect();
      const center = (points: typeof before) => ({ x: (points[0].x + points[1].x) / 2 - bounds.left - bounds.width / 2, y: (points[0].y + points[1].y) / 2 - bounds.top - bounds.height / 2 });
      const a = center(before), b = center(after);
      setView(current => {
        const scale = Math.max(fit, Math.min(4, current.scale * distance(after) / oldDistance));
        const ratio = scale / current.scale;
        return constrain({ scale, x: b.x - (a.x - current.x) * ratio, y: b.y - (a.y - current.y) * ratio });
      });
    }
  };

  return <dialog ref={dialog} className="image-viewer" aria-labelledby="viewer-title" onCancel={event => { event.preventDefault(); close(); }} onClick={event => { if (event.target === event.currentTarget) close(); }}
    onKeyDown={event => {
      if (['+', '=', '-', '0', '1', 'ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'].includes(event.key)) event.preventDefault();
      if (event.key === '+' || event.key === '=') zoom(view.scale * 1.5);
      if (event.key === '-') zoom(view.scale / 1.5);
      if (event.key === '0') setView({ scale: fit, x: 0, y: 0 });
      if (event.key === '1') zoom(1);
      const delta: Record<string, [number, number]> = { ArrowLeft: [40, 0], ArrowRight: [-40, 0], ArrowUp: [0, 40], ArrowDown: [0, -40] };
      if (delta[event.key]) setView(current => constrain({ ...current, x: current.x + delta[event.key][0], y: current.y + delta[event.key][1] }));
    }}>
    <div className="viewer-header"><h2 id="viewer-title">{t('viewer.title')}</h2><button type="button" className="iconbtn" autoFocus aria-label={t('viewer.close')} onClick={close}><X size={20} aria-hidden="true" /></button></div>
    <div className="viewer-controls" role="group" aria-label={t('viewer.controls')}>
      <button type="button" className="iconbtn" aria-label={t('viewer.out')} title={t('viewer.out')} disabled={view.scale <= fit + .001} onClick={() => zoom(view.scale / 1.5)}><Minus size={18} aria-hidden="true" /></button>
      <output className="viewer-percent" aria-live="polite">{Math.round(view.scale * 100)}%</output>
      <button type="button" className="iconbtn" aria-label={t('viewer.in')} title={t('viewer.in')} disabled={view.scale >= 4} onClick={() => zoom(view.scale * 1.5)}><Plus size={18} aria-hidden="true" /></button>
      <span className="viewer-separator" />
      <button type="button" className="btn small" onClick={() => { zoom(1); }}>{t('viewer.actual')}</button>
      <button type="button" className="btn small" onClick={() => setView({ scale: fit, x: 0, y: 0 })}>{t('viewer.fit')}</button>
    </div>
    <div ref={stage} className={`viewer-stage ${view.scale > fit + .001 ? 'can-pan' : ''}`} tabIndex={0} role="region" aria-label={t('viewer.hint')}
      onDoubleClick={() => zoom(Math.abs(view.scale - fit) < .01 ? Math.max(1, fit * 2) : fit)}
      onPointerDown={event => { if (event.button !== 0 || pointers.current.size >= 2) return; event.currentTarget.setPointerCapture(event.pointerId); pointers.current.set(event.pointerId, { x: event.clientX, y: event.clientY }); }}
      onPointerMove={move} onPointerUp={event => pointers.current.delete(event.pointerId)} onPointerCancel={event => pointers.current.delete(event.pointerId)} onLostPointerCapture={event => pointers.current.delete(event.pointerId)}>
      {failed ? <p className="alert" role="alert">{t('viewer.error')}</p> : <img src={picture.src} alt={picture.alt} draggable={false} width={picture.width} height={picture.height} onError={() => setFailed(true)}
        style={{ width: picture.width * view.scale, height: picture.height * view.scale, transform: `translate(calc(-50% + ${view.x}px), calc(-50% + ${view.y}px))` }} />}
    </div>
    <p className="viewer-caption">{picture.alt}<span>{t('viewer.hint')}</span></p>
  </dialog>;
}
