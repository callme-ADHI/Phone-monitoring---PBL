import { NextRequest, NextResponse } from 'next/server';
import { getStudents, registerDeviceToStudent } from '@/lib/supabase';

export async function GET() {
  const students = await getStudents();
  return NextResponse.json({ students });
}

export async function POST(req: NextRequest) {
  try {
    const body = await req.json();
    const { mac_address, student_roll, student_name, device_name, department } = body;

    if (!mac_address || !student_roll) {
      return NextResponse.json(
        { error: 'mac_address and student_roll are required' },
        { status: 400 }
      );
    }

    const result = await registerDeviceToStudent({
      mac_address,
      student_roll,
      student_name,
      device_name,
      department,
    });

    return NextResponse.json({ success: true, result });
  } catch (err: any) {
    return NextResponse.json({ error: err.message || 'Registration failed' }, { status: 500 });
  }
}
