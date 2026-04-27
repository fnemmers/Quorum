import { useEffect } from 'react';
import { useStore } from '../store/useStore';

export default function QuotePanel() {
  const { symbol, quotes, send } = useStore();

  /* Poll snapshot every 10s (free tier only has REST) */
  useEffect(() => {
    const poll = () => send({ cmd: 'snapshot', symbol });
    poll();
    const id = setInterval(poll, 10_000);
    return () => clearInterval(id);
  }, [symbol]);

  const quote = quotes[symbol];

  return (
    <div className="bg-panel border border-border rounded p-3 space-y-2">
      <div className="text-xs text-subtle uppercase tracking-widest font-bold">Live Quote</div>
      {quote ? (
        <div className="grid grid-cols-2 gap-x-4 gap-y-1 text-sm font-mono">
          <Row label="Last"   value={`$${quote.price.toFixed(2)}`} />
          <Row label="Bid"    value={`$${quote.bid.toFixed(2)}`} />
          <Row label="Ask"    value={`$${quote.ask.toFixed(2)}`} />
          <Row label="Volume" value={Number(quote.volume).toLocaleString()} />
          <Row label="Spread" value={`$${(quote.ask - quote.bid).toFixed(3)}`} />
          <Row label="Updated" value={new Date(quote.ts).toLocaleTimeString()} />
        </div>
      ) : (
        <div className="text-ink text-sm">No data</div>
      )}
    </div>
  );
}

function Row({ label, value }: { label: string; value: string }) {
  return (
    <>
      <span className="text-subtle">{label}</span>
      <span className="text-ink">{value}</span>
    </>
  );
}
