// "Line up" for friends (3.11.1 #4): three friends arrive late by different amounts; the Line up
// switch slides on and all three land on the beat. Loops only while it is on screen; lazy loaded.
import { useEffect, useRef, useState } from 'react';
import { motion, useInView, useReducedMotion } from 'motion/react';
import { useT } from '../i18n';

const FRIENDS = [
  { id: 'M', late: 46 },
  { id: 'B', late: 18 },
  { id: 'F', late: 70 },
];
const BEAT_X = 120;

export default function LineUp() {
  const { t } = useT();
  const ref = useRef<HTMLDivElement>(null);
  const inView = useInView(ref, { margin: '-10% 0px' });
  const reduce = useReducedMotion();
  const [on, setOn] = useState(false);

  useEffect(() => {
    if (!inView || reduce) { setOn(!!reduce); return; }
    const id = window.setInterval(() => setOn(v => !v), 2600);
    setOn(false);
    return () => window.clearInterval(id);
  }, [inView, reduce]);

  return (
    <div ref={ref} className="card lineup" aria-hidden="true">
      <div className="lineup-head">
        <span>{t('home.friends.lineUp')}</span>
        <span className={`sw ${on ? 'on' : ''}`}><motion.i animate={{ x: on ? 18 : 0 }} transition={{ type: 'spring', stiffness: 500, damping: 32 }} /></span>
      </div>
      <div className="lineup-lanes">
        <div className="beat" style={{ left: BEAT_X }}><span>{t('home.friends.beat')}</span></div>
        {FRIENDS.map((f, i) => (
          <div className="lane" key={f.id}>
            <motion.span className="avatar" style={{ width: 30, height: 30, fontSize: 13 }}
                         animate={{ x: BEAT_X - 15 + (on ? 0 : f.late) }}
                         transition={{ type: 'spring', stiffness: 120, damping: 16, delay: on ? i * 0.08 : 0 }}>
              {f.id}
            </motion.span>
            <span className="lane-wave" />
          </div>
        ))}
      </div>
    </div>
  );
}
