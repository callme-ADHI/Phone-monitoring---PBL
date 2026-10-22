export interface OccupancyReading {
  personCount: number;
  timestamp: Date;
  rawCount?: number;
}

export type OccupancyTier = 'quiet' | 'moderate' | 'crowded';

export interface DeviceTelemetry {
  id: string;
  up_pkts: number;
  up_bytes: number;
  down_pkts: number;
  down_bytes: number;
  rate_bps: number;
  usage: 'idle' | 'light' | 'active';
  idle_secs: number;
}

export interface ESP32Telemetry {
  connected_count: number;
  routing_status: 'active' | 'unavailable';
  ap_ssid: string;
  ap_ip: string;
  sta_ssid: string;
  sta_status: 'connected' | 'connecting' | 'disconnected';
  sta_ip: string;
  channel: number;
  uptime_secs: number;
  devices: DeviceTelemetry[];
  last_received_at?: number;
  is_alive?: boolean;
}
