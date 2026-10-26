'use client';

import { useState } from 'react';
import { X, Clock, Check, AlertCircle } from 'lucide-react';
import { ClassScheduleInfo } from '@/types/occupancy';

interface Props {
  isOpen: boolean;
  onClose: () => void;
  currentSchedule?: ClassScheduleInfo | null;
  onSaved: () => void;
}

export function ScheduleModal({ isOpen, onClose, currentSchedule, onSaved }: Props) {
  const [startTime, setStartTime] = useState(currentSchedule?.start_time || '08:00');
  const [endTime, setEndTime] = useState(currentSchedule?.end_time || '18:00');
  const [subjectName, setSubjectName] = useState(currentSchedule?.subject_name || 'Lecture Session');
  const [classroom, setClassroom] = useState(currentSchedule?.classroom || 'Room 101');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  if (!isOpen) return null;

  const handleSave = async (e: React.FormEvent) => {
    e.preventDefault();
    setLoading(true);
    setError(null);

    try {
      const res = await fetch('/api/schedule', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          id: currentSchedule?.id || 'b2a50563-da33-47ad-822d-edc0e95243f9',
          start_time: startTime.length === 5 ? `${startTime}:00` : startTime,
          end_time: endTime.length === 5 ? `${endTime}:00` : endTime,
          subject_name: subjectName,
          classroom: classroom,
          days_of_week: [0, 1, 2, 3, 4, 5, 6], // Active all days as requested
        }),
      });

      if (!res.ok) {
        const d = await res.json();
        throw new Error(d.error || 'Failed to update schedule');
      }

      // Also forward timing to ESP32 for STM32 sync
      try {
        await fetch('/api/esp32/schedule', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ start_time: startTime, end_time: endTime }),
        });
      } catch {
        // ESP32 optional route
      }

      onSaved();
      onClose();
    } catch (err: any) {
      setError(err.message || 'Error updating schedule');
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center p-4 bg-black/75 backdrop-blur-sm animate-in fade-in">
      <div className="relative w-full max-w-md p-6 bg-slate-900 border border-slate-800 rounded-3xl shadow-2xl">
        <button
          onClick={onClose}
          className="absolute top-5 right-5 p-2 rounded-xl text-slate-400 hover:text-white hover:bg-slate-800 transition-colors"
        >
          <X className="w-5 h-5" />
        </button>

        <div className="flex items-center gap-3 mb-5">
          <div className="p-3 rounded-2xl bg-indigo-500/10 border border-indigo-500/20 text-indigo-400">
            <Clock className="w-6 h-6" />
          </div>
          <div>
            <h3 className="text-base font-bold text-white">Configure Class Schedule</h3>
            <p className="text-xs text-slate-400">Syncs instantly with Web, ESP32 & STM32</p>
          </div>
        </div>

        {error && (
          <div className="mb-4 p-3 rounded-xl bg-red-500/10 border border-red-500/20 flex items-center gap-2 text-xs text-red-400">
            <AlertCircle className="w-4 h-4 shrink-0" />
            <span>{error}</span>
          </div>
        )}

        <form onSubmit={handleSave} className="space-y-4">
          <div className="grid grid-cols-2 gap-3">
            <div>
              <label className="block text-xs font-medium text-slate-300 mb-1">Start Time</label>
              <input
                type="time"
                value={startTime}
                onChange={(e) => setStartTime(e.target.value)}
                required
                className="w-full px-3 py-2 rounded-xl bg-slate-950 border border-slate-700 font-mono text-sm text-white focus:outline-none focus:border-indigo-500"
              />
            </div>
            <div>
              <label className="block text-xs font-medium text-slate-300 mb-1">End Time</label>
              <input
                type="time"
                value={endTime}
                onChange={(e) => setEndTime(e.target.value)}
                required
                className="w-full px-3 py-2 rounded-xl bg-slate-950 border border-slate-700 font-mono text-sm text-white focus:outline-none focus:border-indigo-500"
              />
            </div>
          </div>

          <div>
            <label className="block text-xs font-medium text-slate-300 mb-1">Subject / Session</label>
            <input
              type="text"
              value={subjectName}
              onChange={(e) => setSubjectName(e.target.value)}
              className="w-full px-3 py-2 rounded-xl bg-slate-950 border border-slate-700 text-sm text-white focus:outline-none focus:border-indigo-500"
              placeholder="e.g. Computer Networks Lab"
            />
          </div>

          <div>
            <label className="block text-xs font-medium text-slate-300 mb-1">Room / Hall</label>
            <input
              type="text"
              value={classroom}
              onChange={(e) => setClassroom(e.target.value)}
              className="w-full px-3 py-2 rounded-xl bg-slate-950 border border-slate-700 text-sm text-white focus:outline-none focus:border-indigo-500"
              placeholder="e.g. Lab 304"
            />
          </div>

          <div className="p-3 rounded-xl bg-slate-950/60 border border-slate-800 text-[11px] text-slate-400">
            ℹ️ <span className="text-slate-300 font-medium">Daily Active:</span> All 7 days are set active. The ESP32 and STM32 will monitor devices and light the status LEDs during this window.
          </div>

          <div className="flex items-center justify-end gap-2 pt-2">
            <button
              type="button"
              onClick={onClose}
              className="px-4 py-2 rounded-xl text-xs font-medium text-slate-400 hover:text-white hover:bg-slate-800 transition-colors"
            >
              Cancel
            </button>
            <button
              type="submit"
              disabled={loading}
              className="flex items-center gap-1.5 px-4 py-2 rounded-xl text-xs font-bold text-white bg-indigo-600 hover:bg-indigo-500 transition-colors disabled:opacity-50"
            >
              {loading ? (
                'Saving...'
              ) : (
                <>
                  <Check className="w-3.5 h-3.5" />
                  <span>Update Class Timing</span>
                </>
              )}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
