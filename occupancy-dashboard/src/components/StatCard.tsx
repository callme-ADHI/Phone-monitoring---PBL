import type { LucideIcon } from 'lucide-react';

interface Props {
  icon: LucideIcon;
  label: string;
  value: string | number;
  subtext?: string;
}

export function StatCard({ icon: Icon, label, value, subtext }: Props) {
  return (
    <div className="relative overflow-hidden rounded-2xl border border-white/10 bg-white/[0.04] backdrop-blur-xl p-5">
      {/* Inner top highlight — characteristic glassmorphism detail */}
      <div className="absolute inset-x-0 top-0 h-px bg-gradient-to-r from-transparent via-white/20 to-transparent" />

      <div className="flex items-start justify-between gap-3">
        <div className="min-w-0 flex-1">
          <p className="text-[10px] text-white/35 uppercase tracking-widest mb-2">{label}</p>
          <p className="text-2xl font-bold text-white truncate">{value}</p>
          {subtext && <p className="text-[11px] text-white/25 mt-1">{subtext}</p>}
        </div>
        <div className="shrink-0 p-2.5 rounded-xl bg-white/5">
          <Icon className="w-4 h-4 text-white/35" />
        </div>
      </div>
    </div>
  );
}
