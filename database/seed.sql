-- ============================================================================
-- Supabase Seed Data: Demo Classroom Roster & Class Schedule
-- Run this in your Supabase SQL Editor after running schema.sql
-- ============================================================================

-- Insert Sample Students
INSERT INTO students (id, roll_no, name, email, department) VALUES
    ('a0000000-0000-0000-0000-000000000001', 'CS2026-001', 'Aditya Sharma', 'aditya@example.edu', 'Computer Science'),
    ('a0000000-0000-0000-0000-000000000002', 'CS2026-002', 'Rahul Nair', 'rahul@example.edu', 'Computer Science'),
    ('a0000000-0000-0000-0000-000000000003', 'CS2026-003', 'Sneha Patel', 'sneha@example.edu', 'Information Technology'),
    ('a0000000-0000-0000-0000-000000000004', 'CS2026-004', 'Vikram Singh', 'vikram@example.edu', 'Computer Science'),
    ('a0000000-0000-0000-0000-000000000005', 'CS2026-005', 'Ananya Roy', 'ananya@example.edu', 'Electronics')
ON CONFLICT (roll_no) DO NOTHING;

-- Insert Pre-registered Devices (Sample MAC Addresses)
INSERT INTO student_devices (student_id, mac_address, device_name, is_primary) VALUES
    ('a0000000-0000-0000-0000-000000000001', 'A4:C1:38:12:34:56', 'Aditya iPhone 14', TRUE),
    ('a0000000-0000-0000-0000-000000000001', '7C:2E:BD:98:76:54', 'Aditya iPad Air', FALSE),
    ('a0000000-0000-0000-0000-000000000002', '58:D9:C3:44:55:66', 'Rahul Samsung S23', TRUE),
    ('a0000000-0000-0000-0000-000000000003', 'BC:D0:74:11:22:33', 'Sneha OnePlus 11', TRUE),
    ('a0000000-0000-0000-0000-000000000004', '34:E6:D7:AA:BB:CC', 'Vikram Pixel 8', TRUE)
ON CONFLICT (mac_address) DO NOTHING;

-- Insert Classroom Schedule (Monday to Friday, covering typical lecture hours)
INSERT INTO class_schedules (subject_code, subject_name, classroom, start_time, end_time, days_of_week, is_active) VALUES
    ('CS301', 'Computer Networks & IoT', 'Lab 304', '09:00:00', '10:30:00', '{1,2,3,4,5}', TRUE),
    ('CS302', 'Operating Systems Lab', 'Room 202', '10:45:00', '12:15:00', '{1,2,3,4,5}', TRUE),
    ('CS303', 'Database Management Systems', 'Auditorium A', '13:30:00', '15:00:00', '{1,2,3,4,5}', TRUE)
ON CONFLICT DO NOTHING;
