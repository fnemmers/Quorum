import { useState } from 'react';
import StatusBar from './components/StatusBar';
import Chart from './components/Chart';
import QuotePanel from './components/QuotePanel';
import PortfolioPanel from './components/PortfolioPanel';
import AlertPanel from './components/AlertPanel';
import RiskPanel from './components/RiskPanel';
import TradeBlotter from './components/TradeBlotter';
import ResearchPanel from './components/ResearchPanel';
import PaperTrailPanel from './components/PaperTrailPanel';

type Tab = 'trading' | 'research' | 'paper';

export default function App() {
  const [tab, setTab] = useState<Tab>('trading');

  return (
    <div className="flex flex-col h-screen bg-surface text-ink overflow-hidden">
      <StatusBar />

      <div className="flex gap-1 px-2 pt-1 border-b border-border">
        <TabButton label="Trading"     active={tab === 'trading'}  onClick={() => setTab('trading')} />
        <TabButton label="Research"    active={tab === 'research'} onClick={() => setTab('research')} />
        <TabButton label="Paper Trail" active={tab === 'paper'}    onClick={() => setTab('paper')} />
      </div>

      {tab === 'trading' && (
        <div className="flex flex-1 overflow-hidden gap-2 p-2">
          <div className="flex-1 bg-panel border border-border rounded overflow-hidden">
            <Chart />
          </div>

          <div className="w-72 flex flex-col gap-2 overflow-y-auto">
            <QuotePanel />
            <PortfolioPanel />
            <RiskPanel />
            <AlertPanel />
            <TradeBlotter />
          </div>
        </div>
      )}
      {tab === 'research' && <ResearchPanel />}
      {tab === 'paper'    && <PaperTrailPanel />}
    </div>
  );
}

function TabButton({ label, active, onClick }:
                   { label: string; active: boolean; onClick: () => void }) {
  return (
    <button
      onClick={onClick}
      className={`px-4 py-1.5 text-xs uppercase tracking-widest font-bold rounded-t border border-b-0 ${
        active
          ? 'bg-panel border-border text-ink'
          : 'bg-surface border-transparent text-subtle hover:text-ink'
      }`}
    >
      {label}
    </button>
  );
}
