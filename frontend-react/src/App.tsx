import { useState } from 'react';
import StatusBar from './components/StatusBar';
import PortfolioPanel from './components/PortfolioPanel';
import CompilationPanel from './components/CompilationPanel';
import TickEvaluationPanel from './components/TickEvaluationPanel';

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

/* ── Live tab ───────────────────────────────────────────────────
 *
 * Layout:
 *   ┌──────────┬──────────────────────────────┐
 *   │ Holdings │ Compilation (Result inside)  │
 *   ├──────────┴──────────────────────────────┤
 *   │ Tick Evaluation (full width)            │
 *   │   - Holdings rebalance table            │
 *   │   - Selected symbol detail card         │
 *   │   - MC Paths | Vol Surface              │
 *   │   - Debriefs                            │
 *   └────────────────────────────────────────┘
 *
 * Tick Eval is the per-symbol workspace. It reacts to whichever ticker
 * is selected (clicked from Result or its own holdings table) and
 * renders the obscurity score, MC path bundle and vol surface for
 * that symbol.
 */

function LiveTab() {
  return (
    <div className="flex-1 overflow-y-auto p-2 space-y-2">
      <div className="flex gap-2 items-start">
        <div className="w-80 flex flex-col gap-2">
          <SectionLabel>Holdings</SectionLabel>
          <PortfolioPanel />
        </div>

        <div className="flex-1 flex flex-col gap-2">
          <CompilationPanel />
        </div>
      </div>

      <div className="border-t border-border pt-2">
        <TickEvaluationPanel />
      </div>
    </div>
  );
}

function SectionLabel({ children }: { children: React.ReactNode }) {
  return (
    <div className="text-[10px] text-subtle uppercase tracking-widest font-bold px-1">
      {children}
    </div>
  );
}

/* ── Backtest tab (placeholder) ───────────────────────────────── */

function BacktestTab() {
  return (
    <div className="flex flex-1 items-center justify-center p-8">
      <div className="bg-panel border border-border rounded p-8 max-w-md text-center space-y-3">
        <div className="text-xs text-subtle uppercase tracking-widest font-bold">
          Backtest
        </div>
        <div className="text-sm text-ink">
          Backtest workspace will live here.
        </div>
        <div className="text-xs text-subtle">
          Rolling windows · point-in-time universe · honest transaction
          costs. Coming soon.
        </div>
      </div>
    </div>
  );
}

/* ── Tab buttons (Live = red dot, Backtest = arrow) ───────────── */

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
