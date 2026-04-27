import { useStore } from '../store/useStore';

export default function RiskPanel() {
  const {
    maxPositionPct, setMaxPosition,
    maxDrawdownPct, setMaxDrawdown,
    killSwitch, toggleKillSwitch,
    holdings, quotes,
  } = useStore();

  const equity = holdings.reduce((acc, h) => {
    const cur = quotes[h.symbol]?.price ?? h.avg_price;
    return acc + cur * h.shares;
  }, 0);

  const totalCost = holdings.reduce((acc, h) => acc + h.avg_price * h.shares, 0);
  const drawdown  = totalCost > 0 ? ((equity - totalCost) / totalCost * 100) : 0;
  const ddTripped = drawdown < -maxDrawdownPct;

  return (
    <div className="bg-panel border border-border rounded p-3 space-y-3">
      <div className="text-xs text-subtle uppercase tracking-widest font-bold">Risk Controls</div>

      <div className="space-y-2 text-xs font-mono">
        <div>
          <div className="flex justify-between mb-1">
            <span className="text-muted">Max Position Size</span>
            <span className="text-ink">{maxPositionPct}%</span>
          </div>
          <input
            type="range" min={1} max={50} value={maxPositionPct}
            onChange={(e) => setMaxPosition(Number(e.target.value))}
            className="w-full accent-accent"
          />
        </div>

        <div>
          <div className="flex justify-between mb-1">
            <span className="text-muted">Drawdown Stop</span>
            <span className={ddTripped ? 'text-bear animate-pulse' : 'text-ink'}>
              {maxDrawdownPct}% {ddTripped && '⚠ TRIPPED'}
            </span>
          </div>
          <input
            type="range" min={1} max={30} value={maxDrawdownPct}
            onChange={(e) => setMaxDrawdown(Number(e.target.value))}
            className="w-full accent-accent"
          />
        </div>

        <div className="flex justify-between items-center border-t border-border pt-2">
          <div>
            <div className="text-muted">Portfolio Equity</div>
            <div className="text-ink">${equity.toFixed(2)}</div>
          </div>
          <div>
            <div className="text-muted">Drawdown</div>
            <div className={drawdown >= 0 ? 'text-bull' : 'text-bear'}>
              {drawdown >= 0 ? '+' : ''}{drawdown.toFixed(2)}%
            </div>
          </div>
        </div>
      </div>

      <button
        onClick={toggleKillSwitch}
        className={`w-full py-1.5 rounded font-bold text-sm transition ${
          killSwitch
            ? 'bg-bear text-ink animate-pulse'
            : 'bg-surface border border-bear text-bear hover:bg-bear hover:text-ink'
        }`}
      >
        {killSwitch ? '⚡ KILL SWITCH — CLICK TO RESUME' : 'KILL SWITCH'}
      </button>
    </div>
  );
}
