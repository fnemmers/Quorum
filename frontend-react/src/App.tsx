import { useState } from 'react';
import StatusBar from './components/StatusBar';
import TickEvaluationPanel from './components/TickEvaluationPanel';
import BatesBacktestPanel from './components/BatesBacktestPanel';
import OptionRankingPanel from './components/OptionRankingPanel';

type Tab = 'live' | 'backtest';

export default function App() {
  const [tab, setTab] = useState<Tab>('live');

  return (
    <div className="flex flex-col h-screen bg-surface text-ink overflow-hidden">
      <StatusBar />

      <div className="flex gap-1 px-2 pt-1 border-b border-border">
        <LiveTabButton     active={tab === 'live'}     onClick={() => setTab('live')} />
        <BacktestTabButton active={tab === 'backtest'} onClick={() => setTab('backtest')} />
      </div>

      {tab === 'live'     && <LiveTab />}
      {tab === 'backtest' && <BacktestTab />}
    </div>
  );
}

/* ── Live tab — per-symbol Bates + news-jump workspace ─────────── */

function LiveTab() {
  return (
    <div className="flex-1 overflow-y-auto p-2">
      <TickEvaluationPanel />
    </div>
  );
}

/* ── Backtest tab — Bates option-level backtest + fused ranking ─
 *
 * Options data is synthetic; the math is real.
 *
 *   Bates Grid    — raw per-option table (edge, greeks, hedged P&L)
 *   Fused Ranking — Phase 4 z-score fusion:
 *                     w_edge·z_edge + w_news·z_news + w_convex·z_cvx
 */

type BacktestSubTab = 'grid' | 'ranking';

function BacktestTab() {
  const [sub, setSub] = useState<BacktestSubTab>('grid');
  return (
    <div className="flex-1 flex flex-col overflow-hidden">
      <div className="flex gap-1 px-2 pt-1 border-b border-border">
        <BacktestSubTabButton label="Bates Grid"
          active={sub === 'grid'}    onClick={() => setSub('grid')} />
        <BacktestSubTabButton label="Fused Ranking (Q−P)"
          active={sub === 'ranking'} onClick={() => setSub('ranking')} />
      </div>
      <div className="flex-1 overflow-y-auto p-2">
        {sub === 'grid'    && <BatesBacktestPanel />}
        {sub === 'ranking' && <OptionRankingPanel />}
      </div>
    </div>
  );
}

function BacktestSubTabButton({ label, active, onClick }:
  { label: string; active: boolean; onClick: () => void }) {
  return (
    <button
      onClick={onClick}
      className={`px-3 py-1.5 text-xs uppercase tracking-widest font-bold rounded-t border border-b-0 ${
        active
          ? 'bg-panel border-border text-ink'
          : 'bg-surface border-transparent text-subtle hover:text-ink'
      }`}
    >
      {label}
    </button>
  );
}

function LiveTabButton({ active, onClick }: { active: boolean; onClick: () => void }) {
  return (
    <button
      onClick={onClick}
      className={`px-4 py-1.5 text-xs uppercase tracking-widest font-bold rounded-t border border-b-0 flex items-center gap-2 ${
        active
          ? 'bg-panel border-border text-ink'
          : 'bg-surface border-transparent text-subtle hover:text-ink'
      }`}
    >
      <span
        className="inline-block w-2 h-2 rounded-full bg-bear"
        style={{
          boxShadow: active ? '0 0 6px #ff3030' : 'none',
          animation: active ? 'pulse 1.4s ease-in-out infinite' : 'none',
        }}
      />
      Live
    </button>
  );
}

function BacktestTabButton({ active, onClick }: { active: boolean; onClick: () => void }) {
  return (
    <button
      onClick={onClick}
      className={`px-4 py-1.5 text-xs uppercase tracking-widest font-bold rounded-t border border-b-0 flex items-center gap-1 ${
        active
          ? 'bg-panel border-border text-ink'
          : 'bg-surface border-transparent text-subtle hover:text-ink'
      }`}
    >
      Backtest
      <span className="text-accent">←</span>
    </button>
  );
}
