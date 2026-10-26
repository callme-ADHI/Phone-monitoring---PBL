import { NextRequest, NextResponse } from 'next/server';
import { getTargetEsp32Ip } from '@/lib/esp32Config';

export async function POST(req: NextRequest) {
  try {
    const body = await req.json();
    const count = Number(body.count ?? body.personCount ?? 0);
    const currentIp = getTargetEsp32Ip();

    const candidates = [
      `http://${currentIp}/api/person?count=${count}`,
      `http://10.38.17.193/api/person?count=${count}`,
      `http://192.168.199.1/api/person?count=${count}`,
    ];

    const resData = await Promise.any(
      candidates.map(async (url) => {
        const res = await fetch(url, {
          method: 'POST',
          signal: AbortSignal.timeout(1000),
        });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        return res.json();
      })
    ).catch(() => null);

    return NextResponse.json({
      success: true,
      personCount: count,
      esp32_synced: Boolean(resData),
      response: resData,
    });
  } catch (err: any) {
    return NextResponse.json({ error: err.message }, { status: 400 });
  }
}

export async function GET(req: NextRequest) {
  const { searchParams } = new URL(req.url);
  const count = Number(searchParams.get('count') || 0);
  const currentIp = getTargetEsp32Ip();

  try {
    const res = await fetch(`http://${currentIp}/api/person?count=${count}`, {
      method: 'POST',
      signal: AbortSignal.timeout(1000),
    });
    const d = await res.json();
    return NextResponse.json({ success: true, count, response: d });
  } catch (e: any) {
    return NextResponse.json({ success: false, error: e.message });
  }
}
