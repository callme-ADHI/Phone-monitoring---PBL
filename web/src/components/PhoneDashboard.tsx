'use client';

import { useState } from 'react';
import { ESP32Telemetry } from '@/types/occupancy';
import { PhoneTable } from './PhoneTable';
import { ClassroomComplianceCard } from './ClassroomComplianceCard';
import { StudentRegistryModal } from './StudentRegistryModal';
import { Wifi, Router, Users, ShieldAlert, UserPlus, Radio, Activity } from 'lucide-react';

interface PhoneDashboardProps {
  data: ESP32Telemetry;
  onRefresh?: () => void;
}

export function PhoneDashboard({ data, onRefresh }: PhoneDashboardProps) {
  const [showRegisterModal, setShowRegisterModal] = useState(false);

  return (
    <div className="space-y-6">
      {/* Top Banner & Quick Actions */}
      <div className="flex flex-wrap items-center justify-between gap-4 p-5 rounded-2xl bg-slate-900/50 border border-slate-800 backdrop-blur-sm">
        <div className="flex items-center gap-3">
          <div className="p-3 rounded-2xl bg-indigo-500/10 border border-indigo-500/20 text-indigo-400">
            <Radio className="w-6 h-6 animate-pulse" />
          </div>
          <div>
            <h1 className="text-lg font-bold text-white tracking-tight">Classroom Network & Phone Monitor</h1>
            <p className="text-xs text-slate-400">
              ESP32 SoftAP &bull; Automatic MAC Address Resolution &bull; Real-time Usage Compliance
            </p>
          </div>
        </div>

        <button
          onClick={() => setShowRegisterModal(true)}
          className="flex items-center gap-2 px-4 py-2 rounded-xl bg-indigo-600 hover:bg-indigo-500 text-white text-xs font-semibold shadow-lg shadow-indigo-600/30 transition-all hover:scale-[1.02] active:scale-[0.98]"
        >
          <UserPlus className="w-4 h-4" />
          <span>Register Student Device</span>
        </button>
      </div>

      {/* Classroom Compliance Policy Card */}
      <ClassroomComplianceCard
        schedule={data.active_schedule}
        summary={data.compliance_summary}
      />

      {/* Hardware Telemetry Stat Cards */}
      <div className="grid grid-cols-2 md:grid-cols-4 gap-4">
        <div className="p-4 rounded-2xl bg-slate-900/60 border border-slate-800/80 shadow-md">
          <div className="flex items-center justify-between text-slate-400 mb-2">
            <span className="text-xs font-medium">Connected Devices</span>
            <Users className="w-4 h-4 text-indigo-400" />
          </div>
          <div className="text-2xl font-bold text-white font-mono">{data.connected_count}</div>
          <div className="text-[11px] text-slate-500 mt-1">
            {data.compliance_summary?.registered_count || 0} registered students
          </div>
        </div>

        <div className="p-4 rounded-2xl bg-slate-900/60 border border-slate-800/80 shadow-md">
          <div className="flex items-center justify-between text-slate-400 mb-2">
            <span className="text-xs font-medium">Policy Violations</span>
            <ShieldAlert className="w-4 h-4 text-red-400" />
          </div>
          <div className={`text-2xl font-bold font-mono ${
            (data.compliance_summary?.violations_count || 0) > 0 ? 'text-red-400' : 'text-slate-200'
          }`}>
            {data.compliance_summary?.violations_count || 0}
          </div>
          <div className="text-[11px] text-slate-500 mt-1">
            {(data.compliance_summary?.violations_count || 0) > 0 ? 'Active in-class usage' : 'No violations'}
          </div>
        </div>

        <div className="p-4 rounded-2xl bg-slate-900/60 border border-slate-800/80 shadow-md">
          <div className="flex items-center justify-between text-slate-400 mb-2">
            <span className="text-xs font-medium">Upstream Internet</span>
            <Wifi className="w-4 h-4 text-emerald-400" />
          </div>
          <div className="text-base font-semibold text-white truncate font-mono">
            {data.sta_ssid || 'Hostel_Guest'}
          </div>
          <div className="text-[11px] text-emerald-400 flex items-center gap-1 mt-1">
            <span className="w-1.5 h-1.5 rounded-full bg-emerald-400 animate-pulse"></span>
            {data.sta_status === 'connected' ? `NAT Active (${data.sta_ip})` : 'Connecting...'}
          </div>
        </div>

        <div className="p-4 rounded-2xl bg-slate-900/60 border border-slate-800/80 shadow-md">
          <div className="flex items-center justify-between text-slate-400 mb-2">
            <span className="text-xs font-medium">ESP32 Router AP</span>
            <Router className="w-4 h-4 text-purple-400" />
          </div>
          <div className="text-base font-semibold text-white font-mono">
            {data.ap_ssid || 'PBL-MC'}
          </div>
          <div className="text-[11px] text-slate-500 mt-1">
            IP {data.ap_ip} &bull; Ch {data.channel}
          </div>
        </div>
      </div>

      {/* Connected Phones & Enriched Student Table */}
      <div className="space-y-3">
        <div className="flex items-center justify-between">
          <h2 className="text-sm font-semibold text-slate-200 flex items-center gap-2">
            <Activity className="w-4 h-4 text-indigo-400" />
            <span>Active Connected Phones</span>
          </h2>
          <span className="text-xs text-slate-500">
            {data.devices?.length || 0} active station(s)
          </span>
        </div>

        <PhoneTable devices={data.devices || []} onRefresh={onRefresh} />
      </div>

      {/* Manual Student Device Registration Modal */}
      <StudentRegistryModal
        isOpen={showRegisterModal}
        onClose={() => setShowRegisterModal(false)}
        onSuccess={() => {
          setShowRegisterModal(false);
          if (onRefresh) onRefresh();
        }}
      />
    </div>
  );
}
