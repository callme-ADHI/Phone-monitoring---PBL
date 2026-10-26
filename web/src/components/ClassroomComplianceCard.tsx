'use client';

import { BellRing, ShieldCheck, ShieldAlert, Users, Clock, AlertTriangle } from 'lucide-react';
import { ClassScheduleInfo, ESP32Telemetry } from '@/types/occupancy';

interface Props {
  schedule?: ClassScheduleInfo | null;
  summary?: ESP32Telemetry['compliance_summary'];
}

export function ClassroomComplianceCard({ schedule, summary }: Props) {
  const inSession = schedule?.in_session ?? false;
  const violations = summary?.violations_count || 0;
  const compliant = summary?.compliant_count || 0;
  const total = summary?.total_connected || 0;
  const unregistered = summary?.unregistered_count || 0;
  const absentStudents = summary?.absent_students || [];

  return (
    <div className="p-5 rounded-2xl bg-gradient-to-br from-slate-900/90 to-slate-950/90 border border-slate-800 shadow-xl backdrop-blur-md">
      <div className="flex flex-wrap items-center justify-between gap-4 pb-4 border-b border-slate-800/80">
        <div className="flex items-center gap-3">
          <div className={`p-2.5 rounded-xl border ${
            inSession 
              ? 'bg-amber-500/10 border-amber-500/30 text-amber-400 animate-pulse'
              : 'bg-emerald-500/10 border-emerald-500/20 text-emerald-400'
          }`}>
            <Clock className="w-5 h-5" />
          </div>
          <div>
            <div className="flex items-center gap-2">
              <h2 className="text-sm font-semibold text-white">Classroom Policy Monitor</h2>
              <span className={`px-2 py-0.5 rounded-full text-[10px] font-bold tracking-wide uppercase ${
                inSession 
                  ? 'bg-amber-500/20 text-amber-300 border border-amber-500/30'
                  : 'bg-slate-800 text-slate-400 border border-slate-700'
              }`}>
                {inSession ? 'Lecture In Session' : 'Off Hours / Recess'}
              </span>
            </div>
            <p className="text-xs text-slate-400 mt-0.5">
              {schedule 
                ? `${schedule.subject_code}: ${schedule.subject_name} (${schedule.start_time} - ${schedule.end_time}) • ${schedule.classroom}`
                : 'No active lecture schedule configured'
              }
            </p>
          </div>
        </div>

        {/* Live Policy Metric Pills */}
        <div className="flex items-center gap-2">
          <div className="flex items-center gap-1.5 px-3 py-1.5 rounded-xl bg-slate-900 border border-slate-800 text-xs">
            <Users className="w-3.5 h-3.5 text-slate-400" />
            <span className="text-slate-400">Total:</span>
            <span className="font-semibold text-white">{total}</span>
          </div>
          <div className="flex items-center gap-1.5 px-3 py-1.5 rounded-xl bg-emerald-500/10 border border-emerald-500/20 text-xs">
            <ShieldCheck className="w-3.5 h-3.5 text-emerald-400" />
            <span className="text-emerald-400">Compliant:</span>
            <span className="font-bold text-emerald-300">{compliant}</span>
          </div>
          <div className={`flex items-center gap-1.5 px-3 py-1.5 rounded-xl text-xs border ${
            violations > 0 
              ? 'bg-red-500/20 border-red-500/40 text-red-300 animate-pulse'
              : 'bg-slate-900 border-slate-800 text-slate-400'
          }`}>
            <ShieldAlert className="w-3.5 h-3.5 text-red-400" />
            <span className={violations > 0 ? 'text-red-300 font-bold' : 'text-slate-400'}>Violations:</span>
            <span className="font-bold">{violations}</span>
          </div>
        </div>
      </div>

      {/* Warning Alert if Active Violations Detected */}
      {inSession && violations > 0 && (
        <div className="mt-4 p-3.5 rounded-xl bg-red-950/40 border border-red-500/30 flex items-center justify-between gap-3 text-red-200">
          <div className="flex items-center gap-3">
            <div className="p-2 rounded-lg bg-red-500/20 text-red-400">
              <BellRing className="w-4 h-4 animate-bounce" />
            </div>
            <div>
              <p className="text-xs font-semibold text-red-300">
                Unauthorized Phone Usage Detected During Lecture!
              </p>
              <p className="text-[11px] text-red-400/80">
                {violations} device(s) actively transmitting network data while class is underway.
              </p>
            </div>
          </div>
          <span className="px-2.5 py-1 rounded-md text-[10px] font-bold uppercase bg-red-500 text-white tracking-wide shadow-md">
            FLAGGED
          </span>
        </div>
      )}

      {/* Absent Students Summary Pill Row */}
      {inSession && absentStudents.length > 0 && (
        <div className="mt-3 pt-3 border-t border-slate-800/60 flex flex-wrap items-center gap-2">
          <span className="text-[11px] text-slate-400 flex items-center gap-1 font-medium">
            <AlertTriangle className="w-3.5 h-3.5 text-amber-400" />
            Not Connected / Absent ({absentStudents.length}):
          </span>
          {absentStudents.slice(0, 5).map((s) => (
            <span
              key={s.id}
              className="px-2 py-0.5 rounded-md bg-slate-800/70 border border-slate-700/60 text-[10px] text-slate-300"
            >
              {s.name} ({s.roll_no})
            </span>
          ))}
          {absentStudents.length > 5 && (
            <span className="text-[10px] text-slate-500">
              +{absentStudents.length - 5} more
            </span>
          )}
        </div>
      )}
    </div>
  );
}
