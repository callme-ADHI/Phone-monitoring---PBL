export interface OccupancyReading {
  personCount: number;
  timestamp: Date;
  rawCount?: number;
}

export type OccupancyTier = 'quiet' | 'moderate' | 'crowded';

export interface DeviceTelemetry {
  id: string;
  mac?: string;
  ip?: string;
  rssi?: number;
  up_pkts: number;
  up_bytes: number;
  down_pkts: number;
  down_bytes: number;
  rate_bps: number;
  usage: 'idle' | 'light' | 'active';
  idle_secs: number;
  // Student & Compliance Resolution
  student_id?: string;
  student_name?: string;
  student_roll?: string;
  department?: string;
  device_name?: string;
  compliance_status?: 'compliant_idle' | 'violation_active' | 'unregistered_active' | 'off_hours';
}

export interface ClassScheduleInfo {
  id: string;
  subject_code: string;
  subject_name: string;
  classroom: string;
  start_time: string;
  end_time: string;
  is_active: boolean;
  in_session: boolean;
}

export interface StudentRecord {
  id: string;
  roll_no: string;
  name: string;
  email?: string;
  department?: string;
  devices: {
    id: string;
    mac_address: string;
    device_name: string;
    is_primary: boolean;
    last_seen_at?: string;
  }[];
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
  active_schedule?: ClassScheduleInfo | null;
  compliance_summary?: {
    total_connected: number;
    registered_count: number;
    violations_count: number;
    compliant_count: number;
    unregistered_count: number;
    absent_students: { id: string; roll_no: string; name: string }[];
  };
}
