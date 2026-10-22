'use client';

import { useMemo } from 'react';
import { motion } from 'framer-motion';
import { Users, Smartphone, ShieldAlert, CheckCircle2, Wifi, ArrowRight } from 'lucide-react';
import type { CameraData } from '@/hooks/useCameraData';
import type { PhoneData } from '@/hooks/usePhoneData';
import { PhoneTable } from './PhoneTable';

interface Props {
  cameraData: CameraData;
  phoneData: PhoneData;
  onSelectTab: (tab: 'camera' | 'phones') => void;
}

export function CombinedDashboard({ cameraData, phoneData, onSelectTab }: Props) {
  const people = cameraData.current.personCount;
  const phones = phoneData.telemetry.connected_count;

  const correlation = useMemo(() => {
    if (!cameraData.isConnected && !phoneData.isConnected) {
      return {
        label: 'Awaiting Feeds',
        desc: 'Start person_counter.py and power on ESP32 to begin correlation analysis.',
        color: 'text-white/40',
        border: 'border-white/10',
        bg: 'bg-white/5',
        icon: Wifi,
      };
    }

    const diff = people - phones;

    if (diff === 0) {
      return {
        label: 'Perfect 1:1 Match',
        desc: `Every detected person (${people}) has exactly one active phone associated on the network.`,
        color: 'text-emerald-400',
        border: 'border-emerald-500/30',
        bg: 'bg-emerald-500/10',
        icon: CheckCircle2,
      };
    } else if (diff > 0) {
      return {
        label: `${diff} Unconnected Person${diff > 1 ? 's' : ''}`,
        desc: `Camera sees ${people} people, but only ${phones} phone${phones === 1 ? '' : 's'} connected to WiFi (phones disconnected, off, or not joined).`,
        color: 'text-amber-400',
        border: 'border-amber-500/30',
        bg: 'bg-amber-500/10',
        icon: ShieldAlert,
      };
    } else {
      const extra = Math.abs(diff);
      return {
        label: `${extra} Secondary Device${extra > 1 ? 's' : ''}`,
        desc: `Only ${people} people in room, but ${phones} devices connected (laptops/tablets or multiple phones per person).`,
        color: 'text-orange-400',
        border: 'border-orange-500/30',
        bg: 'bg-orange-500/10',
        icon: ShieldAlert,
      };
    }
  }, [people, phones, cameraData.isConnected, phoneData.isConnected]);

  const CorrelationIcon = correlation.icon;

  return (
    <div className="w-full max-w-5xl space-y-6">
      {/* ── Correlation / Discrepancy Card ─────────────────────────── */}
      <div
        className={`p-4 sm:p-5 rounded-2xl border ${correlation.border} ${correlation.bg} backdrop-blur-xl flex items-start gap-4 transition-all`}
      >
        <div className="shrink-0 p-2.5 rounded-xl bg-black/20 mt-0.5">
          <CorrelationIcon className={`w-5 h-5 ${correlation.color}`} />
        </div>
        <div className="flex-1 min-w-0">
          <div className="flex items-center gap-2">
            <h4 className={`text-sm font-bold ${correlation.color}`}>{correlation.label}</h4>
            <span className="text-[10px] uppercase font-bold tracking-wider px-2 py-0.5 rounded bg-black/30 text-white/50">
              Cross-Validation
            </span>
          </div>
          <p className="text-xs text-white/70 mt-1 leading-relaxed">{correlation.desc}</p>
        </div>
      </div>

      {/* ── Dual Side-by-Side Hero Counters ────────────────────────── */}
      <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
        {/* Camera Person Counter Card */}
        <div
          onClick={() => onSelectTab('camera')}
          className="group relative overflow-hidden rounded-2xl border border-white/10 bg-white/[0.04] backdrop-blur-xl p-6 cursor-pointer hover:border-cyan-500/40 transition-all"
        >
          <div className="flex items-center justify-between mb-4">
            <div className="flex items-center gap-2 text-xs font-semibold uppercase tracking-wider text-white/50">
              <Users className="w-4 h-4 text-cyan-400" />
              <span>Camera Person Count</span>
            </div>
            <span className="text-[11px] text-cyan-400 flex items-center gap-1 group-hover:translate-x-1 transition-transform">
              Full View <ArrowRight className="w-3 h-3" />
            </span>
          </div>

          <div className="flex items-baseline gap-3 my-2">
            <span className="font-mono text-7xl font-black text-cyan-400 leading-none">
              {people}
            </span>
            <span className="text-xs text-white/30 uppercase tracking-widest">
              of {cameraData.roomCapacity} capacity
            </span>
          </div>

          <div className="flex items-center justify-between text-xs text-white/40 mt-6 pt-4 border-t border-white/5">
            <span>Source: OpenCV + YOLOv8n</span>
            <span className={cameraData.isConnected ? 'text-emerald-400' : 'text-white/20'}>
              ● {cameraData.isConnected ? 'Live Webcam' : 'Camera Offline'}
            </span>
          </div>
        </div>

        {/* ESP32 Phone Counter Card */}
        <div
          onClick={() => onSelectTab('phones')}
          className="group relative overflow-hidden rounded-2xl border border-white/10 bg-white/[0.04] backdrop-blur-xl p-6 cursor-pointer hover:border-sky-500/40 transition-all"
        >
          <div className="flex items-center justify-between mb-4">
            <div className="flex items-center gap-2 text-xs font-semibold uppercase tracking-wider text-white/50">
              <Smartphone className="w-4 h-4 text-sky-400" />
              <span>WiFi Phone Monitor</span>
            </div>
            <span className="text-[11px] text-sky-400 flex items-center gap-1 group-hover:translate-x-1 transition-transform">
              Full View <ArrowRight className="w-3 h-3" />
            </span>
          </div>

          <div className="flex items-baseline gap-3 my-2">
            <span className="font-mono text-7xl font-black text-sky-400 leading-none">
              {phones}
            </span>
            <span className="text-xs text-white/30 uppercase tracking-widest">
              associated stations
            </span>
          </div>

          <div className="flex items-center justify-between text-xs text-white/40 mt-6 pt-4 border-t border-white/5">
            <span>SSID: {phoneData.telemetry.ap_ssid}</span>
            <span className={phoneData.isConnected ? 'text-emerald-400' : 'text-amber-400'}>
              ● {phoneData.isConnected ? 'ESP32 Connected' : 'Waiting for ESP32'}
            </span>
          </div>
        </div>
      </div>

      {/* ── Active Devices Preview Table ───────────────────────────── */}
      <div className="space-y-3">
        <div className="flex items-center justify-between px-1">
          <h3 className="text-xs font-semibold uppercase tracking-wider text-white/60">
            Active Devices in Room ({phoneData.telemetry.devices.length})
          </h3>
          <button
            onClick={() => onSelectTab('phones')}
            className="text-xs text-sky-400 hover:underline flex items-center gap-1"
          >
            View Full Traffic Details →
          </button>
        </div>
        <PhoneTable devices={phoneData.telemetry.devices} />
      </div>
    </div>
  );
}
