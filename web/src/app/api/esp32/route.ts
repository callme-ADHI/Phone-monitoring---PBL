import { NextRequest, NextResponse } from 'next/server';

// In-memory telemetry cache for the latest ESP32 state
let latestEsp32Data = {
  connected_count: 0,
  routing_status: 'unavailable' as 'active' | 'unavailable',
  ap_ssid: 'PBL-MC',
  ap_ip: '192.168.4.1',
  sta_ssid: 'VoiDX',
  sta_status: 'disconnected' as 'connected' | 'connecting' | 'disconnected',
  sta_ip: '0.0.0.0',
  channel: 1,
  uptime_secs: 0,
  devices: [] as any[],
  last_received_at: 0,
};

export async function GET() {
  const now = Date.now();
  let isAlive = latestEsp32Data.last_received_at > 0 && (now - latestEsp32Data.last_received_at) < 6000;

  // Fallback: If no push telemetry received recently, attempt a fast query to 192.168.4.1
  if (!isAlive) {
    try {
      const res = await fetch('http://192.168.4.1/api/status', {
        signal: AbortSignal.timeout(500),
        cache: 'no-store',
      });
      if (res.ok) {
        const directData = await res.json();
        latestEsp32Data = {
          connected_count: Number(directData.connected_count) || 0,
          routing_status: directData.routing_status === 'active' ? 'active' : 'unavailable',
          ap_ssid: directData.ap_ssid || 'PBL-MC',
          ap_ip: directData.ap_ip || '192.168.4.1',
          sta_ssid: directData.sta_ssid || 'Hostel_Guest',
          sta_status: directData.sta_status || 'connected',
          sta_ip: directData.sta_ip || '0.0.0.0',
          channel: Number(directData.channel) || 6,
          uptime_secs: Number(directData.uptime_secs) || 0,
          devices: Array.isArray(directData.devices) ? directData.devices : [],
          last_received_at: Date.now(),
        };
        isAlive = true;
      }
    } catch {
      // 192.168.4.1 unreachable, keep default state
    }
  }

  return NextResponse.json(
    {
      ...latestEsp32Data,
      is_alive: isAlive,
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
      routing_status: payload.routing_status === 'active' ? 'active' : 'unavailable',
      ap_ssid: payload.ap_ssid || 'PBL-MC',
      ap_ip: payload.ap_ip || '192.168.4.1',
      sta_ssid: payload.sta_ssid || 'VoiDX',
      sta_status: payload.sta_status || 'connected',
      sta_ip: payload.sta_ip || '0.0.0.0',
      channel: Number(payload.channel) || 1,
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
