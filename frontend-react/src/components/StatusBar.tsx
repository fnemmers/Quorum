import { useStore } from '../store/useStore';

export default function StatusBar() {
  const { bridgeConnected, killSwitch, symbol, setSymbol } = useStore();

  const symbols = ['AAPL', 'MSFT', 'GOOGL', 'AMZN', 'NVDA', 'TSLA', 'SPY'];

  return (
    <div className="flex items-center justify-between px-4 py-2 bg-panel border-b border-border text-sm">
      <div className="flex items-center gap-4">
        <span className="text-accent font-bold tracking-widest">STOCKAPP</span>
        <div className="flex items-center gap-1.5">
          <span className={`w-2 h-2 rounded-full ${bridgeConnected ? 'bg-bull' : 'bg-bear'}`} />
          <span className={bridgeConnected ? 'text-bull' : 'text-bear'}>
            {bridgeConnected ? 'LIVE' : 'OFFLINE'}
          </span>
        </div>
      </div>

      <div className="flex items-center gap-2">
        <span className="text-gray-500 text-xs">SYMBOL</span>
        <select
          value={symbol}
          onChange={(e) => setSymbol(e.target.value)}
          className="bg-surface border border-border text-white text-sm px-2 py-1 rounded focus:outline-none focus:border-accent"
        >
          {symbols.map((s) => (
            <option key={s} value={s}>{s}</option>
          ))}
        </select>
      </div>

      {killSwitch && (
        <div className="text-bear font-bold animate-pulse">⚠ KILL SWITCH ACTIVE</div>
      )}

      <div className="text-gray-500 text-xs">
        {new Date().toLocaleTimeString()}
      </div>
    </div>
  );
}
