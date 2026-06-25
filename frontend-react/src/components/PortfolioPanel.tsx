import { useState } from 'react';
import { useStore } from '../store/useStore';

export default function PortfolioPanel() {
  const { holdings, quotes, addHolding, removeHolding, addBlotterEntry } = useStore();
  const [sym, setSym]     = useState('AAPL');
  const [shares, setShares] = useState('');
  const [price, setPrice]   = useState('');

  const totalPnl = holdings.reduce((acc, h) => {
    const cur = quotes[h.symbol]?.price ?? h.current ?? h.avg_price;
    return acc + (cur - h.avg_price) * h.shares;
  }, 0);

  function submit() {
    const s = parseFloat(shares);
    const p = parseFloat(price);
    if (!sym || isNaN(s) || isNaN(p)) return;
    addHolding(sym, s, p);
    addBlotterEntry({ symbol: sym, side: 'buy', quantity: s, price: p, ts: Date.now(), strategy: 'manual' });
    setShares(''); setPrice('');
  }

  return (
    <div className="bg-panel border border-border rounded p-3 space-y-3">
      <div className="flex items-center justify-between">
        <span className="text-xs text-subtle uppercase tracking-widest font-bold">Portfolio</span>
        <span className={`text-sm font-mono font-bold ${totalPnl >= 0 ? 'text-bull' : 'text-bear'}`}>
          {totalPnl >= 0 ? '+' : ''}${totalPnl.toFixed(2)} PnL
        </span>
      </div>

      <div className="space-y-1 max-h-[40vh] overflow-y-auto">
        {holdings.length === 0 && <div className="text-ink text-xs">No holdings</div>}
        {holdings.map((h) => {
          const cur = quotes[h.symbol]?.price ?? h.current ?? h.avg_price;
          const pnl = (cur - h.avg_price) * h.shares;
          return (
            <div key={h.symbol} className="flex items-center justify-between text-xs font-mono">
              <span className="text-accent w-12 font-bold">{h.symbol}</span>
              <span className="text-muted">{h.shares}sh @ ${h.avg_price.toFixed(2)}</span>
              <span className={pnl >= 0 ? 'text-bull' : 'text-bear'}>
                {pnl >= 0 ? '+' : ''}${pnl.toFixed(2)}
              </span>
              <button
                onClick={() => { removeHolding(h.symbol); addBlotterEntry({ symbol: h.symbol, side: 'sell', quantity: h.shares, price: cur, ts: Date.now(), strategy: 'manual' }); }}
                className="text-ink hover:text-bear ml-1"
              >✕</button>
            </div>
          );
        })}
      </div>

      <div className="border-t border-border pt-2 grid grid-cols-3 gap-1">
        <input
          placeholder="SYM" value={sym} onChange={(e) => setSym(e.target.value.toUpperCase())}
          className="bg-surface border border-border text-ink text-xs px-2 py-1 rounded focus:outline-none focus:border-accent"
        />
        <input
          placeholder="Shares" type="number" value={shares} onChange={(e) => setShares(e.target.value)}
          className="bg-surface border border-border text-ink text-xs px-2 py-1 rounded focus:outline-none focus:border-accent"
        />
        <input
          placeholder="Price" type="number" value={price} onChange={(e) => setPrice(e.target.value)}
          className="bg-surface border border-border text-ink text-xs px-2 py-1 rounded focus:outline-none focus:border-accent"
        />
        <button
          onClick={submit}
          className="col-span-3 bg-accent text-black text-xs font-bold py-1 rounded hover:bg-blue-400 transition"
        >+ Add Position</button>
      </div>
    </div>
  );
}
