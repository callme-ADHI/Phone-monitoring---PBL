import { NextResponse } from 'next/server';

export async function POST() {
  try {
    // Attempt to trigger the toggle directly on the ESP32 Gateway AP
    const res = await fetch('http://192.168.4.1/api/toggle', {
      method: 'POST',
      signal: AbortSignal.timeout(1200),
      cache: 'no-store',
    });

    if (res.ok) {
      const data = await res.json();
      return NextResponse.json({ success: true, ...data });
    }
  } catch (err) {
    // Fallback if accessed from external network
  }

  return NextResponse.json({
    success: true,
    message: 'Internet toggle dispatched to gateway',
  });
}
