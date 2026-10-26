import { StudentRecord, ClassScheduleInfo } from '@/types/occupancy';

const SUPABASE_URL = process.env.NEXT_PUBLIC_SUPABASE_URL || '';
const SUPABASE_ANON_KEY = process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY || '';

export const isSupabaseConfigured = Boolean(SUPABASE_URL && SUPABASE_ANON_KEY);

const SUPABASE_TIMEOUT_MS = 4000; // Hard timeout for all Supabase calls

const HEADERS: Record<string, string> = {
  apikey: SUPABASE_ANON_KEY,
  Authorization: `Bearer ${SUPABASE_ANON_KEY}`,
  'Content-Type': 'application/json',
  Connection: 'close',
};

// Helper: fetch with hard timeout to prevent HTTP/2 hang (UND_ERR_HEADERS_TIMEOUT)
async function sbFetch(url: string, opts: RequestInit = {}): Promise<Response> {
  return fetch(url, {
    ...opts,
    signal: AbortSignal.timeout(SUPABASE_TIMEOUT_MS),
  });
}

// ---------------------------------------------------------------------------
// Students — reads directly from Supabase, no mock fallback
// ---------------------------------------------------------------------------
export async function getStudents(): Promise<StudentRecord[]> {
  if (!isSupabaseConfigured) return [];
  try {
    const res = await sbFetch(
      `${SUPABASE_URL}/rest/v1/students?select=id,roll_no,name,email,department,student_devices(id,mac_address,device_name,is_primary,last_seen_at)`,
      { headers: HEADERS, cache: 'no-store' }
    );
    if (!res.ok) {
      console.error('getStudents failed:', res.status);
      return [];
    }
    const data = await res.json();
    return data.map((s: any) => ({
      ...s,
      devices: s.student_devices || [],
    }));
  } catch {
    return [];
  }
}

// Targeted lookup — queries student_devices table directly by MAC
export async function findStudentByMac(
  mac: string
): Promise<{ student: StudentRecord; device_name: string } | null> {
  if (!isSupabaseConfigured || !mac) return null;
  try {
    const normMac = mac.trim().toUpperCase();
    const res = await sbFetch(
      `${SUPABASE_URL}/rest/v1/student_devices?mac_address=eq.${encodeURIComponent(normMac)}&select=id,mac_address,device_name,is_primary,student_id,students(id,roll_no,name,email,department)`,
      { headers: HEADERS, cache: 'no-store' }
    );
    if (!res.ok) return null;
    const rows = await res.json();
    if (!rows?.length) return null;
    const row = rows[0];
    if (!row.students) return null;
    const student: StudentRecord = {
      id: row.students.id,
      roll_no: row.students.roll_no,
      name: row.students.name,
      email: row.students.email,
      department: row.students.department,
      devices: [
        {
          id: row.id,
          mac_address: row.mac_address,
          device_name: row.device_name,
          is_primary: row.is_primary,
        },
      ],
    };
    return { student, device_name: row.device_name };
  } catch {
    return null;
  }
}

export async function registerDeviceToStudent(params: {
  mac_address: string;
  student_roll: string;
  student_name?: string;
  device_name?: string;
  department?: string;
}) {
  if (!isSupabaseConfigured) throw new Error('Supabase not configured');

  const normMac = params.mac_address.trim().toUpperCase();

  // 1. Find or create student by roll_no
  const studentRes = await sbFetch(
    `${SUPABASE_URL}/rest/v1/students?roll_no=eq.${encodeURIComponent(params.student_roll)}&select=id`,
    { headers: HEADERS }
  );
  const studentList = await studentRes.json();
  let studentId: string | undefined = studentList?.[0]?.id;

  if (!studentId) {
    if (!params.student_name) throw new Error('student_name required to create new student');
    const createRes = await sbFetch(`${SUPABASE_URL}/rest/v1/students`, {
      method: 'POST',
      headers: { ...HEADERS, Prefer: 'return=representation' },
      body: JSON.stringify({
        roll_no: params.student_roll,
        name: params.student_name,
        department: params.department || 'General',
      }),
    });
    if (!createRes.ok) throw new Error(`Failed to create student: ${createRes.status}`);
    const created = await createRes.json();
    studentId = created?.[0]?.id;
  }

  if (!studentId) throw new Error('Could not resolve student ID');

  // 2. Upsert device
  const devRes = await sbFetch(`${SUPABASE_URL}/rest/v1/student_devices`, {
    method: 'POST',
    headers: { ...HEADERS, Prefer: 'resolution=merge-duplicates,return=representation' },
    body: JSON.stringify({
      student_id: studentId,
      mac_address: normMac,
      device_name: params.device_name || 'Smartphone',
      last_seen_at: new Date().toISOString(),
    }),
  });
  if (!devRes.ok) throw new Error(`Failed to register device: ${devRes.status}`);

  return { success: true };
}

// ---------------------------------------------------------------------------
// Class Schedules — with real day-of-week check
// ---------------------------------------------------------------------------
export async function getClassSchedules(): Promise<ClassScheduleInfo[]> {
  if (!isSupabaseConfigured) return [];

  try {
    const res = await sbFetch(
      `${SUPABASE_URL}/rest/v1/class_schedules?is_active=eq.true`,
      { headers: HEADERS, cache: 'no-store' }
    );
    if (!res.ok) return [];
    const data = await res.json();

    const now = new Date();
    const dayOfWeek = now.getDay(); // 0=Sun, 1=Mon, ... 6=Sat
    const curTime = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;

    return data.map((sch: any) => {
      const start = (sch.start_time ?? '00:00').slice(0, 5);
      const end = (sch.end_time ?? '23:59').slice(0, 5);
      const daysOk = Array.isArray(sch.days_of_week)
        ? sch.days_of_week.includes(dayOfWeek)
        : true; // if no days set, assume always active
      return {
        id: sch.id,
        subject_code: sch.subject_code,
        subject_name: sch.subject_name,
        classroom: sch.classroom,
        start_time: start,
        end_time: end,
        days_of_week: sch.days_of_week ?? [],
        is_active: sch.is_active,
        in_session: daysOk && start <= curTime && end >= curTime,
      };
    });
  } catch {
    return [];
  }
}

export async function updateClassSchedule(
  schedule: Partial<ClassScheduleInfo> & { id: string }
) {
  if (!isSupabaseConfigured) throw new Error('Supabase not configured');
  const res = await sbFetch(`${SUPABASE_URL}/rest/v1/class_schedules?id=eq.${schedule.id}`, {
    method: 'PATCH',
    headers: { ...HEADERS, Prefer: 'return=representation' },
    body: JSON.stringify(schedule),
  });
  if (!res.ok) throw new Error(`updateClassSchedule failed: ${res.status}`);
  return res.json();
}
