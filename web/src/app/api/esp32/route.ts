import { NextRequest, NextResponse } from 'next/server';
import { findStudentByMac, getStudents, getClassSchedules } from '@/lib/supabase';
import { DeviceTelemetry, ESP32Telemetry } from '@/types/occupancy';
import { getTargetEsp32Ip, setTargetEsp32Ip } from '@/lib/esp32Config';

// In-memory telemetry cache for the latest ESP32 state
let latestEsp32Data: ESP32Telemetry = {
  connected_count: 0,
  routing_status: 'unavailable',
  ap_ssid: 'PBL-MC',
  ap_ip: '192.168.4.1',
  sta_ssid: 'Hostel_Guest',
  sta_status: 'disconnected',
  sta_ip: '0.0.0.0',
  channel: 6,
  uptime_secs: 0,
  devices: [],
  last_received_at: 0,
};

async function enrichDevicesWithIdentityAndCompliance(devices: any[]) {
  const schedules = await getClassSchedules();
  const activeSchedule = schedules.find((s) => s.in_session) || null;
  const allStudents = await getStudents();

  const enrichedDevices: DeviceTelemetry[] = [];
  const connectedStudentIds = new Set<string>();

  for (const raw of devices) {
    const mac = raw.mac || raw.id || '';
    const match = await findStudentByMac(mac);

    let compliance_status: DeviceTelemetry['compliance_status'] = 'off_hours';
    const isOutActive = (raw.up_bytes && raw.up_bytes > 5000) || raw.usage === 'active' || (raw.idle_secs !== undefined && raw.idle_secs < 15);

    if (activeSchedule) {
      if (match) {
        // Registered Student Device
        compliance_status = isOutActive ? 'violation_active' : 'compliant_idle';
      } else {
        // Unknown Device connected during class
        compliance_status = isOutActive ? 'unregistered_active' : 'compliant_idle';
      }
    } else {
      compliance_status = 'off_hours';
    }

    if (match) {
      connectedStudentIds.add(match.student.id);
    }

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

  // Calculate absent / not connected students during class
  const absent_students = allStudents
    .filter((s) => !connectedStudentIds.has(s.id))
    .map((s) => ({ id: s.id, roll_no: s.roll_no, name: s.name }));

  const violations_count = enrichedDevices.filter((d) => d.compliance_status === 'violation_active' || d.compliance_status === 'unregistered_active').length;
  const compliant_count = enrichedDevices.filter((d) => d.compliance_status === 'compliant_idle').length;
  const registered_count = enrichedDevices.filter((d) => Boolean(d.student_id)).length;

  return {
    enrichedDevices,
    activeSchedule,
    compliance_summary: {
      total_connected: enrichedDevices.length,
      registered_count,
      violations_count,
      compliant_count,
      unregistered_count: enrichedDevices.length - registered_count,
      absent_students,
    },
  };
}

export async function GET() {
  const now = Date.now();
  let isAlive = latestEsp32Data.last_received_at! > 0 && (now - latestEsp32Data.last_received_at!) < 8000;

  // Fallback Polling: If no push telemetry received recently, query ESP32 directly
  if (!isAlive) {
    const currentConfigIp = getTargetEsp32Ip();
    const candidateUrls = [
      `http://${currentConfigIp}/api/status`,
      'http://192.168.4.1/api/status',
      'http://10.38.17.251/api/status',
    ];

    for (const url of candidateUrls) {
      try {
        const res = await fetch(url, {
          signal: AbortSignal.timeout(600),
          cache: 'no-store',
        });
        if (res.ok) {
          const directData = await res.json();
          if (directData.sta_ip && directData.sta_ip !== '0.0.0.0') {
            setTargetEsp32Ip(directData.sta_ip);
          }
          latestEsp32Data = {
            connected_count: Number(directData.connected_count) || 0,
            internet_allowed: directData.internet_allowed !== undefined ? Boolean(directData.internet_allowed) : true,
            routing_status: directData.routing_status === 'active' ? 'active' : 'unavailable',
            ap_ssid: directData.ap_ssid || 'PBL-MC',
            ap_ip: directData.ap_ip || '192.168.4.1',
            sta_ssid: directData.sta_ssid || 'VoiDX',
            sta_status: directData.sta_status || 'connected',
            sta_ip: directData.sta_ip || currentConfigIp,
            channel: Number(directData.channel) || 6,
            uptime_secs: Number(directData.uptime_secs) || 0,
            devices: Array.isArray(directData.devices) ? directData.devices : [],
            last_received_at: Date.now(),
          };
          isAlive = true;
          break;
        }
      } catch {
        // Try next candidate
      }
    }
  }

  // Enrich with student identity and real-time class compliance
  const { enrichedDevices, activeSchedule, compliance_summary } = await enrichDevicesWithIdentityAndCompliance(latestEsp32Data.devices || []);

  return NextResponse.json(
    {
      ...latestEsp32Data,
      devices: enrichedDevices,
      is_alive: isAlive,
      active_schedule: activeSchedule,
      compliance_summary,
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

    latestEsp32Data = {
      connected_count: Number(payload.connected_count) || 0,
      internet_allowed: payload.internet_allowed !== undefined ? Boolean(payload.internet_allowed) : true,
      routing_status: payload.routing_status === 'active' ? 'active' : 'unavailable',
      ap_ssid: payload.ap_ssid || 'PBL-MC',
      ap_ip: payload.ap_ip || '192.168.4.1',
      sta_ssid: payload.sta_ssid || 'Hostel_Guest',
      sta_status: payload.sta_status || 'connected',
      sta_ip: payload.sta_ip || '0.0.0.0',
      channel: Number(payload.channel) || 6,
      uptime_secs: Number(payload.uptime_secs) || 0,
      devices: Array.isArray(payload.devices) ? payload.devices : [],
      last_received_at: Date.now(),
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
