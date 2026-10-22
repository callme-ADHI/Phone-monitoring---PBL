'use client';

import { motion } from 'framer-motion';
import { Wifi, Globe, Clock, Radio, ArrowUpRight, ArrowDownLeft } from 'lucide-react';
import type { ESP32Telemetry } from '@/types/occupancy';
import { PhoneTable } from './PhoneTable';
import { StatCard } from './StatCard';

interface Props {
  telemetry: ESP32Telemetry;
  secondsSinceUpdate: number;
}

function fmtBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1048576) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1048576).toFixed(2)} MB`;
}

function fmtUptime(sec: number): string {
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  if (h > 0) return `${h}h ${m}m ${s}s`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

export function PhoneDashboard({ telemetry, secondsSinceUpdate }: Props) {
  const totalUpBytes = telemetry.devices.reduce((acc, d) => acc + d.up_bytes, 0);
  const totalDownBytes = telemetry.devices.reduce((acc, d) => acc + d.down_bytes, 0);

  return (
    <div className="w-full max-w-5xl space-y-6">
      {/* ── Hero Phone Count ────────────────────────────────────────── */}
      <div className="flex flex-col items-center justify-center py-6 text-center">
        <div className="relative">
          <motion.div
            className="absolute rounded-full blur-[90px] -z-10 w-full h-full scale-150 bg-sky-500/15"
            animate={{ opacity: telemetry.connected_count > 0 ? 0.25 : 0.08 }}
            transition={{ duration: 0.8 }}
          />
          <span className="font-mono block font-black leading-none text-sky-400 text-7xl sm:text-9xl select-none">
            {telemetry.connected_count}
          </span>
        </div>
        <p className="text-xs text-white/35 uppercase tracking-widest mt-4">
          Phones Connected to <span className="text-white/60 font-semibold">{telemetry.ap_ssid}</span>
        </p>

        {/* Status badges */}
        <div className="flex items-center gap-2.5 mt-3 flex-wrap justify-center">
          <span
            className={`px-3 py-1 rounded-full text-[11px] font-semibold tracking-wider uppercase border ${
              telemetry.routing_status === 'active'
                ? 'bg-emerald-500/10 border-emerald-500/30 text-emerald-400'
                : 'bg-amber-500/10 border-amber-500/30 text-amber-400'
            }`}
          >
            Internet NAT: {telemetry.routing_status}
          </span>
          <span className="px-3 py-1 rounded-full text-[11px] font-semibold tracking-wider text-white/50 bg-white/5 border border-white/10">
            AP IP: {telemetry.ap_ip}
          </span>
          <span className="px-3 py-1 rounded-full text-[11px] font-semibold tracking-wider text-white/50 bg-white/5 border border-white/10">
            STA IP: {telemetry.sta_ip}
          </span>
        </div>
      </div>

      {/* ── Stat Cards Grid ─────────────────────────────────────────── */}
      <div className="grid grid-cols-2 lg:grid-cols-4 gap-4">
        <StatCard
          icon={Globe}
          label="Upstream Network"
          value={telemetry.sta_ssid}
          subtext={`Status: ${telemetry.sta_status}`}
        />
        <StatCard
          icon={Radio}
          label="WiFi Channel"
          value={`Ch ${telemetry.channel}`}
          subtext="AP/STA locked frequency"
        />
        <StatCard
          icon={ArrowUpRight}
          label="Total Upload"
          value={fmtBytes(totalUpBytes)}
          subtext="Clients to Internet"
        />
        <StatCard
          icon={ArrowDownLeft}
          label="Total Download"
          value={fmtBytes(totalDownBytes)}
          subtext="Internet to Clients"
        />
      </div>

      {/* ── Connected Devices Table ─────────────────────────────────── */}
      <PhoneTable devices={telemetry.devices} />

      {/* ── Telemetry Health Footer ─────────────────────────────────── */}
      <div className="text-center text-[11px] text-white/25 pt-2">
        ESP32 Uptime: {fmtUptime(telemetry.uptime_secs)} • Telemetry{' '}
        {telemetry.is_alive ? `updated ${secondsSinceUpdate}s ago` : 'waiting for ESP32 connection'}
      </div>
    </div>
  );
}
