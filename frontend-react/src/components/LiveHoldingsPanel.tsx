import { useEffect, useState } from 'react';
import { useStore, type LiveHolding } from '../store/useStore';

/*
 * LiveHoldingsPanel  --  Live tab body.
 *
 * A read-only view of the user's real-money book.  No orders are
 * placed from here; the row form lets the user record what they
 * actually hold (symbol, shares, cost basis) so market value and
 * unrealized P&L can be shown against live Polygon quotes.
 *
 * Seed row: AAPL (highschool holding).  User edits qty / avg cost
 * to match their brokerage statement.
 */

const money = (x: number) =>
  x == null || Number.isNaN(x) ? '—' :
  `$${x.toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 })}`;
const signedMoney = (x: number) =>
  x == null || Number.isNaN(x) ? '—' :
  `${x >= 0 ? '+' : '−'}$${Math.abs(x).toLocaleString(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 })}`;
const pct = (x: number) =>
  x == null || Number.isNaN(x) ? '—' :
  `${x >= 0 ? '+' : ''}${(x * 100).toFixed(2)}%`;
const qtyFmt = (x: number) => x.toLocaleString(undefined, { maximumFractionDigits: 4 });
const pnlColor = (x: number) =>
  x > 0 ? 'text-bull' : x < 0 ? 'text-bear' : 'text-subtle';

function Tile({ label, value, sub, color }:
  { label: string; value: string; sub?: string; color?: string }) {
  return (
    <div className="border border-border rounded p-2 bg-panel">
      <div className="text-[10px] text-subtle uppercase tracking-widest">{label}</div>
      <div className={`text-lg font-mono ${color ?? 'text-ink'}`}>{value}</div>
      {sub && <div className="text-[10px] text-subtle font-mono">{sub}</div>}
    </div>
  );
}

