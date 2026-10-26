'use client';

import { useState } from 'react';
import { motion, AnimatePresence } from 'framer-motion';

import { useCameraData } from '@/hooks/useCameraData';
import { usePhoneData } from '@/hooks/usePhoneData';
import { AmbientBackground } from './AmbientBackground';
import { Navigation, type DashboardTab } from './Navigation';
import { CombinedDashboard } from './CombinedDashboard';
import { CameraDashboard } from './CameraDashboard';
import { PhoneDashboard } from './PhoneDashboard';

interface Props {
  defaultTab?: DashboardTab;
}

export function Dashboard({ defaultTab = 'combined' }: Props) {
  const [activeTab, setActiveTab] = useState<DashboardTab>(defaultTab);

  const cameraData = useCameraData();
  const phoneData = usePhoneData();

  return (
    <div className="relative min-h-screen bg-[#07070f] text-white overflow-hidden selection:bg-cyan-500/30">
      <AmbientBackground />

      <main className="relative z-10 flex flex-col items-center min-h-screen px-4 sm:px-8 py-8">
        {/* ── Top Header ──────────────────────────────────────────────────────── */}
        <header className="w-full max-w-5xl flex items-start sm:items-center justify-between gap-4 mb-6">
          <div>
            <h1 className="text-[11px] font-semibold tracking-[0.25em] uppercase text-white/50">
              Room Intelligence System
            </h1>
            <p className="text-xs text-white/25 mt-0.5">
              Classroom A · Vision + Network Presence
            </p>
          </div>

          <div className="flex items-center gap-2">
            <span className="text-[11px] text-white/40 hidden sm:inline">PBL-MC Platform</span>
            <div className="w-1.5 h-1.5 rounded-full bg-cyan-400 shadow-[0_0_8px_rgba(34,211,238,0.8)]" />
          </div>
        </header>

        {/* ── Navigation Tabs ─────────────────────────────────────────────────── */}
        <Navigation
          activeTab={activeTab}
          onChangeTab={setActiveTab}
          isCameraConnected={cameraData.isConnected}
          isPhoneConnected={phoneData.isConnected}
        />

        {/* ── Dynamic Tab Content ─────────────────────────────────────────────── */}
        <AnimatePresence mode="wait">
          {activeTab === 'combined' && (
            <motion.div
              key="combined"
              initial={{ opacity: 0, y: 12 }}
              animate={{ opacity: 1, y: 0 }}
              exit={{ opacity: 0, y: -12 }}
              transition={{ duration: 0.2 }}
              className="w-full flex justify-center"
            >
              <CombinedDashboard
                cameraData={cameraData}
                phoneData={phoneData}
                onSelectTab={(tab) => setActiveTab(tab)}
              />
            </motion.div>
          )}

          {activeTab === 'camera' && (
            <motion.div
              key="camera"
              initial={{ opacity: 0, y: 12 }}
              animate={{ opacity: 1, y: 0 }}
              exit={{ opacity: 0, y: -12 }}
              transition={{ duration: 0.2 }}
              className="w-full flex justify-center"
            >
              <CameraDashboard cameraData={cameraData} />
            </motion.div>
          )}

          {activeTab === 'phones' && (
            <motion.div
              key="phones"
              initial={{ opacity: 0, y: 12 }}
              animate={{ opacity: 1, y: 0 }}
              exit={{ opacity: 0, y: -12 }}
              transition={{ duration: 0.2 }}
              className="w-full flex justify-center"
            >
              <PhoneDashboard
                data={phoneData.telemetry}
                onRefresh={phoneData.refresh}
              />
            </motion.div>
          )}
        </AnimatePresence>
      </main>
    </div>
  );
}
