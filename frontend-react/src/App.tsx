import StatusBar from './components/StatusBar';
import Chart from './components/Chart';
import QuotePanel from './components/QuotePanel';
import PortfolioPanel from './components/PortfolioPanel';
import AlertPanel from './components/AlertPanel';
import RiskPanel from './components/RiskPanel';
import TradeBlotter from './components/TradeBlotter';

export default function App() {
  return (
    <div className="flex flex-col h-screen bg-surface text-white overflow-hidden">
      <StatusBar />

      <div className="flex flex-1 overflow-hidden gap-2 p-2">
        {/* Main chart area */}
        <div className="flex-1 bg-panel border border-border rounded overflow-hidden">
          <Chart />
        </div>

        {/* Right sidebar */}
        <div className="w-72 flex flex-col gap-2 overflow-y-auto">
          <QuotePanel />
          <PortfolioPanel />
          <RiskPanel />
          <AlertPanel />
          <TradeBlotter />
        </div>
      </div>
    </div>
  );
}