export default function LiveHoldingsPanel() {
  const {
    liveHoldings, liveHoldingsBusy,
    refreshLiveHoldings, liveHoldingsSet,
    sp500Tickers,
  } = useStore();

  const [symbol,  setSymbol]  = useState('AAPL');
  const [qty,     setQty]     = useState<number>(0);
  const [avgCost, setAvgCost] = useState<number>(0);
  const [notes,   setNotes]   = useState('');

  useEffect(() => {
    refreshLiveHoldings();
  }, [refreshLiveHoldings]);

  const submit = () => {
    const s = symbol.trim().toUpperCase();
    if (!s) return;
    liveHoldingsSet(s, qty, avgCost, notes);
    setNotes('');
  };

  const loadForEdit = (h: LiveHolding) => {
    setSymbol(h.symbol);
    setQty(h.qty);
    setAvgCost(h.avg_cost);
    setNotes(h.notes ?? '');
  };

  const removeHolding = (sym: string) => {
    liveHoldingsSet(sym, 0, 0, '');
  };

  return (
    <div className="flex-1 flex gap-2 p-2 overflow-y-auto">
      {/* ── Left: entry form ─────────────────────────────────── */}
      <div className="w-72 flex flex-col gap-2">
        <div className="bg-panel border border-border rounded p-3 space-y-3">
          <div className="text-xs text-subtle uppercase tracking-widest font-bold">
            Enter / Update Holding
          </div>

          <label className="block text-xs">
            <div className="text-subtle mb-1">Symbol</div>
            <input
              list="sp500-list"
              type="text"
              value={symbol}
              onChange={(e) => setSymbol(e.target.value)}
              placeholder="AAPL"
              className="w-full bg-surface border border-border rounded px-2 py-1 text-ink font-mono uppercase"
            />
            <datalist id="sp500-list">
              {sp500Tickers.map((t) => <option key={t} value={t} />)}
            </datalist>
          </label>

          <label className="block text-xs">
            <div className="text-subtle mb-1">Shares</div>
            <input
              type="number" min={0} step={0.0001} value={qty}
              onChange={(e) => setQty(Number(e.target.value))}
              className="w-full bg-surface border border-border rounded px-2 py-1 text-ink font-mono"
            />
          </label>

          <label className="block text-xs">
            <div className="text-subtle mb-1">Avg Cost ($/share)</div>
            <input
              type="number" min={0} step={0.01} value={avgCost}
              onChange={(e) => setAvgCost(Number(e.target.value))}
              className="w-full bg-surface border border-border rounded px-2 py-1 text-ink font-mono"
            />
          </label>

          <label className="block text-xs">
            <div className="text-subtle mb-1">Notes (optional)</div>
            <input
              type="text" value={notes}
              onChange={(e) => setNotes(e.target.value)}
              placeholder="e.g. bought 2019 in HS"
              className="w-full bg-surface border border-border rounded px-2 py-1 text-ink font-mono text-[11px]"
            />
          </label>

          <button
            onClick={submit}
            disabled={liveHoldingsBusy || !symbol}
            className="w-full bg-accent text-white text-xs font-bold uppercase tracking-widest py-2 rounded disabled:opacity-50"
          >
            {liveHoldingsBusy ? 'Saving…' : 'Save Holding'}
          </button>

          <div className="text-[10px] text-subtle leading-relaxed">
            Set shares to 0 to remove. This tab is a read-only book —
            no orders are routed from here.
          </div>

          <button
            onClick={() => refreshLiveHoldings()}
            className="w-full bg-surface border border-border text-xs uppercase tracking-widest font-bold py-1.5 rounded hover:text-ink"
          >Refresh Prices</button>
        </div>
      </div>

      {/* ── Right: summary + table ───────────────────────────── */}
      <div className="flex-1 flex flex-col gap-2 min-w-0">
        <div className="grid grid-cols-4 gap-2">
          <Tile
            label="Cost Basis"
            value={money(liveHoldings?.total_cost ?? 0)}
            sub={`${liveHoldings?.holdings.length ?? 0} names`}
          />
          <Tile
            label="Market Value"
            value={money(liveHoldings?.total_value ?? 0)}
          />
          <Tile
            label="Unrealized P&L"
            value={signedMoney(liveHoldings?.unrealized_pnl ?? 0)}
            color={pnlColor(liveHoldings?.unrealized_pnl ?? 0)}
          />
          <Tile
            label="Return"
            value={pct(liveHoldings?.unrealized_pct ?? 0)}
            color={pnlColor(liveHoldings?.unrealized_pct ?? 0)}
          />
        </div>

        <div className="bg-panel border border-border rounded flex flex-col overflow-hidden">
          <div className="px-3 py-1.5 border-b border-border text-xs uppercase tracking-widest font-bold text-subtle">
            Holdings
          </div>
          <div className="overflow-x-auto">
            <table className="w-full text-xs font-mono">
              <thead className="bg-surface text-subtle uppercase tracking-widest text-[10px]">
                <tr>
                  <th className="text-left  px-2 py-1">Symbol</th>
                  <th className="text-right px-2 py-1">Shares</th>
                  <th className="text-right px-2 py-1">Avg Cost</th>
                  <th className="text-right px-2 py-1">Last</th>
                  <th className="text-right px-2 py-1">Cost Basis</th>
                  <th className="text-right px-2 py-1">Market Value</th>
                  <th className="text-right px-2 py-1">Unrealized</th>
                  <th className="text-right px-2 py-1">%</th>
                  <th className="text-left  px-2 py-1">Notes</th>
                  <th className="px-2 py-1"></th>
                </tr>
              </thead>
              <tbody>
                {(liveHoldings?.holdings ?? []).map((h: LiveHolding) => (
                  <tr
                    key={h.symbol}
                    onClick={() => loadForEdit(h)}
                    className="border-t border-border hover:bg-surface/50 cursor-pointer"
                  >
                    <td className="px-2 py-1 font-bold">{h.symbol}</td>
                    <td className="px-2 py-1 text-right">{qtyFmt(h.qty)}</td>
                    <td className="px-2 py-1 text-right">{money(h.avg_cost)}</td>
                    <td className="px-2 py-1 text-right">{h.has_price ? money(h.last_price) : '—'}</td>
                    <td className="px-2 py-1 text-right">{money(h.cost_basis)}</td>
                    <td className="px-2 py-1 text-right">{money(h.market_value)}</td>
                    <td className={`px-2 py-1 text-right ${pnlColor(h.unrealized_pnl)}`}>{signedMoney(h.unrealized_pnl)}</td>
                    <td className={`px-2 py-1 text-right ${pnlColor(h.unrealized_pct)}`}>{pct(h.unrealized_pct)}</td>
                    <td className="px-2 py-1 text-subtle text-[11px] truncate max-w-[200px]">{h.notes}</td>
                    <td className="px-2 py-1 text-right">
                      <button
                        onClick={(e) => { e.stopPropagation(); removeHolding(h.symbol); }}
                        className="text-[10px] text-subtle hover:text-bear uppercase tracking-widest"
                      >remove</button>
                    </td>
                  </tr>
                ))}
                {(liveHoldings?.holdings.length ?? 0) === 0 && (
                  <tr>
                    <td colSpan={10} className="px-2 py-4 text-center text-subtle italic">
                      No holdings recorded yet. Enter your AAPL shares and cost basis on the left.
                    </td>
                  </tr>
                )}
              </tbody>
            </table>
          </div>
        </div>
      </div>
    </div>
  );
}
