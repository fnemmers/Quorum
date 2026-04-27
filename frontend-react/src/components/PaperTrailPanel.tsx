import { useState, useMemo } from 'react';
import { useStore, PAPER_STARTING_CASH, type PaperPosition } from '../store/useStore';

function fmtMoney(n: number): string {
  return n.toLocaleString('en-US', { style: 'currency', currency: 'USD', maximumFractionDigits: 2 });
}

function fmtTs(ms: number): string {
  return new Date(ms).toISOString().slice(0, 19).replace('T', ' ');
}

/* ── Header: cash / equity / P&L ─────────────────────────────── */

function HeaderStats({ positions }: { positions: PaperPosition[] }) {
  const { paperCash, quotes } = useStore();

  const { marketValue, costBasis } = useMemo(() => {
    let mv = 0, cb = 0;
    for (const p of positions) {
      const last = quotes[p.symbol]?.price ?? p.avgCost;
      mv += last * p.shares;
      cb += p.avgCost * p.shares;
    }
    return { marketValue: mv, costBasis: cb };
  }, [positions, quotes]);

  const equity = paperCash + marketValue;
  const totalPnl = equity - PAPER_STARTING_CASH;
  const totalPnlPct = totalPnl / PAPER_STARTING_CASH * 100;
  const openPnl = marketValue - costBasis;

  return (
    <div className="grid grid-cols-4 gap-2">
      <Stat label="Cash"          value={fmtMoney(paperCash)}    sub={`${(paperCash / equity * 100).toFixed(1)}% of equity`} />
      <Stat label="Market Value"  value={fmtMoney(marketValue)}  sub={`${positions.length} positions`} />
      <Stat label="Total Equity"  value={fmtMoney(equity)}       sub={`start ${fmtMoney(PAPER_STARTING_CASH)}`} />
      <Stat
        label="Total P&L"
        value={`${totalPnl >= 0 ? '+' : ''}${fmtMoney(totalPnl)}`}
        sub={`${totalPnl >= 0 ? '+' : ''}${totalPnlPct.toFixed(2)}% · open ${openPnl >= 0 ? '+' : ''}${fmtMoney(openPnl)}`}
        valueClass={totalPnl >= 0 ? 'text-bull' : 'text-bear'}
      />
    </div>
  );
}

function Stat({ label, value, sub, valueClass = 'text-ink' }:
              { label: string; value: string; sub: string; valueClass?: string }) {
  return (
    <div className="bg-panel border border-border rounded p-3">
      <div className="text-[10px] uppercase tracking-widest font-bold text-subtle">{label}</div>
      <div className={`text-lg font-mono ${valueClass}`}>{value}</div>
      <div className="text-[10px] text-subtle font-mono">{sub}</div>
    </div>
  );
}

/* ── Buy/Sell form ───────────────────────────────────────────── */

