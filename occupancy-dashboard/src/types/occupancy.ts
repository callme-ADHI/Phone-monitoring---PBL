export interface OccupancyReading {
  personCount: number;
  timestamp: Date;

  // ── Reserved for future data sources — not rendered in this version ─────────
  // phoneCount:      BLE/WiFi-probe count from the second ESP32
  // wifiDeviceCount: total Wi-Fi devices seen by the probe ESP32
  // mismatch:        true when |personCount - phoneCount| > threshold
  phoneCount?: number;
  wifiDeviceCount?: number;
  mismatch?: boolean;
}

export type OccupancyTier = 'quiet' | 'moderate' | 'crowded';
