import { useStore } from '../store/useStore';

export default function StatusBar() {
  const { bridgeConnected } = useStore();

  return (
    <div className="flex items-center justify-between px-4 py-2 bg-grey border-b border-border text-sm">
      <div className="flex items-center gap-4">
        <span className="text-accent font-bold tracking-widest">QUORUM</span>
        <div className="flex items-center gap-1.5">
          <span className={`w-2 h-2 rounded-full ${bridgeConnected ? 'bg-bull' : 'bg-bear'}`} />
          <span className={bridgeConnected ? 'text-bull' : 'text-bear'}>
            {bridgeConnected ? 'LIVE' : 'OFFLINE'}
          </span>
        </div>
        <span className="text-subtle text-xs">Bates + AI-Jump Diffusion</span>
      </div>
      <div className="text-subtle text-xs">
        {new Date().toLocaleTimeString()}
      </div>
    </div>
  );
}
