'use client';

import { useState, useEffect, useCallback, useRef } from 'react';
import type { ESP32Telemetry } from '@/types/occupancy';

export interface PhoneData {
  telemetry: ESP32Telemetry;
  secondsSinceUpdate: number;
  isConnected: boolean;
  refresh: () => Promise<void>;
}

// Fetch directly from ESP32 for raw live data (sub-second latency)
// Falls back to Next.js proxy which also does Supabase enrichment
const DIRECT_ESP32_TIMEOUT = 800;
const POLL_INTERVAL_MS = 1200;
const ENRICH_INTERVAL_MS = 5000; // Supabase enrichment less frequently

let cachedEsp32Ip = '10.38.17.251';

async function fetchDirectFromEsp32(): Promise<any | null> {
  const urls = [
    `http://${cachedEsp32Ip}/api/status`,
    'http://192.168.199.1/api/status',
  ];
  for (const url of urls) {
    try {
      const res = await fetch(url, {
        signal: AbortSignal.timeout(DIRECT_ESP32_TIMEOUT),
        cache: 'no-store',
        mode: 'cors',
      });
      if (res.ok) {
        const data = await res.json();
        if (data.sta_ip && data.sta_ip !== '0.0.0.0') {
          cachedEsp32Ip = data.sta_ip;
        }
        return data;
      }
    } catch {
      // try next
    }
  }
  return null;
}

async function fetchEnrichedFromServer(): Promise<ESP32Telemetry | null> {
  try {
    const res = await fetch('/api/esp32', {
      cache: 'no-store',
      signal: AbortSignal.timeout(4000),
    });
    if (res.ok) return await res.json();
  } catch {
    // ignore
  }
  return null;
}

export function usePhoneData(): PhoneData {
  const [telemetry, setTelemetry] = useState<ESP32Telemetry>({
    connected_count: 0,
    routing_status: 'unavailable',
    ap_ssid: 'PBL-MC',
    ap_ip: '192.168.199.1',
    sta_ssid: 'VoiDX',
    sta_status: 'disconnected',
    sta_ip: '0.0.0.0',
    channel: 6,
    uptime_secs: 0,
    devices: [],
    is_alive: false,
  });
  const [secondsSince, setSecondsSince] = useState(0);
  const enrichedRef = useRef<Partial<ESP32Telemetry>>({});

  // Fast path: fetch raw from ESP32 and merge with last enrichment
  const fetchFast = useCallback(async () => {
    const raw = await fetchDirectFromEsp32();
    if (raw) {
      setSecondsSince(0);
      setTelemetry((prev) => {
        // Merge raw traffic data with enriched student/compliance data from last slow fetch
        const enrichedDevices = enrichedRef.current.devices || [];
        const mergedDevices = (raw.devices || []).map((rawDev: any) => {
          const enriched = enrichedDevices.find(
            (e: any) => e.mac === rawDev.mac || e.id === rawDev.id
          );
          return enriched
            ? {
                ...enriched,
                // Always use fresh traffic values from ESP32
                up_bytes: rawDev.up_bytes,
                down_bytes: rawDev.down_bytes,
                up_pkts: rawDev.up_pkts,
                down_pkts: rawDev.down_pkts,
                rate_bps: rawDev.rate_bps,
                rssi: rawDev.rssi,
                dns_queries: rawDev.dns_queries,
                last_domain: rawDev.last_domain,
                usage: rawDev.usage,
                idle_secs: rawDev.idle_secs,
              }
            : { ...rawDev, compliance_status: 'off_hours' as const };
        });

        return {
          ...prev,
          connected_count: raw.connected_count,
          internet_allowed: raw.internet_allowed,
          routing_status: raw.routing_status,
          ap_ssid: raw.ap_ssid,
          ap_ip: raw.ap_ip,
          sta_ssid: raw.sta_ssid,
          sta_status: raw.sta_status,
          sta_ip: raw.sta_ip,
          channel: raw.channel,
          uptime_secs: raw.uptime_secs,
          devices: mergedDevices,
          is_alive: true,
          last_received_at: Date.now(),
        };
      });
    }
  }, []);

  // Slow path: full enrichment (student names, compliance, schedule)
  const fetchEnriched = useCallback(async () => {
    const enriched = await fetchEnrichedFromServer();
    if (enriched) {
      enrichedRef.current = enriched;
      setTelemetry(enriched);
      setSecondsSince(0);
    }
  }, []);

  // Combined refresh (used by manual refresh button)
  const refresh = useCallback(async () => {
    await fetchEnriched();
  }, [fetchEnriched]);

  useEffect(() => {
    // Initial enriched fetch
    fetchEnriched();

    // Fast raw poll every 1.2s
    const fastTimer = setInterval(fetchFast, POLL_INTERVAL_MS);

    // Enriched (with Supabase student names) every 5s
    const enrichTimer = setInterval(fetchEnriched, ENRICH_INTERVAL_MS);

    // Clock
    const clockTimer = setInterval(() => setSecondsSince((s) => s + 1), 1000);

    return () => {
      clearInterval(fastTimer);
      clearInterval(enrichTimer);
      clearInterval(clockTimer);
    };
  }, [fetchFast, fetchEnriched]);

  return {
    telemetry,
    secondsSinceUpdate: secondsSince,
    isConnected: !!telemetry.is_alive,
    refresh,
  };
}
