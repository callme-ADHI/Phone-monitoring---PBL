import { NextRequest, NextResponse } from 'next/server';
import { getTargetEsp32Ip, setTargetEsp32Ip } from '@/lib/esp32Config';

export async function GET() {
  return NextResponse.json({
    ip: getTargetEsp32Ip(),
  });
}

export async function POST(req: NextRequest) {
  try {
    const body = await req.json();
    if (body.ip) {
      setTargetEsp32Ip(body.ip);
      return NextResponse.json({ success: true, ip: getTargetEsp32Ip() });
    }
    return NextResponse.json({ error: 'IP is required' }, { status: 400 });
  } catch {
    return NextResponse.json({ error: 'Invalid payload' }, { status: 400 });
  }
}
