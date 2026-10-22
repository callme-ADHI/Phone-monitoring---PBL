'use client';

import { useState, useEffect, useCallback } from 'react';
import type { OccupancyReading } from '@/types/occupancy';

export interface CameraData {
  current: OccupancyReading;
  history: OccupancyReading[];
  roomCapacity: number;
  secondsSinceUpdate: number;
  isConnected: boolean;
}

const ROOM_CAPACITY = 30;
const API_URL = 'http://localhost:8000/api/occupancy';
const POLL_INTERVAL_MS = 1000;

export function useCameraData(): CameraData {
  const [current, setCurrent] = useState<OccupancyReading>({
    personCount: 0,
    timestamp: new Date(),
  });
  const [history, setHistory] = useState<OccupancyReading[]>([
    { personCount: 0, timestamp: new Date() },
  ]);
  const [secondsSince, setSecondsSince] = useState(0);
  const [isConnected, setIsConnected] = useState(false);

  const fetchData = useCallback(async () => {
    try {
      const res = await fetch(API_URL, { cache: 'no-store' });
      if (!res.ok) throw new Error('Camera service unavailable');
      const data = await res.json();

      const reading: OccupancyReading = {
        personCount: Number(data.personCount) || 0,
        rawCount: Number(data.rawCount) || 0,
        timestamp: new Date(data.timestamp || Date.now()),
      };

      setCurrent(reading);
      setIsConnected(true);
      setSecondsSince(0);

      if (Array.isArray(data.history) && data.history.length > 0) {
        setHistory(
          data.history.map((h: { personCount: number; timestamp: string }) => ({
            personCount: Number(h.personCount) || 0,
            timestamp: new Date(h.timestamp),
          }))
        );
      } else {
        setHistory((prev) => [...prev.slice(-71), reading]);
      }
    } catch {
      setIsConnected(false);
    }
  }, []);

  useEffect(() => {
    fetchData();
    const pollTimer = setInterval(fetchData, POLL_INTERVAL_MS);
    const clockTimer = setInterval(() => setSecondsSince((s) => s + 1), 1000);

    return () => {
      clearInterval(pollTimer);
      clearInterval(clockTimer);
    };
  }, [fetchData]);

  return {
    current,
    history,
    roomCapacity: ROOM_CAPACITY,
    secondsSinceUpdate: secondsSince,
    isConnected,
  };
}
