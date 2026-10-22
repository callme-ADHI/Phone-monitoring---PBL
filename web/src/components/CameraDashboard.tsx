'use client';

import { useMemo } from 'react';
import { motion } from 'framer-motion';
import { TrendingUp, Activity, MapPin, BarChart3 } from 'lucide-react';
import type { CameraData } from '@/hooks/useCameraData';
import { getTier, tierConfig } from '@/lib/tiers';
import { HeroCount } from './HeroCount';
import { SparklineCard } from './SparklineCard';
import { StatCard } from './StatCard';

interface Props {
  cameraData: CameraData;
}

export function CameraDashboard({ cameraData }: Props) {
  const { current, history, roomCapacity } = cameraData;
  const tier = getTier(current.personCount, roomCapacity);
  const cfg = tierConfig[tier];

  const { peak, avg, occupancyPct, roomStatus } = useMemo(() => {
    const counts = history.map((r) => r.personCount);
    const p = Math.max(...counts, 0);
    const a = counts.length > 0 ? Math.round(counts.reduce((s, v) => s + v, 0) / counts.length) : 0;
    const pct = Math.round((current.personCount / roomCapacity) * 100);
    const status =
      current.personCount === 0
        ? 'Empty'
        : current.personCount >= roomCapacity
        ? 'At capacity'
        : tier === 'crowded'
        ? 'Nearly full'
        : tier === 'moderate'
        ? 'In use'
        : 'Available';
    return { peak: p, avg: a, occupancyPct: pct, roomStatus: status };
  }, [history, current, roomCapacity, tier]);

  return (
    <div className="w-full max-w-5xl space-y-6">
      {/* ── Hero Person Count ───────────────────────────────────────── */}
      <section className="flex items-center justify-center w-full py-4">
        <HeroCount count={current.personCount} capacity={roomCapacity} tier={tier} />
      </section>

      {/* ── Sparkline Trend ─────────────────────────────────────────── */}
      <SparklineCard history={history} tier={tier} capacity={roomCapacity} />

      {/* ── 4 Stat Cards ────────────────────────────────────────────── */}
      <div className="grid grid-cols-2 lg:grid-cols-4 gap-4">
        <StatCard
          icon={TrendingUp}
          label="Peak Today"
          value={peak}
          subtext="people detected"
        />
        <StatCard
          icon={Activity}
          label="15-min Average"
          value={avg}
          subtext={`${occupancyPct}% occupancy`}
        />
        <StatCard
          icon={MapPin}
          label="Room Status"
          value={roomStatus}
          subtext={`Cap. ${roomCapacity}`}
        />
        <StatCard
          icon={BarChart3}
          label="Occupancy Rate"
          value={`${occupancyPct}%`}
          subtext={`${current.personCount} of ${roomCapacity}`}
        />
      </div>

      <div className="text-center text-[11px] text-white/20 pt-2">
        AI Model: YOLOv8n (COCO Class 0) • Smoothing: Rolling Median 5 frames • Camera:{' '}
        {cameraData.isConnected ? 'Streaming' : 'Connecting to person_counter.py...'}
      </div>
    </div>
  );
}
