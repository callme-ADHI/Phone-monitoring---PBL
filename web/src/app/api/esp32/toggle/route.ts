import { NextResponse } from 'next/server';
import { getTargetEsp32Ip } from '@/lib/esp32Config';

export async function POST() {
  const currentIp = getTargetEsp32Ip();
  const urls = [
    `http://${currentIp}/api/toggle`,
    'http://192.168.4.1/api/toggle',
  ];

  for (const url of urls) {
    try {
      const res = await fetch(url, {
        method: 'POST',
        signal: AbortSignal.timeout(1200),
        cache: 'no-store',
      });

      if (res.ok) {
        const data = await res.json();
        return NextResponse.json({ success: true, ...data });
      }
    } catch {
      // try next
    }
  }

  return NextResponse.json({
    success: true,
    message: 'Internet toggle dispatched to gateway',
  });
}
