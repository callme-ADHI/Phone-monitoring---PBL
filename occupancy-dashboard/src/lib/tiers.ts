import type { OccupancyTier } from '@/types/occupancy';

// Tier boundaries (ratio of count / capacity)
const MODERATE_AT = 0.35;
const CROWDED_AT  = 0.70;

export function getTier(count: number, capacity: number): OccupancyTier {
  const r = count / capacity;
  if (r < MODERATE_AT) return 'quiet';
  if (r < CROWDED_AT)  return 'moderate';
  return 'crowded';
}

// Design tokens per tier.
// Colors chosen for calm readability — no alarm-red, no garish saturation.
//   quiet    → cyan-400:   cool, spacious feel
//   moderate → amber-400:  warm attention without urgency
//   crowded  → orange-400: energetic, still comfortable to look at
export const tierConfig = {
  quiet: {
    color:       '#22d3ee',   // cyan-400
    rgba:        'rgba(34,211,238,',
    textClass:   'text-cyan-400',
    accentClass: 'bg-cyan-400',
    label:       'Quiet',
  },
  moderate: {
    color:       '#fbbf24',   // amber-400
    rgba:        'rgba(251,191,36,',
    textClass:   'text-amber-400',
    accentClass: 'bg-amber-400',
    label:       'Moderate',
  },
  crowded: {
    color:       '#fb923c',   // orange-400
    rgba:        'rgba(251,146,60,',
    textClass:   'text-orange-400',
    accentClass: 'bg-orange-400',
    label:       'Crowded',
  },
} as const satisfies Record<OccupancyTier, {
  color: string; rgba: string; textClass: string; accentClass: string; label: string;
}>;

export type TierConfig = typeof tierConfig;
