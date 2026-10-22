'use client';

import { useState, useEffect, useCallback } from 'react';
import type { ESP32Telemetry } from '@/types/occupancy';

export interface PhoneData {
  telemetry: ESP32Telemetry;
  secondsSinceUpdate: number;
  isConnected: boolean;
}

const POLL_INTERVAL_MS = 1500;

export function usePhoneData(): PhoneData {
  const [telemetry, setTelemetry] = useState<ESP32Telemetry>({
    connected_count: 0,
    routing_status: 'unavailable',
    ap_ssid: 'PBL-MC',
    ap_ip: '192.168.4.1',
    sta_ssid: 'VoiDX',
    sta_status: 'disconnected',
    sta_ip: '0.0.0.0',
    channel: 1,
    uptime_secs: 0,
    devices: [],
    is_alive: false,
  });
  const [secondsSince, setSecondsSince] = useState(0);

  const fetchPhoneData = useCallback(async () => {
    try {
      const res = await fetch('/api/esp32', { cache: 'no-store' });
      if (!res.ok) throw new Error('Failed to fetch telemetry');
      const data: ESP32Telemetry = await res.json();

      setTelemetry(data);
      if (data.is_alive) {
        setSecondsSince(0);
      }
    } catch {
      // keep previous state
    }
  }, []);

  useEffect(() => {
    fetchPhoneData();
    const pollTimer = setInterval(fetchPhoneData, POLL_INTERVAL_MS);
    const clockTimer = setInterval(() => setSecondsSince((s) => s + 1), 1000);

    return () => {
      clearInterval(pollTimer);
      clearInterval(clockTimer);
    };
  }, [fetchPhoneData]);

  return {
    telemetry,
    secondsSinceUpdate: secondsSince,
    isConnected: !!telemetry.is_alive,
  };
}