function TradeForm() {
  const { paperBuy, paperSell, quotes } = useStore();
  const [symbol, setSymbol] = useState('');
  const [shares, setShares] = useState('');
  const [price,  setPrice]  = useState('');
  const [error,  setError]  = useState<string | null>(null);
  const [flash,  setFlash]  = useState<string | null>(null);

  const submit = (side: 'buy' | 'sell') => {
    setError(null); setFlash(null);
    const sh = Number(shares);
    const pr = Number(price);
    const fn = side === 'buy' ? paperBuy : paperSell;
    const err = fn(symbol, sh, pr);
    if (err) {
      setError(err);
    } else {
      setFlash(`${side.toUpperCase()} ${sh} ${symbol.toUpperCase()} @ ${fmtMoney(pr)}`);
      setSymbol(''); setShares(''); setPrice('');
    }
  };

  const fillFromQuote = () => {
    const sym = symbol.trim().toUpperCase();
    const q = quotes[sym];
    if (q) setPrice(q.price.toFixed(2));
  };

  return (
    <div className="bg-panel border border-border rounded p-3 space-y-3">
      <div className="text-xs text-subtle uppercase tracking-widest font-bold">Manual Trade</div>

      <div className="grid grid-cols-3 gap-2 text-xs font-mono">
        <div>
          <div className="text-muted mb-1">symbol</div>
          <input
            type="text"
            value={symbol}
            onChange={(e) => setSymbol(e.target.value.toUpperCase())}
            placeholder="AAPL"
            className="w-full bg-surface border border-border rounded px-2 py-1 text-ink uppercase"
          />
        </div>
        <div>
          <div className="text-muted mb-1">shares</div>
          <input
            type="number" min={0} step="any"
            value={shares}
            onChange={(e) => setShares(e.target.value)}
            placeholder="100"
            className="w-full bg-surface border border-border rounded px-2 py-1 text-ink"
          />
        </div>
        <div>
          <div className="flex justify-between">
            <span className="text-muted mb-1">price</span>
            <button
              type="button"
              onClick={fillFromQuote}
              className="text-[10px] text-accent hover:underline"
              title="Fill with last quote"
            >use last</button>
          </div>
          <input
            type="number" min={0} step="any"
            value={price}
            onChange={(e) => setPrice(e.target.value)}
            placeholder="150.00"
            className="w-full bg-surface border border-border rounded px-2 py-1 text-ink"
          />
        </div>
      </div>

      <div className="grid grid-cols-2 gap-2">
        <button
          onClick={() => submit('buy')}
          className="py-1.5 rounded bg-bull text-surface font-bold text-sm hover:opacity-90"
        >BUY</button>
        <button
          onClick={() => submit('sell')}
          className="py-1.5 rounded bg-bear text-ink font-bold text-sm hover:opacity-90"
        >SELL</button>
      </div>

      {error && <div className="text-xs text-bear font-mono">{error}</div>}
      {flash && <div className="text-xs text-bull font-mono">{flash}</div>}
    </div>
  );
}

/* ── Positions table ─────────────────────────────────────────── */

