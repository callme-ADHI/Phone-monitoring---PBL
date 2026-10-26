import { NextRequest, NextResponse } from 'next/server';
import { getClassSchedules, updateClassSchedule } from '@/lib/supabase';

export async function GET() {
  const schedules = await getClassSchedules();
  return NextResponse.json({ schedules });
}

export async function POST(req: NextRequest) {
  try {
    const body = await req.json();
    const schedule = await updateClassSchedule(body);
    return NextResponse.json({ success: true, schedule });
  } catch (err: any) {
    return NextResponse.json({ error: err.message || 'Failed to update schedule' }, { status: 500 });
  }
}
