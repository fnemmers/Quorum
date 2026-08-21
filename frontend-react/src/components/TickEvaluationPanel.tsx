import { useState } from 'react';
import { useStore } from '../store/useStore';
import Chart from './Chart';
import MCPathBundlePanel from './MCPathBundlePanel';
import HestonSurfacePanel from './HestonSurfacePanel';
import HestonDiagnosticsPanel from './HestonDiagnosticsPanel';
import NewsJumpPanel from './NewsJumpPanel';

/*
 * TickEvaluationPanel  --  Per-symbol Bates + news-jump workspace.
 *
 * User picks (or types) a ticker; the panel loads its:
 *   - Candlestick chart (Polygon daily bars + live quote overlay)
 *   - MC Path Bundle (Bates paths with news-jump overlay applied)
 *   - Heston-implied vol surface (Bates CF pricing)
 *   - Heston / Bates calibration diagnostics
 *   - News-jump signal that modulated the underlying params
 */

export default function TickEvaluationPanel() {
  const { sp500Tickers, selectedSymbol, selectSymbol } = useStore();
  const [typed, setTyped] = useState('');

  const commit = () => {
    const s = typed.trim().toUpperCase();
    if (!s) return;
    selectSymbol(s, 30);
  };

  return (
    <div className="bg-panel border border-border rounded flex flex-col overflow-hidden">
      {/* Header + symbol picker */}
      <div className="px-3 py-2 border-b border-border flex items-center justify-between flex-wrap gap-2">
        <div className="text-xs text-subtle uppercase tracking-widest font-bold">
          Tick Evaluation
          {selectedSymbol && (
            <span className="text-accent ml-2 normal-case">· {selectedSymbol}</span>
          )}
        </div>
        <div className="flex items-center gap-2 text-xs font-mono">
          <span className="text-subtle">Ticker</span>
          <input
            list="sp500-list"
            type="text"
            value={typed}
            onChange={(e) => setTyped(e.target.value)}
            onKeyDown={(e) => { if (e.key === 'Enter') commit(); }}
            placeholder="AAPL"
            className="bg-surface border border-border rounded px-2 py-0.5 text-ink w-24 uppercase"
          />
          <datalist id="sp500-list">
            {sp500Tickers.map((t) => <option key={t} value={t} />)}
          </datalist>
          <button
            onClick={commit}
            className="bg-accent text-black text-xs font-bold px-3 py-1 rounded"
          >Load</button>
          {selectedSymbol && (
            <button
              onClick={() => selectSymbol(null)}
              className="text-subtle hover:text-ink text-xs ml-2"
            >clear</button>
          )}
        </div>
      </div>

      {/* Candlestick + MC Paths + Vol Surface */}
      <div className="border-t border-border p-2 grid grid-cols-3 gap-2 min-h-[280px]">
        <Chart compact />
        <MCPathBundlePanel />
        <HestonSurfacePanel />
      </div>

      {/* Heston calibration diagnostics */}
      {selectedSymbol && (
        <div className="border-t border-border p-2">
          <HestonDiagnosticsPanel />
        </div>
      )}

      {/* News → Bates jump-parameter overlay */}
      <div className="border-t border-border p-2">
        <NewsJumpPanel />
      </div>
    </div>
  );
}
