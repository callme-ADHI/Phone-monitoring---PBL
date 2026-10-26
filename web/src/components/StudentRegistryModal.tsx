'use client';

import { useState } from 'react';
import { X, Smartphone, UserCheck, ShieldAlert } from 'lucide-react';

interface Props {
  initialMac?: string;
  isOpen: boolean;
  onClose: () => void;
  onSuccess: () => void;
}

export function StudentRegistryModal({ initialMac = '', isOpen, onClose, onSuccess }: Props) {
  const [mac, setMac] = useState(initialMac);
  const [rollNo, setRollNo] = useState('');
  const [name, setName] = useState('');
  const [deviceName, setDeviceName] = useState('Personal Phone');
  const [department, setDepartment] = useState('Computer Science');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  if (!isOpen) return null;

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setLoading(true);
    setError(null);

    try {
      const res = await fetch('/api/students', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          mac_address: mac,
          student_roll: rollNo,
          student_name: name,
          device_name: deviceName,
          department,
        }),
      });

      const data = await res.json();
      if (!res.ok) throw new Error(data.error || 'Failed to map device');

      onSuccess();
      onClose();
    } catch (err: any) {
      setError(err.message);
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center p-4 bg-black/70 backdrop-blur-sm animate-in fade-in">
      <div className="relative w-full max-w-md p-6 bg-slate-900 border border-slate-700/80 rounded-2xl shadow-2xl text-slate-100">
        <button
          onClick={onClose}
          className="absolute top-4 right-4 p-1.5 rounded-lg text-slate-400 hover:text-white hover:bg-slate-800 transition-colors"
        >
          <X className="w-5 h-5" />
        </button>

        <div className="flex items-center gap-3 mb-5">
          <div className="p-2.5 rounded-xl bg-indigo-500/10 border border-indigo-500/20 text-indigo-400">
            <Smartphone className="w-6 h-6" />
          </div>
          <div>
            <h2 className="text-lg font-semibold text-white">Assign Device to Student</h2>
            <p className="text-xs text-slate-400">Map phone MAC address to student roster in Supabase</p>
          </div>
        </div>

        {error && (
          <div className="flex items-center gap-2 p-3 mb-4 rounded-xl bg-red-500/10 border border-red-500/20 text-red-400 text-xs">
            <ShieldAlert className="w-4 h-4 shrink-0" />
            <span>{error}</span>
          </div>
        )}

        <form onSubmit={handleSubmit} className="space-y-4">
          <div>
            <label className="block text-xs font-medium text-slate-300 mb-1">
              Hardware MAC Address
            </label>
            <input
              type="text"
              required
              value={mac}
              onChange={(e) => setMac(e.target.value)}
              placeholder="AA:BB:CC:DD:EE:FF"
              className="w-full px-3.5 py-2 text-sm bg-slate-800/80 border border-slate-700 rounded-xl text-white font-mono placeholder:text-slate-500 focus:outline-none focus:border-indigo-500 focus:ring-1 focus:ring-indigo-500"
            />
          </div>

          <div className="grid grid-cols-2 gap-3">
            <div>
              <label className="block text-xs font-medium text-slate-300 mb-1">
                Roll Number
              </label>
              <input
                type="text"
                required
                value={rollNo}
                onChange={(e) => setRollNo(e.target.value)}
                placeholder="CS2026-001"
                className="w-full px-3.5 py-2 text-sm bg-slate-800/80 border border-slate-700 rounded-xl text-white placeholder:text-slate-500 focus:outline-none focus:border-indigo-500"
              />
            </div>
            <div>
              <label className="block text-xs font-medium text-slate-300 mb-1">
                Device Name
              </label>
              <input
                type="text"
                value={deviceName}
                onChange={(e) => setDeviceName(e.target.value)}
                placeholder="iPhone / Galaxy"
                className="w-full px-3.5 py-2 text-sm bg-slate-800/80 border border-slate-700 rounded-xl text-white placeholder:text-slate-500 focus:outline-none focus:border-indigo-500"
              />
            </div>
          </div>

          <div>
            <label className="block text-xs font-medium text-slate-300 mb-1">
              Student Full Name
            </label>
            <input
              type="text"
              required
              value={name}
              onChange={(e) => setName(e.target.value)}
              placeholder="e.g. Aditya Sharma"
              className="w-full px-3.5 py-2 text-sm bg-slate-800/80 border border-slate-700 rounded-xl text-white placeholder:text-slate-500 focus:outline-none focus:border-indigo-500"
            />
          </div>

          <div>
            <label className="block text-xs font-medium text-slate-300 mb-1">
              Department
            </label>
            <select
              value={department}
              onChange={(e) => setDepartment(e.target.value)}
              className="w-full px-3.5 py-2 text-sm bg-slate-800/80 border border-slate-700 rounded-xl text-white focus:outline-none focus:border-indigo-500"
            >
              <option value="Computer Science">Computer Science</option>
              <option value="Information Technology">Information Technology</option>
              <option value="Electronics & Comm.">Electronics & Comm.</option>
              <option value="Mechanical">Mechanical</option>
              <option value="General">General</option>
            </select>
          </div>

          <div className="flex items-center justify-end gap-3 pt-3 border-t border-slate-800">
            <button
              type="button"
              onClick={onClose}
              className="px-4 py-2 text-xs font-medium rounded-xl text-slate-400 hover:text-white hover:bg-slate-800 transition-colors"
            >
              Cancel
            </button>
            <button
              type="submit"
              disabled={loading}
              className="flex items-center gap-2 px-5 py-2 text-xs font-semibold rounded-xl bg-indigo-600 hover:bg-indigo-500 text-white shadow-lg shadow-indigo-600/30 transition-all disabled:opacity-50"
            >
              <UserCheck className="w-4 h-4" />
              <span>{loading ? 'Saving...' : 'Save & Link MAC'}</span>
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
