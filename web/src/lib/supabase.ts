import { StudentRecord, ClassScheduleInfo } from '@/types/occupancy';

// Supabase environment variables
const SUPABASE_URL = process.env.NEXT_PUBLIC_SUPABASE_URL || '';
const SUPABASE_ANON_KEY = process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY || '';

export const isSupabaseConfigured = Boolean(SUPABASE_URL && SUPABASE_ANON_KEY);

// In-Memory Fallback Store (Initialized with realistic demo data)
let mockStudents: StudentRecord[] = [
  {
    id: 'a0000000-0000-0000-0000-000000000001',
    roll_no: 'CS2026-001',
    name: 'Aditya Sharma',
    email: 'aditya@example.edu',
    department: 'Computer Science',
    devices: [
      { id: 'dev-1', mac_address: 'A4:C1:38:12:34:56', device_name: 'iPhone 14', is_primary: true },
      { id: 'dev-2', mac_address: '7C:2E:BD:98:76:54', device_name: 'iPad Air', is_primary: false },
    ],
  },
  {
    id: 'a0000000-0000-0000-0000-000000000002',
    roll_no: 'CS2026-002',
    name: 'Rahul Nair',
    email: 'rahul@example.edu',
    department: 'Computer Science',
    devices: [
      { id: 'dev-3', mac_address: '58:D9:C3:44:55:66', device_name: 'Galaxy S23', is_primary: true },
    ],
  },
  {
    id: 'a0000000-0000-0000-0000-000000000003',
    roll_no: 'CS2026-003',
    name: 'Sneha Patel',
    email: 'sneha@example.edu',
    department: 'Information Technology',
    devices: [
      { id: 'dev-4', mac_address: 'BC:D0:74:11:22:33', device_name: 'OnePlus 11', is_primary: true },
    ],
  },
  {
    id: 'a0000000-0000-0000-0000-000000000004',
    roll_no: 'CS2026-004',
    name: 'Vikram Singh',
    email: 'vikram@example.edu',
    department: 'Computer Science',
    devices: [
      { id: 'dev-5', mac_address: '34:E6:D7:AA:BB:CC', device_name: 'Pixel 8 Pro', is_primary: true },
    ],
  },
  {
    id: 'a0000000-0000-0000-0000-000000000005',
    roll_no: 'CS2026-005',
    name: 'Ananya Roy',
    email: 'ananya@example.edu',
    department: 'Electronics',
    devices: [],
  },
];

let mockSchedules: ClassScheduleInfo[] = [
  {
    id: 'sch-1',
    subject_code: 'CS301',
    subject_name: 'Computer Networks & IoT',
    classroom: 'Lab 304',
    start_time: '08:00',
    end_time: '18:00', // Set wide for immediate demonstration
    is_active: true,
    in_session: true,
  },
];

export async function getStudents(): Promise<StudentRecord[]> {
  if (isSupabaseConfigured) {
    try {
      const res = await fetch(`${SUPABASE_URL}/rest/v1/students?select=id,roll_no,name,email,department,student_devices(*)`, {
        headers: {
          apikey: SUPABASE_ANON_KEY,
          Authorization: `Bearer ${SUPABASE_ANON_KEY}`,
        },
        cache: 'no-store',
      });
      if (res.ok) {
        const data = await res.json();
        return data.map((s: any) => ({
          ...s,
          devices: s.student_devices || [],
        }));
      }
    } catch (e) {
      console.error('Failed to query Supabase students, falling back to mock:', e);
    }
  }
  return mockStudents;
}

export async function findStudentByMac(mac: string): Promise<{ student: StudentRecord; device_name: string } | null> {
  const normMac = mac.trim().toLowerCase();
  const students = await getStudents();

  for (const student of students) {
    for (const dev of student.devices) {
      if (dev.mac_address.toLowerCase() === normMac) {
        return { student, device_name: dev.device_name };
      }
    }
  }
  return null;
}

