'use client';

import {
  AreaChart, Area, YAxis, Tooltip, ResponsiveContainer,
} from 'recharts';
import type { OccupancyReading } from '@/types/occupancy';
import type { OccupancyTier } from '@/types/occupancy';
import { tierConfig } from '@/lib/tiers';

interface Props {
  history:  OccupancyReading[];
  tier:     OccupancyTier;
  capacity: number;
}

function fmt(d: Date) {
  return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

interface ChartPoint { t: string; v: number; }

// Custom tooltip so we control exact styling without recharts' default box shadow
function SparkTooltip({ active, payload, label }: {
  active?: boolean; payload?: Array<{ value: number }>; label?: string;
}) {
  if (!active || !payload?.length) return null;
  return (
    <div className="rounded-lg border border-white/10 bg-black/80 backdrop-blur px-3 py-2 text-xs">
      <p className="text-white/40 mb-0.5">{label}</p>
      <p className="text-white font-semibold">{payload[0]?.value} people</p>
    </div>
  );
}

export function SparklineCard({ history, tier, capacity }: Props) {
  const cfg  = tierConfig[tier];
  const data: ChartPoint[] = history.map((r) => ({ t: fmt(r.timestamp), v: r.personCount }));
  const last = history[history.length - 1]?.personCount ?? 0;

  return (
    <div className="relative overflow-hidden rounded-2xl border border-white/10 bg-white/[0.04] backdrop-blur-xl p-5">
      <div className="absolute inset-x-0 top-0 h-px bg-gradient-to-r from-transparent via-white/20 to-transparent" />

      <div className="flex items-center justify-between mb-4">
        <p className="text-[10px] text-white/35 uppercase tracking-widest">Last 15 minutes</p>
        <span className={`text-xs font-semibold ${cfg.textClass}`}>
          {last} now
        </span>
      </div>

      <ResponsiveContainer width="100%" height={110}>
        <AreaChart data={data} margin={{ top: 4, right: 2, left: -28, bottom: 0 }}>
          <defs>
            <linearGradient id="sg" x1="0" y1="0" x2="0" y2="1">
              <stop offset="5%"  stopColor={cfg.color} stopOpacity={0.35} />
              <stop offset="95%" stopColor={cfg.color} stopOpacity={0}    />
            </linearGradient>
          </defs>
          <YAxis
            domain={[0, capacity]}
            tick={{ fill: 'rgba(255,255,255,0.2)', fontSize: 10 }}
            axisLine={false}
            tickLine={false}
            tickCount={3}
          />
          <Tooltip content={<SparkTooltip />} cursor={{ stroke: 'rgba(255,255,255,0.08)' }} />
          <Area
            type="monotone"
            dataKey="v"
            stroke={cfg.color}
            strokeWidth={1.5}
            fill="url(#sg)"
            dot={false}
            activeDot={{ r: 3, fill: cfg.color, strokeWidth: 0 }}
            isAnimationActive={true}
            animationDuration={400}
            animationEasing="ease-out"
          />
        </AreaChart>
      </ResponsiveContainer>
    </div>
  );
}
