'use client';

import { useState } from 'react';
import { DeviceTelemetry } from '@/types/occupancy';
import { UserCheck, UserPlus, ShieldAlert, ShieldCheck, ArrowUpRight, ArrowDownLeft, Wifi, Smartphone } from 'lucide-react';
import { StudentRegistryModal } from './StudentRegistryModal';

interface PhoneTableProps {
  devices: DeviceTelemetry[];
  onRefresh?: () => void;
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function formatBps(bps: number): string {
  if (bps < 1000) return `${bps} bps`;
  if (bps < 1000000) return `${(bps / 1000).toFixed(1)} kbps`;
  return `${(bps / 1000000).toFixed(1)} Mbps`;
}

export function PhoneTable({ devices, onRefresh }: PhoneTableProps) {
  const [selectedMac, setSelectedMac] = useState<string | null>(null);

  return (
    <>
      <div className="overflow-x-auto rounded-2xl border border-slate-800 bg-slate-900/60 backdrop-blur-sm shadow-xl">
        <table className="w-full text-left text-xs text-slate-300">
          <thead className="bg-slate-900/90 text-slate-400 uppercase tracking-wider text-[10px] border-b border-slate-800">
            <tr>
              <th scope="col" className="px-4 py-3.5 font-semibold">Student / Device</th>
              <th scope="col" className="px-4 py-3.5 font-semibold">MAC & IP Address</th>
              <th scope="col" className="px-4 py-3.5 font-semibold">Classroom Status</th>
              <th scope="col" className="px-4 py-3.5 font-semibold">Outgoing (TX)</th>
              <th scope="col" className="px-4 py-3.5 font-semibold">Incoming (RX)</th>
              <th scope="col" className="px-4 py-3.5 font-semibold">Rate</th>
              <th scope="col" className="px-4 py-3.5 font-semibold text-right">Action</th>
            </tr>
          </thead>
          <tbody className="divide-y divide-slate-800/60">
            {devices.length === 0 ? (
              <tr>
                <td colSpan={7} className="px-4 py-12 text-center text-slate-500">
                  <div className="flex flex-col items-center justify-center gap-2">
                    <Smartphone className="w-8 h-8 text-slate-600 animate-pulse" />
                    <p className="text-sm font-medium text-slate-400">No phones currently connected</p>
                    <p className="text-xs text-slate-500">Connect a device to WiFi SSID 'PBL-MC' to begin monitoring</p>
                  </div>
                </td>
              </tr>
            ) : (
              devices.map((device) => {
                const macAddress = device.mac || device.id;
                const isRegistered = Boolean(device.student_name);
                const isViolation = device.compliance_status === 'violation_active' || device.compliance_status === 'unregistered_active';

                return (
                  <tr
                    key={device.id}
                    className={`transition-colors hover:bg-slate-800/40 ${
                      isViolation ? 'bg-red-500/[0.04]' : ''
                    }`}
                  >
                    {/* Student Identity */}
                    <td className="px-4 py-3.5 whitespace-nowrap">
                      <div className="flex items-center gap-2.5">
                        <div className={`p-2 rounded-xl border ${
                          isRegistered
                            ? 'bg-indigo-500/10 border-indigo-500/20 text-indigo-400'
                            : 'bg-slate-800 border-slate-700 text-slate-400'
                        }`}>
                          <Smartphone className="w-4 h-4" />
                        </div>
                        <div>
                          {isRegistered ? (
                            <>
                              <div className="font-semibold text-white flex items-center gap-1.5">
                                <span>{device.student_name}</span>
                                <UserCheck className="w-3.5 h-3.5 text-emerald-400 shrink-0" />
                              </div>
                              <div className="text-[11px] text-slate-400">
                                {device.student_roll} • {device.device_name || 'Phone'}
                              </div>
                            </>
                          ) : (
                            <>
                              <div className="font-medium text-slate-400 italic">
                                Unregistered Device
                              </div>
                              <div className="text-[11px] text-slate-500">
                                ID #{device.id}
                              </div>
                            </>
                          )}
                        </div>
                      </div>
                    </td>

                    {/* Hardware MAC & IP */}
                    <td className="px-4 py-3.5 whitespace-nowrap">
                      <div className="font-mono text-xs text-slate-200">
                        {macAddress}
                      </div>
                      <div className="text-[11px] text-slate-500 flex items-center gap-1">
                        <Wifi className="w-3 h-3 text-slate-400" />
                        {device.ip || 'DHCP Assigned'}
                        {device.rssi && <span className="text-[10px]">({device.rssi} dBm)</span>}
                      </div>
                    </td>

                    {/* Classroom Policy Compliance Status */}
                    <td className="px-4 py-3.5 whitespace-nowrap">
                      {device.compliance_status === 'violation_active' ? (
                        <span className="inline-flex items-center gap-1.5 px-2.5 py-1 rounded-full text-[10px] font-bold uppercase bg-red-500/20 text-red-300 border border-red-500/30 animate-pulse">
                          <ShieldAlert className="w-3 h-3 text-red-400" />
                          In-Use Violation
                        </span>
                      ) : device.compliance_status === 'unregistered_active' ? (
                        <span className="inline-flex items-center gap-1.5 px-2.5 py-1 rounded-full text-[10px] font-bold uppercase bg-amber-500/20 text-amber-300 border border-amber-500/30">
                          <ShieldAlert className="w-3 h-3 text-amber-400" />
                          Unregistered Active
                        </span>
                      ) : device.compliance_status === 'compliant_idle' ? (
                        <span className="inline-flex items-center gap-1.5 px-2.5 py-1 rounded-full text-[10px] font-semibold bg-emerald-500/10 text-emerald-300 border border-emerald-500/20">
                          <ShieldCheck className="w-3 h-3 text-emerald-400" />
                          Compliant (Idle)
                        </span>
                      ) : (
                        <span className="inline-flex items-center gap-1.5 px-2.5 py-1 rounded-full text-[10px] font-medium bg-slate-800 text-slate-400 border border-slate-700">
                          Off Hours
                        </span>
                      )}
                    </td>

                    {/* Outgoing TX Traffic */}
                    <td className="px-4 py-3.5 whitespace-nowrap">
                      <div className="flex items-center gap-1 text-slate-300">
                        <ArrowUpRight className="w-3.5 h-3.5 text-blue-400" />
                        <span className="font-mono">{formatBytes(device.up_bytes)}</span>
                      </div>
                      <span className="text-[10px] text-slate-500">{device.up_pkts} pkts</span>
                    </td>

                    {/* Incoming RX Traffic */}
                    <td className="px-4 py-3.5 whitespace-nowrap">
                      <div className="flex items-center gap-1 text-slate-300">
                        <ArrowDownLeft className="w-3.5 h-3.5 text-purple-400" />
                        <span className="font-mono">{formatBytes(device.down_bytes)}</span>
                      </div>
                      <span className="text-[10px] text-slate-500">{device.down_pkts} pkts</span>
                    </td>

                    {/* Current Transfer Rate */}
                    <td className="px-4 py-3.5 whitespace-nowrap font-mono text-slate-300">
                      {formatBps(device.rate_bps)}
                    </td>

                    {/* Action: Quick Link to Student */}
                    <td className="px-4 py-3.5 whitespace-nowrap text-right">
                      <button
                        onClick={() => setSelectedMac(macAddress)}
                        className={`inline-flex items-center gap-1 px-2.5 py-1.5 rounded-lg text-xs font-medium transition-all ${
                          isRegistered
                            ? 'text-slate-400 hover:text-white hover:bg-slate-800 border border-slate-800'
                            : 'bg-indigo-600/20 hover:bg-indigo-600/30 text-indigo-300 border border-indigo-500/30'
                        }`}
                      >
                        {isRegistered ? (
                          <>Edit</>
                        ) : (
                          <>
                            <UserPlus className="w-3.5 h-3.5 text-indigo-400" />
                            <span>Assign</span>
                          </>
                        )}
                      </button>
                    </td>
                  </tr>
                );
              })
            )}
          </tbody>
        </table>
      </div>

      {selectedMac && (
        <StudentRegistryModal
          isOpen={Boolean(selectedMac)}
          initialMac={selectedMac}
          onClose={() => setSelectedMac(null)}
          onSuccess={() => {
            setSelectedMac(null);
            if (onRefresh) onRefresh();
          }}
        />
      )}
    </>
  );
}
