import { NextRequest, NextResponse } from 'next/server';
import { findStudentByMac, getStudents, getClassSchedules } from '@/lib/supabase';
import { DeviceTelemetry, ESP32Telemetry } from '@/types/occupancy';
import { getTargetEsp32Ip, setTargetEsp32Ip } from '@/lib/esp32Config';

// Default empty telemetry when ESP32 is offline
const emptyTelemetry: ESP32Telemetry = {
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
  last_received_at: 0,
  is_alive: false,
};

let latestEsp32Data: ESP32Telemetry = { ...emptyTelemetry };

async function fetchFromEsp32(): Promise<boolean> {
  const currentConfigIp = getTargetEsp32Ip();

  const candidates = [
    `http://${currentConfigIp}/api/status`,
    'http://192.168.199.1/api/status',
    'http://10.38.17.193/api/status',
    'http://10.38.17.251/api/status',
  ];

  const seen = new Set<string>();
  const urls: string[] = [];
  for (const u of candidates) {
    if (!seen.has(u)) {
      seen.add(u);
      urls.push(u);
    }
  }

  for (const url of urls) {
    try {
      const res = await fetch(url, {
        signal: AbortSignal.timeout(900),
        cache: 'no-store',
      });
      if (!res.ok) continue;
      const d = await res.json();

      if (d.sta_ip && d.sta_ip !== '0.0.0.0') {
        setTargetEsp32Ip(d.sta_ip);
      }

      latestEsp32Data = {
        connected_count: Number(d.connected_count) || 0,
        internet_allowed: d.internet_allowed !== undefined ? Boolean(d.internet_allowed) : true,
        routing_status: d.routing_status === 'active' ? 'active' : 'unavailable',
        ap_ssid: d.ap_ssid || 'PBL-MC',
        ap_ip: d.ap_ip || '192.168.199.1',
        sta_ssid: d.sta_ssid || 'VoiDX',
        sta_status: d.sta_status || 'connected',
        sta_ip: d.sta_ip || currentConfigIp,
        channel: Number(d.channel) || 6,
        uptime_secs: Number(d.uptime_secs) || 0,
        devices: Array.isArray(d.devices) ? d.devices : [],
        last_received_at: Date.now(),
        is_alive: true,
      };
      return true;
    } catch {
      // try next candidate
    }
  }

  return false;
}

async function enrichDevices(devices: any[]) {
  const schedules = await getClassSchedules();
  const activeSchedule = schedules.find((s) => s.in_session) || null;
  const allStudents = await getStudents();

  const enrichedDevices: DeviceTelemetry[] = [];
  const connectedStudentIds = new Set<string>();

  for (const raw of devices) {
    const mac = (raw.mac || raw.id || '').trim().toUpperCase();
    const match = mac ? await findStudentByMac(mac) : null;

    const isActive =
      (raw.up_bytes && raw.up_bytes > 2000) ||
      raw.usage === 'active' ||
      (raw.idle_secs !== undefined && raw.idle_secs < 15);

    let compliance_status: DeviceTelemetry['compliance_status'] = 'off_hours';
    if (activeSchedule) {
      if (match) {
        compliance_status = isActive ? 'violation_active' : 'compliant_idle';
      } else {
        compliance_status = isActive ? 'unregistered_active' : 'compliant_idle';
      }
    }

    if (match) connectedStudentIds.add(match.student.id);

    enrichedDevices.push({
      id: raw.id,
      mac: raw.mac || raw.id,
      ip: raw.ip,
      rssi: raw.rssi,
      up_pkts: Number(raw.up_pkts) || 0,
      up_bytes: Number(raw.up_bytes) || 0,
      down_pkts: Number(raw.down_pkts) || 0,
      down_bytes: Number(raw.down_bytes) || 0,
      rate_bps: Number(raw.rate_bps) || 0,
      dns_queries: Number(raw.dns_queries) || 0,
      last_domain: raw.last_domain || 'None',
      usage: raw.usage || (raw.idle_secs < 10 ? 'active' : 'idle'),
      idle_secs: Number(raw.idle_secs) || 0,
      student_id: match?.student.id,
      student_name: match?.student.name,
      student_roll: match?.student.roll_no,
      department: match?.student.department,
      device_name: match?.device_name,
      compliance_status,
    });
  }

  const absent_students = allStudents
    .filter((s) => !connectedStudentIds.has(s.id))
    .map((s) => ({ id: s.id, roll_no: s.roll_no, name: s.name }));

  return {
    enrichedDevices,
    activeSchedule,
    compliance_summary: {
      total_connected: enrichedDevices.length,
      registered_count: enrichedDevices.filter((d) => Boolean(d.student_id)).length,
      violations_count: enrichedDevices.filter(
        (d) => d.compliance_status === 'violation_active' || d.compliance_status === 'unregistered_active'
      ).length,
      compliant_count: enrichedDevices.filter((d) => d.compliance_status === 'compliant_idle').length,
      unregistered_count: enrichedDevices.filter((d) => !d.student_id).length,
      absent_students,
    },
  };
}

export async function GET() {
  const now = Date.now();
  const staleMs = 3500;
  const isAliveFromPush =
    latestEsp32Data.last_received_at! > 0 && now - latestEsp32Data.last_received_at! < staleMs;

  let isAlive = isAliveFromPush;
  if (!isAlive) {
    isAlive = await fetchFromEsp32();
  }

  // CRITICAL: If ESP32 is offline, zero out all counters and devices immediately!
  // Never show stale devices when disconnected.
  const activePayload = isAlive ? latestEsp32Data : { ...emptyTelemetry, is_alive: false };

  const { enrichedDevices, activeSchedule, compliance_summary } = await enrichDevices(
    activePayload.devices || []
  );

  return NextResponse.json(
    {
      ...activePayload,
      devices: enrichedDevices,
      is_alive: isAlive,
      active_schedule: activeSchedule,
      compliance_summary,
      esp32_ip: getTargetEsp32Ip(),
    },
    {
      headers: {
        'Cache-Control': 'no-store, max-age=0',
        'Access-Control-Allow-Origin': '*',
      },
    }
  );
}

export async function POST(req: NextRequest) {
  try {
    const payload = await req.json();

    if (payload.sta_ip && payload.sta_ip !== '0.0.0.0') {
      setTargetEsp32Ip(payload.sta_ip);
    }

    latestEsp32Data = {
      connected_count: Number(payload.connected_count) || 0,
      internet_allowed: payload.internet_allowed !== undefined ? Boolean(payload.internet_allowed) : true,
      routing_status: payload.routing_status === 'active' ? 'active' : 'unavailable',
      ap_ssid: payload.ap_ssid || 'PBL-MC',
      ap_ip: payload.ap_ip || '192.168.199.1',
      sta_ssid: payload.sta_ssid || 'VoiDX',
      sta_status: payload.sta_status || 'connected',
      sta_ip: payload.sta_ip || '0.0.0.0',
      channel: Number(payload.channel) || 6,
      uptime_secs: Number(payload.uptime_secs) || 0,
      devices: Array.isArray(payload.devices) ? payload.devices : [],
      last_received_at: Date.now(),
      is_alive: true,
    };

    return NextResponse.json({
      status: 'ok',
      received_devices: latestEsp32Data.devices.length,
      timestamp: latestEsp32Data.last_received_at,
    });
  } catch {
    return NextResponse.json({ error: 'Invalid telemetry payload' }, { status: 400 });
  }
}
