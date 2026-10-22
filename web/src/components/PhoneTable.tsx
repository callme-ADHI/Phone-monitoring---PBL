'use client';

import type { DeviceTelemetry } from '@/types/occupancy';

interface Props {
  devices: DeviceTelemetry[];
}

function fmtBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1048576) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1048576).toFixed(2)} MB`;
}

export function PhoneTable({ devices }: Props) {
  return (
    <div className="relative overflow-hidden rounded-2xl border border-white/10 bg-white/[0.04] backdrop-blur-xl">
      <div className="p-5 border-b border-white/10 flex items-center justify-between flex-wrap gap-2">
        <div>
          <h3 className="text-sm font-semibold text-white">Connected Devices (PBL-MC)</h3>
          <p className="text-[11px] text-white/40 mt-0.5">
            Salted anonymous IDs • Unencrypted 802.11 header inspection only
          </p>
        </div>
        <div className="text-xs text-white/50">
          Total: <span className="text-white font-bold">{devices.length}</span> active
        </div>
      </div>

      <div className="overflow-x-auto">
        <table className="w-full text-left text-xs">
          <thead>
            <tr className="border-b border-white/10 bg-white/[0.02] text-white/40 uppercase tracking-wider text-[10px]">
              <th className="py-3 px-5">Anonymous ID</th>
              <th className="py-3 px-5">Uplink (Tx)</th>
              <th className="py-3 px-5">Downlink (Rx)</th>
              <th className="py-3 px-5">Recent Rate</th>
              <th className="py-3 px-5">Activity State</th>
              <th className="py-3 px-5">Last Active</th>
            </tr>
          </thead>
          <tbody className="divide-y divide-white/5">
            {devices.length === 0 ? (
              <tr>
                <td colSpan={6} className="py-8 text-center text-white/30 text-xs">
                  No devices currently associated with the ESP32 access point (PBL-MC)
                </td>
              </tr>
            ) : (
              devices.map((dev) => {
                let badgeStyle = 'bg-white/10 text-white/50';
                if (dev.usage === 'light') {
                  badgeStyle = 'bg-cyan-500/15 text-cyan-400 border border-cyan-500/30';
                } else if (dev.usage === 'active') {
                  badgeStyle = 'bg-orange-500/15 text-orange-400 border border-orange-500/30';
                }

                return (
                  <tr key={dev.id} className="hover:bg-white/[0.02] transition-colors">
                    <td className="py-3.5 px-5">
                      <span className="font-mono font-bold text-sky-400 bg-sky-500/10 border border-sky-500/20 px-2 py-0.5 rounded text-[11px]">
                        DEV-{dev.id}
                      </span>
                    </td>
                    <td className="py-3.5 px-5 text-white/80">
                      {dev.up_pkts.toLocaleString()}{' '}
                      <span className="text-white/35 text-[10px]">({fmtBytes(dev.up_bytes)})</span>
                    </td>
                    <td className="py-3.5 px-5 text-white/80">
                      {dev.down_pkts.toLocaleString()}{' '}
                      <span className="text-white/35 text-[10px]">({fmtBytes(dev.down_bytes)})</span>
                    </td>
                    <td className="py-3.5 px-5 font-mono text-white/90">
                      {(dev.rate_bps / 1024).toFixed(1)} KB/s
                    </td>
                    <td className="py-3.5 px-5">
                      <span
                        className={`inline-block px-2.5 py-0.5 rounded-full text-[10px] font-semibold uppercase tracking-wider ${badgeStyle}`}
                      >
                        {dev.usage}
                      </span>
                    </td>
                    <td className="py-3.5 px-5 text-white/40">{dev.idle_secs}s ago</td>
                  </tr>
                );
              })
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}
