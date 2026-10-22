'use client';

import { useMemo } from 'react';
import { motion } from 'framer-motion';
import { TrendingUp, Activity, MapPin, BarChart3 } from 'lucide-react';

import { useOccupancyData } from '@/hooks/useOccupancyData';
import { getTier, tierConfig }   from '@/lib/tiers';
import { AmbientBackground }  from './AmbientBackground';
import { HeroCount }          from './HeroCount';
import { LiveIndicator }      from './LiveIndicator';
import { SparklineCard }      from './SparklineCard';
import { StatCard }           from './StatCard';

// Entrance animation: the outer container staggers its direct motion children.
// Nested containers (the cards grid) have their own stagger so stat cards
// fan in after the sparkline, not in lockstep with it.
const outer = {
  hidden:  { opacity: 0 },
  visible: { opacity: 1, transition: { staggerChildren: 0.09, delayChildren: 0.15 } },
};
const item = {
  hidden:  { opacity: 0, y: 22 },
  visible: { opacity: 1, y: 0,  transition: { type: 'spring', stiffness: 110, damping: 22 } },
};
const grid = {
  hidden:  {},
  visible: { transition: { staggerChildren: 0.07 } },
};

export function Dashboard() {
  const { current, history, roomCapacity, secondsSinceUpdate, isConnected } = useOccupancyData();
  const tier = getTier(current.personCount, roomCapacity);
  const cfg  = tierConfig[tier];

  const { peak, avg, occupancyPct, roomStatus } = useMemo(() => {
    const counts = history.map((r) => r.personCount);
    const p = Math.max(...counts);
    const a = Math.round(counts.reduce((s, v) => s + v, 0) / counts.length);
    const pct = Math.round((current.personCount / roomCapacity) * 100);
    const status =
      current.personCount === 0 ? 'Empty'
      : current.personCount >= roomCapacity ? 'At capacity'
      : tier === 'crowded'  ? 'Nearly full'
      : tier === 'moderate' ? 'In use'
      : 'Available';
    return { peak: p, avg: a, occupancyPct: pct, roomStatus: status };
  }, [history, current, roomCapacity, tier]);

  return (
    <div className="relative min-h-screen bg-[#07070f] text-white overflow-hidden">
      <AmbientBackground />

      <motion.main
        className="relative z-10 flex flex-col items-center min-h-screen px-4 sm:px-8 py-8"
        variants={outer}
        initial="hidden"
        animate="visible"
      >
        {/* ── Header ─────────────────────────────────────────────────────────── */}
        <motion.header
          className="w-full max-w-5xl flex items-start sm:items-center justify-between gap-4 mb-10 sm:mb-16"
          variants={item}
        >
          <div>
            <h1 className="text-[11px] font-semibold tracking-[0.2em] uppercase text-white/50">
              Room Occupancy
            </h1>
            <p className="text-xs text-white/20 mt-0.5">Classroom A · Building 3</p>
          </div>
          <LiveIndicator secondsSince={secondsSinceUpdate} isConnected={isConnected} />
        </motion.header>

        {/* ── Hero count ─────────────────────────────────────────────────────── */}
        <motion.section
          className="flex-1 flex items-center justify-center w-full py-4"
          variants={item}
        >
          <HeroCount count={current.personCount} capacity={roomCapacity} tier={tier} />
        </motion.section>

        {/* ── Cards ──────────────────────────────────────────────────────────── */}
        <motion.section className="w-full max-w-5xl space-y-4 mt-10 mb-6" variants={item}>
          {/* Sparkline — full width above the stat cards */}
          <SparklineCard history={history} tier={tier} capacity={roomCapacity} />

          {/* 4 stat cards: 2 col on mobile, 4 col on lg */}
          <motion.div
            className="grid grid-cols-2 lg:grid-cols-4 gap-4"
            variants={grid}
          >
            {[
              { icon: TrendingUp, label: 'Peak today',   value: peak,          subtext: 'people detected'            },
              { icon: Activity,   label: '15-min avg',   value: avg,           subtext: `${occupancyPct}% occupancy` },
              { icon: MapPin,     label: 'Room status',  value: roomStatus,    subtext: `Cap. ${roomCapacity}`       },
              { icon: BarChart3,  label: 'Occupancy',    value: `${occupancyPct}%`, subtext: `${current.personCount} of ${roomCapacity}` },
            ].map((s) => (
              <motion.div key={s.label} variants={item}>
                <StatCard icon={s.icon} label={s.label} value={s.value} subtext={s.subtext} />
              </motion.div>
            ))}
          </motion.div>
        </motion.section>

        {/* ── Footer ─────────────────────────────────────────────────────────── */}
        <motion.footer
          className="w-full max-w-5xl text-center text-[10px] text-white/10 pb-2"
          variants={item}
        >
          Occupancy system · camera-based detection · updates every 2.5 s
          <span className={`ml-2 font-medium ${cfg.textClass} opacity-50`}>● {cfg.label}</span>
        </motion.footer>
      </motion.main>
    </div>
  );
}