function PositionsTable({ positions }: { positions: PaperPosition[] }) {
  const { quotes } = useStore();

  return (
    <div className="bg-panel border border-border rounded p-3 space-y-2">
      <div className="text-xs text-subtle uppercase tracking-widest font-bold">Positions</div>

      {positions.length === 0 ? (
        <div className="text-xs text-subtle italic">No open positions.</div>
      ) : (
        <div className="overflow-x-auto">
          <table className="w-full text-xs font-mono">
            <thead>
              <tr className="text-subtle text-left border-b border-border">
                <th className="py-1 pr-2">Symbol</th>
                <th className="py-1 pr-2 text-right">Shares</th>
                <th className="py-1 pr-2 text-right">Avg Cost</th>
                <th className="py-1 pr-2 text-right">Last</th>
                <th className="py-1 pr-2 text-right">Market Value</th>
                <th className="py-1 pr-2 text-right">P&L</th>
                <th className="py-1 pr-2 text-right">P&L %</th>
              </tr>
            </thead>
            <tbody>
              {positions.map((p) => {
                const last = quotes[p.symbol]?.price ?? p.avgCost;
                const mv   = last * p.shares;
                const cost = p.avgCost * p.shares;
                const pnl  = mv - cost;
                const pct  = cost > 0 ? pnl / cost * 100 : 0;
                const cls  = pnl >= 0 ? 'text-bull' : 'text-bear';
                const stale = !quotes[p.symbol];
                return (
                  <tr key={p.symbol} className="border-b border-border/50">
                    <td className="py-1 pr-2 text-ink font-bold">{p.symbol}</td>
                    <td className="py-1 pr-2 text-right">{p.shares}</td>
                    <td className="py-1 pr-2 text-right">{fmtMoney(p.avgCost)}</td>
                    <td className={`py-1 pr-2 text-right ${stale ? 'text-subtle italic' : ''}`}>
                      {fmtMoney(last)}{stale && '*'}
                    </td>
                    <td className="py-1 pr-2 text-right">{fmtMoney(mv)}</td>
                    <td className={`py-1 pr-2 text-right ${cls}`}>{pnl >= 0 ? '+' : ''}{fmtMoney(pnl)}</td>
                    <td className={`py-1 pr-2 text-right ${cls}`}>{pnl >= 0 ? '+' : ''}{pct.toFixed(2)}%</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
          <div className="text-[10px] text-subtle mt-1">* no live quote — using avg cost</div>
        </div>
      )}
    </div>
  );
}

/* ── Trade history ───────────────────────────────────────────── */

function TradeHistory() {
  const { paperTrades } = useStore();

  return (
    <div className="bg-panel border border-border rounded p-3 space-y-2">
      <div className="flex justify-between items-center">
        <div className="text-xs text-subtle uppercase tracking-widest font-bold">Trade History</div>
        <div className="text-xs text-subtle font-mono">{paperTrades.length} trades</div>
      </div>

      {paperTrades.length === 0 ? (
        <div className="text-xs text-subtle italic">No trades yet.</div>
      ) : (
        <div className="max-h-72 overflow-y-auto">
          <table className="w-full text-xs font-mono">
            <thead className="sticky top-0 bg-panel">
              <tr className="text-subtle text-left border-b border-border">
                <th className="py-1 pr-2">Time</th>
                <th className="py-1 pr-2">Side</th>
                <th className="py-1 pr-2">Symbol</th>
                <th className="py-1 pr-2 text-right">Shares</th>
                <th className="py-1 pr-2 text-right">Price</th>
                <th className="py-1 pr-2 text-right">Total</th>
              </tr>
            </thead>
            <tbody>
              {paperTrades.map((t) => (
                <tr key={t.id} className="border-b border-border/50">
                  <td className="py-1 pr-2 text-muted">{fmtTs(t.ts)}</td>
                  <td className={`py-1 pr-2 ${t.side === 'buy' ? 'text-bull' : 'text-bear'}`}>
                    {t.side.toUpperCase()}
                  </td>
                  <td className="py-1 pr-2 text-ink font-bold">{t.symbol}</td>
                  <td className="py-1 pr-2 text-right">{t.shares}</td>
                  <td className="py-1 pr-2 text-right">{fmtMoney(t.price)}</td>
                  <td className="py-1 pr-2 text-right">{fmtMoney(t.shares * t.price)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}

/* ── Top-level panel ──────────────────────────────────────────── */

export default function PaperTrailPanel() {
  const { paperPositions, paperReset, paperTrades } = useStore();

  const onReset = () => {
    const msg = paperTrades.length > 0
      ? `Reset paper portfolio to ${fmtMoney(PAPER_STARTING_CASH)} and erase ${paperTrades.length} trades?`
      : `Reset paper portfolio to ${fmtMoney(PAPER_STARTING_CASH)}?`;
    if (window.confirm(msg)) paperReset();
  };

  return (
    <div className="flex-1 overflow-y-auto p-2 space-y-2">
      <div className="flex justify-between items-center">
        <div className="text-xs text-muted uppercase tracking-widest font-bold">
          Paper Trail · {fmtMoney(PAPER_STARTING_CASH)} starting cash
        </div>
        <button
          onClick={onReset}
          className="text-xs px-3 py-1 rounded border border-border text-muted hover:text-ink hover:border-bear"
        >reset</button>
      </div>

      <HeaderStats positions={paperPositions} />

      <div className="grid grid-cols-1 md:grid-cols-3 gap-2">
        <div className="md:col-span-1">
          <TradeForm />
        </div>
        <div className="md:col-span-2">
          <PositionsTable positions={paperPositions} />
        </div>
      </div>

      <TradeHistory />
    </div>
  );
}
