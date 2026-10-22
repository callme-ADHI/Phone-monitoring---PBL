'use client';

import { useEffect } from 'react';
import { motion, useMotionValue, useTransform, animate } from 'framer-motion';
import type { OccupancyTier } from '@/types/occupancy';
import { tierConfig } from '@/lib/tiers';

interface Props {
  count: number;
  capacity: number;
  tier: OccupancyTier;
}

export function HeroCount({ count, capacity, tier }: Props) {
  const cfg = tierConfig[tier];

  // Spring-animated floating-point counter
  const motionVal = useMotionValue(count);
  const displayVal = useTransform(motionVal, (v) => String(Math.round(v)));

  useEffect(() => {
    const ctrl = animate(motionVal, count, {
      type: 'spring',
      stiffness: 65,
      damping: 18,
      mass: 1.1,
    });
    return ctrl.stop;
  }, [count, motionVal]);

  const pct = Math.min((count / capacity) * 100, 100);

  return (
    <div className="flex flex-col items-center gap-5">
      {/* Number + glow halo */}
      <div className="relative flex items-center justify-center">
        {/* Glow: a blurred coloured blob positioned behind the digit(s) */}
        <motion.div
          className="absolute rounded-full blur-[90px] -z-10 w-full h-full scale-150"
          animate={{ backgroundColor: cfg.color, opacity: 0.15 }}
          transition={{ duration: 1, ease: 'easeInOut' }}
        />

        {/* The main digit — color animated by Framer Motion, text updated by MotionValue */}
        <motion.span
          className="tabular-hero block font-black leading-none select-none
                     text-[8rem] sm:text-[11rem] lg:text-[14rem]"
          animate={{ color: cfg.color }}
          transition={{ duration: 0.8, ease: 'easeInOut' }}
        >
          {displayVal}
        </motion.span>
      </div>

      {/* Capacity label */}
      <p className="text-xs text-white/25 tracking-widest uppercase">
        of{' '}
        <span className="text-white/50 font-semibold">{capacity}</span> capacity
      </p>

      {/* Thin occupancy bar */}
      <div className="w-48 h-[3px] rounded-full bg-white/10 overflow-hidden">
        <motion.div
          className="h-full rounded-full"
          animate={{ width: `${pct}%`, backgroundColor: cfg.color }}
          transition={{ type: 'spring', stiffness: 55, damping: 20 }}
        />
      </div>

      {/* Tier badge */}
      <motion.div
        className="px-4 py-1 rounded-full text-[11px] font-semibold tracking-widest uppercase border"
        animate={{
          borderColor: `${cfg.rgba}0.35)`,
          color:        cfg.color,
          backgroundColor: `${cfg.rgba}0.08)`,
        }}
        transition={{ duration: 0.8 }}
      >
        {cfg.label}
      </motion.div>
    </div>
  );
}
