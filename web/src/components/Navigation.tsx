'use client';

import { Users, Smartphone, Layers } from 'lucide-react';

export type DashboardTab = 'combined' | 'camera' | 'phones';

interface Props {
  activeTab: DashboardTab;
  onChangeTab: (tab: DashboardTab) => void;
  isCameraConnected: boolean;
  isPhoneConnected: boolean;
}

export function Navigation({
  activeTab,
  onChangeTab,
  isCameraConnected,
  isPhoneConnected,
}: Props) {
  const tabs = [
    {
      id: 'combined' as DashboardTab,
      label: 'Combined View',
      icon: Layers,
      desc: 'People + Phones',
    },
    {
      id: 'camera' as DashboardTab,
      label: 'Person Counter',
      icon: Users,
      desc: 'Webcam YOLO',
      status: isCameraConnected,
    },
    {
      id: 'phones' as DashboardTab,
      label: 'Phone Monitor',
      icon: Smartphone,
      desc: 'ESP32 Router',
      status: isPhoneConnected,
    },
  ];

  return (
    <div className="w-full max-w-5xl flex flex-col sm:flex-row items-center justify-between gap-4 mb-8 bg-white/[0.03] border border-white/10 rounded-2xl p-2.5 backdrop-blur-xl">
      <div className="flex items-center gap-1.5 w-full sm:w-auto overflow-x-auto">
        {tabs.map((t) => {
          const Icon = t.icon;
          const isActive = activeTab === t.id;
          return (
            <button
              key={t.id}
              onClick={() => onChangeTab(t.id)}
              className={`flex items-center gap-2.5 px-4 py-2 rounded-xl text-xs font-semibold transition-all select-none whitespace-nowrap ${
                isActive
                  ? 'bg-white/15 text-white shadow-lg border border-white/15'
                  : 'text-white/50 hover:text-white hover:bg-white/5'
              }`}
            >
              <Icon className="w-4 h-4" />
              <span>{t.label}</span>
              {t.status !== undefined && (
                <span
                  className={`w-2 h-2 rounded-full ${
                    t.status ? 'bg-emerald-400 shadow-[0_0_8px_rgba(52,211,153,0.8)]' : 'bg-white/20'
                  }`}
                  title={t.status ? 'Connected' : 'Disconnected'}
                />
              )}
            </button>
          );
        })}
      </div>

      <div className="flex items-center gap-4 text-[11px] text-white/40 px-2">
        <div className="flex items-center gap-1.5">
          <span
            className={`w-2 h-2 rounded-full ${
              isCameraConnected ? 'bg-emerald-400' : 'bg-red-400'
            }`}
          />
          <span>Camera {isCameraConnected ? 'Live' : 'Offline'}</span>
        </div>
        <span>•</span>
        <div className="flex items-center gap-1.5">
          <span
            className={`w-2 h-2 rounded-full ${
              isPhoneConnected ? 'bg-emerald-400' : 'bg-amber-400'
            }`}
          />
          <span>ESP32 {isPhoneConnected ? 'Live' : 'Waiting'}</span>
        </div>
      </div>
    </div>
  );
}