export async function registerDeviceToStudent(params: {
  mac_address: string;
  student_roll: string;
  student_name?: string;
  device_name?: string;
  department?: string;
}) {
  const normMac = params.mac_address.trim().toUpperCase();

  if (isSupabaseConfigured) {
    try {
      // 1. Find or create student
      let studentRes = await fetch(`${SUPABASE_URL}/rest/v1/students?roll_no=eq.${encodeURIComponent(params.student_roll)}`, {
        headers: { apikey: SUPABASE_ANON_KEY, Authorization: `Bearer ${SUPABASE_ANON_KEY}` },
      });
      let studentList = await studentRes.json();
      let studentId = studentList?.[0]?.id;

      if (!studentId && params.student_name) {
        const createRes = await fetch(`${SUPABASE_URL}/rest/v1/students`, {
          method: 'POST',
          headers: {
            apikey: SUPABASE_ANON_KEY,
            Authorization: `Bearer ${SUPABASE_ANON_KEY}`,
            'Content-Type': 'application/json',
            Prefer: 'return=representation',
          },
          body: JSON.stringify({
            roll_no: params.student_roll,
            name: params.student_name,
            department: params.department || 'General',
          }),
        });
        const created = await createRes.json();
        studentId = created?.[0]?.id;
      }

      if (studentId) {
        // Upsert device
        await fetch(`${SUPABASE_URL}/rest/v1/student_devices`, {
          method: 'POST',
          headers: {
            apikey: SUPABASE_ANON_KEY,
            Authorization: `Bearer ${SUPABASE_ANON_KEY}`,
            'Content-Type': 'application/json',
            Prefer: 'resolution=merge-duplicates',
          },
          body: JSON.stringify({
            student_id: studentId,
            mac_address: normMac,
            device_name: params.device_name || 'Smartphone',
            last_seen_at: new Date().toISOString(),
          }),
        });
        return { success: true };
      }
    } catch (e) {
      console.error('Supabase device registration failed, updating in-memory:', e);
    }
  }

  // In-Memory Fallback Update
  let student = mockStudents.find((s) => s.roll_no.toLowerCase() === params.student_roll.trim().toLowerCase());
  if (!student && params.student_name) {
    student = {
      id: `std-${Date.now()}`,
      roll_no: params.student_roll,
      name: params.student_name,
      department: params.department || 'General',
      devices: [],
    };
    mockStudents.push(student);
  }

  if (student) {
    const existingDev = student.devices.find((d) => d.mac_address.toLowerCase() === normMac.toLowerCase());
    if (existingDev) {
      existingDev.device_name = params.device_name || existingDev.device_name;
    } else {
      student.devices.push({
        id: `dev-${Date.now()}`,
        mac_address: normMac,
        device_name: params.device_name || 'Personal Phone',
        is_primary: student.devices.length === 0,
        last_seen_at: new Date().toISOString(),
      });
    }
    return { success: true, student };
  }

  throw new Error('Student not found and no name provided to create');
}

export async function getClassSchedules(): Promise<ClassScheduleInfo[]> {
  if (isSupabaseConfigured) {
    try {
      const res = await fetch(`${SUPABASE_URL}/rest/v1/class_schedules?is_active=eq.true`, {
        headers: { apikey: SUPABASE_ANON_KEY, Authorization: `Bearer ${SUPABASE_ANON_KEY}` },
        cache: 'no-store',
      });
      if (res.ok) {
        const data = await res.json();
        const now = new Date();
        const curTime = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;
        return data.map((sch: any) => ({
          ...sch,
          in_session: sch.start_time <= curTime && sch.end_time >= curTime,
        }));
      }
    } catch (e) {
      console.error('Failed to query Supabase schedules:', e);
    }
  }

  const now = new Date();
  const curTime = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;
  return mockSchedules.map((sch) => ({
    ...sch,
    in_session: sch.start_time <= curTime && sch.end_time >= curTime,
  }));
}

export async function updateClassSchedule(schedule: ClassScheduleInfo) {
  const idx = mockSchedules.findIndex((s) => s.id === schedule.id);
  if (idx !== -1) {
    mockSchedules[idx] = schedule;
  } else {
    mockSchedules.push(schedule);
  }
  return schedule;
}
