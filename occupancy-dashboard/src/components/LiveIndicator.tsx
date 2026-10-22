'use client';

interface Props {
  secondsSince: number;
  isConnected?: boolean;
}

export function LiveIndicator({ secondsSince, isConnected = true }: Props) {
  return (
    <div className="flex items-center gap-3 text-sm text-white/40 select-none">
      <span className="flex items-center gap-1.5">
        <span className="relative flex h-2 w-2">
          {isConnected && (
            <span className="animate-ping absolute inline-flex h-full w-full rounded-full bg-emerald-400 opacity-75" />
          )}
          <span
            className={`relative inline-flex h-2 w-2 rounded-full ${
              isConnected ? 'bg-emerald-500' : 'bg-amber-500'
            }`}
          />
        </span>
        <span
          className={`font-semibold tracking-wide ${
            isConnected ? 'text-emerald-400' : 'text-amber-400'
          }`}
        >
          {isConnected ? 'Camera Live' : 'Camera Disconnected'}
        </span>
      </span>
      <span className="text-white/20">·</span>
      <span>{isConnected ? `Updated ${secondsSince}s ago` : 'Waiting for Python...'}</span>
    </div>
  );
}
