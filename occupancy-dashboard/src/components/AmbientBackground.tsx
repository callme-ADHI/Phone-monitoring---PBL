'use client';

import { motion } from 'framer-motion';

const BLOBS = [
  { w: 680, h: 560, l: '-8%',  t: '-8%', x: [0,200, 60, 0], y: [0,140,260, 0], color: 'rgba(34,211,238,0.08)',   dur: 30 },
  { w: 520, h: 520, l: '42%',  t: '-12%',x: [0,-140,80, 0], y: [0,200, 90, 0], color: 'rgba(99,102,241,0.08)',   dur: 36 },
  { w: 620, h: 560, l: '58%',  t: '38%', x: [0,-180,50, 0], y: [0,-130,90, 0], color: 'rgba(139,92,246,0.06)',   dur: 33 },
  { w: 400, h: 400, l: '18%',  t: '58%', x: [0, 120,-70, 0],y: [0,-90, 50, 0], color: 'rgba(251,191,36,0.05)',   dur: 24 },
] as const;

export function AmbientBackground() {
  return (
    <div className="fixed inset-0 overflow-hidden pointer-events-none" aria-hidden="true">
      {BLOBS.map((b, i) => (
        <motion.div
          key={i}
          className="absolute rounded-full blur-3xl will-change-transform"
          style={{ width: b.w, height: b.h, left: b.l, top: b.t, background: b.color }}
          animate={{ x: b.x, y: b.y }}
          transition={{ duration: b.dur, repeat: Infinity, repeatType: 'mirror', ease: 'easeInOut' }}
        />
      ))}

      {/* Subtle dot-grid texture */}
      <div
        className="absolute inset-0 opacity-[0.03]"
        style={{
          backgroundImage: 'radial-gradient(circle, rgba(255,255,255,0.8) 1px, transparent 1px)',
          backgroundSize: '32px 32px',
        }}
      />

      {/* Edge vignette so blobs don't fight the content */}
      <div className="absolute inset-0 bg-[radial-gradient(ellipse_80%_80%_at_50%_50%,transparent_40%,rgba(7,7,15,0.7)_100%)]" />
    </div>
  );
}
