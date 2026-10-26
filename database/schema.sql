-- ============================================================================
-- Supabase PostgreSQL Schema: Classroom Phone Monitoring & Attendance System
-- Copy and paste this directly into your Supabase project's SQL Editor.
-- ============================================================================

-- Enable UUID extension
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- 1. Students Roster Table
CREATE TABLE IF NOT EXISTS students (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    roll_no VARCHAR(50) UNIQUE NOT NULL,
    name VARCHAR(150) NOT NULL,
    email VARCHAR(255),
    department VARCHAR(100) DEFAULT 'General',
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

-- 2. Student Devices Table (Supports multiple MAC addresses per student)
CREATE TABLE IF NOT EXISTS student_devices (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    student_id UUID NOT NULL REFERENCES students(id) ON DELETE CASCADE,
    mac_address VARCHAR(17) UNIQUE NOT NULL, -- Format: AA:BB:CC:DD:EE:FF
    device_name VARCHAR(100) DEFAULT 'Smartphone',
    is_primary BOOLEAN DEFAULT TRUE,
    created_at TIMESTAMPTZ DEFAULT NOW(),
    last_seen_at TIMESTAMPTZ DEFAULT NOW()
);

-- Index for instant MAC lookups from ESP32 telemetry
CREATE INDEX IF NOT EXISTS idx_student_devices_mac ON student_devices(LOWER(mac_address));
CREATE INDEX IF NOT EXISTS idx_student_devices_student_id ON student_devices(student_id);

-- 3. Class Timetable / Schedule Table
CREATE TABLE IF NOT EXISTS class_schedules (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    subject_code VARCHAR(50) NOT NULL,
    subject_name VARCHAR(150) NOT NULL,
    classroom VARCHAR(50) DEFAULT 'Room 101',
    start_time TIME NOT NULL,           -- e.g. '09:00:00'
    end_time TIME NOT NULL,             -- e.g. '10:30:00'
    days_of_week INT[] DEFAULT '{1,2,3,4,5}', -- 0=Sun, 1=Mon, ..., 6=Sat
    is_active BOOLEAN DEFAULT TRUE,
    created_at TIMESTAMPTZ DEFAULT NOW()
);

-- 4. Device Activity & Violation Logs
CREATE TABLE IF NOT EXISTS device_activity_logs (
    id BIGSERIAL PRIMARY KEY,
    mac_address VARCHAR(17) NOT NULL,
    student_id UUID REFERENCES students(id) ON DELETE SET NULL,
    recorded_at TIMESTAMPTZ DEFAULT NOW(),
    is_class_hour BOOLEAN DEFAULT FALSE,
    schedule_id UUID REFERENCES class_schedules(id) ON DELETE SET NULL,
    up_bytes BIGINT DEFAULT 0,
    down_bytes BIGINT DEFAULT 0,
    rate_bps INT DEFAULT 0,
    idle_secs INT DEFAULT 0,
    -- Status values: 'compliant_idle', 'violation_active', 'unregistered_active', 'off_hours'
    status VARCHAR(50) NOT NULL,
    notes TEXT
);

CREATE INDEX IF NOT EXISTS idx_logs_timestamp ON device_activity_logs(recorded_at DESC);
CREATE INDEX IF NOT EXISTS idx_logs_mac ON device_activity_logs(LOWER(mac_address));

-- 5. Helpful View: Active Students & Their Devices
CREATE OR REPLACE VIEW view_student_roster AS
SELECT 
    s.id AS student_id,
    s.roll_no,
    s.name AS student_name,
    s.department,
    d.id AS device_id,
    d.mac_address,
    d.device_name,
    d.last_seen_at
FROM students s
LEFT JOIN student_devices d ON s.id = d.student_id;

-- 6. Function to Auto-Update Device Last Seen
CREATE OR REPLACE FUNCTION update_device_last_seen()
RETURNS TRIGGER AS $$
BEGIN
    UPDATE student_devices 
    SET last_seen_at = NEW.recorded_at 
    WHERE LOWER(mac_address) = LOWER(NEW.mac_address);
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_update_device_last_seen ON device_activity_logs;
CREATE TRIGGER trg_update_device_last_seen
AFTER INSERT ON device_activity_logs
FOR EACH ROW
EXECUTE FUNCTION update_device_last_seen();

-- Supabase RLS (Row Level Security) - permissive for local classroom demo
ALTER TABLE students ENABLE ROW LEVEL SECURITY;
ALTER TABLE student_devices ENABLE ROW LEVEL SECURITY;
ALTER TABLE class_schedules ENABLE ROW LEVEL SECURITY;
ALTER TABLE device_activity_logs ENABLE ROW LEVEL SECURITY;

CREATE POLICY "Allow public read-write for students" ON students FOR ALL USING (true) WITH CHECK (true);
CREATE POLICY "Allow public read-write for student_devices" ON student_devices FOR ALL USING (true) WITH CHECK (true);
CREATE POLICY "Allow public read-write for class_schedules" ON class_schedules FOR ALL USING (true) WITH CHECK (true);
CREATE POLICY "Allow public read-write for device_activity_logs" ON device_activity_logs FOR ALL USING (true) WITH CHECK (true);
