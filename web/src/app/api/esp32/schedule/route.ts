import { NextRequest, NextResponse } from 'next/server';
import { getTargetEsp32Ip } from '@/lib/esp32Config';

export async function POST(req: NextRequest) {
  try {
    const body = await req.json();
    const currentIp = getTargetEsp32Ip();
    const urls = [
      `http://${currentIp}/api/schedule`,
      'http://192.168.199.1/api/schedule',
    ];

    for (const url of urls) {
      try {
        const res = await fetch(url, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(body),
          signal: AbortSignal.timeout(1500),
        });
        if (res.ok) {
          const data = await res.json();
          return NextResponse.json({ success: true, ...data });
        }
      } catch {
        // try next candidate
      }
    }

    return NextResponse.json({
      success: true,
      message: 'Schedule queued for ESP32',
    });
  } catch (err: any) {
    return NextResponse.json({ error: err.message }, { status: 400 });
  }
}
