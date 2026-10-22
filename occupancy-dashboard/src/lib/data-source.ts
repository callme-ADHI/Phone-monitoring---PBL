// ── Mock data source ───────────────────────────────────────────────────────
// Replace this file's internals to connect real data without touching any
// component or hook. The exported interface that useOccupancyData() depends on:
//
//   generateHistory()  → OccupancyReading[]   (called once on mount)
//   getNextReading(current, momentum)          (called every UPDATE_INTERVAL_MS)
//   ROOM_CAPACITY      → number
//   UPDATE_INTERVAL_MS → number

import type { OccupancyReading } from '@/types/occupancy';

export const ROOM_CAPACITY = 30;
export const UPDATE_INTERVAL_MS = 2500;

const HISTORY_STEP_SEC = 15;
const HISTORY_DURATION_MIN = 15;

function clamp(v: number, lo: number, hi: number) {
  return Math.max(lo, Math.min(hi, v));
}

// Momentum-smoothed random walk: small pull toward mid-range keeps readings
// interesting without wild swings.
function step(
  count: number,
  momentum: number,
): { count: number; momentum: number } {
  const pull = (12 - count) * 0.06;           // gentle gravity toward ~12
  const noise = (Math.random() - 0.5) * 3.8;
  const m = clamp(momentum * 0.65 + noise + pull, -6, 6);
  return { count: Math.round(clamp(count + m, 0, ROOM_CAPACITY)), momentum: m };
}

export function generateHistory(): OccupancyReading[] {
  const now = Date.now();
  const steps = (HISTORY_DURATION_MIN * 60) / HISTORY_STEP_SEC;
  const readings: OccupancyReading[] = [];
  let count = Math.floor(Math.random() * 18) + 2;
  let momentum = 0;

  for (let i = steps; i >= 0; i--) {
    ({ count, momentum } = step(count, momentum));
    readings.push({
      personCount: count,
      timestamp: new Date(now - i * HISTORY_STEP_SEC * 1000),
    });
  }
  return readings;
}

export function getNextReading(
  current: OccupancyReading,
  momentum: number,
): { reading: OccupancyReading; momentum: number } {
  const { count, momentum: m } = step(current.personCount, momentum);
  return {
    reading: { personCount: count, timestamp: new Date() },
    momentum: m,
  };
}
