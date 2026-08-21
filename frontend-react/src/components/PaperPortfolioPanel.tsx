import { useEffect, useState } from 'react';
import { useStore, type PaperPosition, type PaperOrder } from '../store/useStore';

/*
 * PaperPortfolioPanel  --  Paper trading tab body.
 *
 * $1M starting cash seeded server-side.  Market-order fills come from
 * the same Polygon quote feed as the Research tab: live quote if the
 * symbol is subscribed, else REST snapshot, else last cached close.
 * No commissions or slippage — the point is to sanity-check ranked
 * ideas end-to-end, not to model microstructure.
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

const qty = (x: number) => x.toLocaleString(undefined, { maximumFractionDigits: 4 });

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

export default function PaperPortfolioPanel() {
  const {
    paperState, paperBusy, paperError,
    refreshPaper, paperOrder, paperReset,
    sp500Tickers,
  } = useStore();

  const [orderSym,  setOrderSym]  = useState('AAPL');
  const [orderSide, setOrderSide] = useState<'buy' | 'sell'>('buy');
  const [orderQty,  setOrderQty]  = useState<number>(10);
  const [showReset, setShowReset] = useState(false);

  useEffect(() => {
    refreshPaper();
  }, [refreshPaper]);

  const submit = () => {
    const s = orderSym.trim().toUpperCase();
    if (!s || !(orderQty > 0)) return;
    paperOrder(s, orderSide, orderQty);
  };

  return (
    <div className="flex-1 flex gap-2 p-2 overflow-y-auto">
      {/* ── Left: ticket + reset ─────────────────────────────── */}
      <div className="w-72 flex flex-col gap-2">
        <div className="bg-panel border border-border rounded p-3 space-y-3">
          <div className="text-xs text-subtle uppercase tracking-widest font-bold">
            Order Ticket
          </div>

          <label className="block text-xs">
            <div className="text-subtle mb-1">Symbol</div>
            <input
              list="sp500-list"
              type="text"
              value={orderSym}
              onChange={(e) => setOrderSym(e.target.value)}
              onKeyDown={(e) => { if (e.key === 'Enter') submit(); }}
              placeholder="AAPL"
              className="w-full bg-surface border border-border rounded px-2 py-1 text-ink font-mono uppercase"
            />
            <datalist id="sp500-list">
              {sp500Tickers.map((t) => <option key={t} value={t} />)}
            </datalist>
          </label>

          <div className="grid grid-cols-2 gap-2 text-xs">
            <button
              onClick={() => setOrderSide('buy')}
              className={`py-1.5 rounded border text-xs font-bold uppercase tracking-widest ${
                orderSide === 'buy'
                  ? 'bg-bull text-black border-border'
                  : 'bg-surface text-subtle border-border hover:text-ink'
              }`}
            >Buy</button>
            <button
              onClick={() => setOrderSide('sell')}
              className={`py-1.5 rounded border text-xs font-bold uppercase tracking-widest ${
                orderSide === 'sell'
                  ? 'bg-bear text-white border-border'
                  : 'bg-surface text-subtle border-border hover:text-ink'
              }`}
            >Sell</button>
          </div>

          <label className="block text-xs">
            <div className="text-subtle mb-1">Quantity (fractional OK)</div>
            <input
              type="number" min={0} step={0.01} value={orderQty}
              onChange={(e) => setOrderQty(Number(e.target.value))}
              className="w-full bg-surface border border-border rounded px-2 py-1 text-ink font-mono"
            />
          </label>

          <button
            onClick={submit}
            disabled={paperBusy || !orderSym || !(orderQty > 0)}
            className="w-full bg-accent text-white text-xs font-bold uppercase tracking-widest py-2 rounded disabled:opacity-50"
          >
            {paperBusy ? 'Working…' : `Submit ${orderSide === 'buy' ? 'Buy' : 'Sell'} @ Market`}
          </button>

          {paperError && (
            <div className="text-[11px] text-bear font-mono break-words">
              {paperError}
            </div>
          )}

          <div className="text-[10px] text-subtle leading-relaxed">
            Fills at latest Polygon quote. No commissions or slippage.
            E*TRADE sandbox execution wiring is stubbed in <code>.env</code>
            but not yet used — this engine is self-contained.
          </div>
        </div>

        <div className="bg-panel border border-border rounded p-3 space-y-2">
          <div className="text-xs text-subtle uppercase tracking-widest font-bold">
            Account
          </div>
          <button
            onClick={() => refreshPaper()}
            className="w-full bg-surface border border-border text-xs uppercase tracking-widest font-bold py-1.5 rounded hover:text-ink"
          >Refresh</button>
          {!showReset ? (
            <button
              onClick={() => setShowReset(true)}
              className="w-full bg-surface border border-border text-xs uppercase tracking-widest text-subtle py-1.5 rounded hover:text-bear"
            >Reset account…</button>
          ) : (
            <div className="border border-bear rounded p-2 space-y-2">
              <div className="text-[11px] text-bear">
                Wipe positions/orders and reset cash to initial?
              </div>
              <div className="grid grid-cols-2 gap-2">
                <button
                  onClick={() => { paperReset(); setShowReset(false); }}
                  className="bg-bear text-white text-xs font-bold py-1 rounded"
                >Confirm</button>
                <button
                  onClick={() => setShowReset(false)}
                  className="bg-surface border border-border text-xs py-1 rounded"
                >Cancel</button>
              </div>
            </div>
          )}
        </div>
      </div>

      {/* ── Right: summary + tables ─────────────────────────── */}
      <div className="flex-1 flex flex-col gap-2 min-w-0">
        <div className="grid grid-cols-5 gap-2">
          <Tile
            label="Equity"
            value={money(paperState?.equity ?? 0)}
            sub={`init ${money(paperState?.initial_cash ?? 0)}`}
          />
          <Tile
            label="Cash"
            value={money(paperState?.cash ?? 0)}
          />
          <Tile
            label="Positions MV"
            value={money(paperState?.positions_value ?? 0)}
            sub={`${paperState?.positions.length ?? 0} names`}
          />
          <Tile
            label="Unrealized P&L"
            value={signedMoney(paperState?.unrealized_pnl ?? 0)}
            color={pnlColor(paperState?.unrealized_pnl ?? 0)}
          />
          <Tile
            label="Total Return"
            value={pct(paperState?.total_return_pct ?? 0)}
            sub={signedMoney(paperState?.total_pnl ?? 0)}
            color={pnlColor(paperState?.total_pnl ?? 0)}
          />
        </div>

        {/* Positions table */}
        <div className="bg-panel border border-border rounded flex flex-col overflow-hidden">
          <div className="px-3 py-1.5 border-b border-border text-xs uppercase tracking-widest font-bold text-subtle">
            Positions
          </div>
          <div className="overflow-x-auto">
            <table className="w-full text-xs font-mono">
              <thead className="bg-surface text-subtle uppercase tracking-widest text-[10px]">
                <tr>
                  <th className="text-left  px-2 py-1">Symbol</th>
                  <th className="text-right px-2 py-1">Qty</th>
                  <th className="text-right px-2 py-1">Avg Cost</th>
                  <th className="text-right px-2 py-1">Last</th>
                  <th className="text-right px-2 py-1">Market Value</th>
                  <th className="text-right px-2 py-1">Unrealized</th>
                  <th className="text-right px-2 py-1">%</th>
                </tr>
              </thead>
              <tbody>
                {(paperState?.positions ?? []).map((p: PaperPosition) => (
                  <tr key={p.symbol} className="border-t border-border">
                    <td className="px-2 py-1 font-bold">{p.symbol}</td>
                    <td className="px-2 py-1 text-right">{qty(p.qty)}</td>
                    <td className="px-2 py-1 text-right">{money(p.avg_cost)}</td>
                    <td className="px-2 py-1 text-right">{p.has_price ? money(p.last_price) : '—'}</td>
                    <td className="px-2 py-1 text-right">{money(p.market_value)}</td>
                    <td className={`px-2 py-1 text-right ${pnlColor(p.unrealized_pnl)}`}>{signedMoney(p.unrealized_pnl)}</td>
                    <td className={`px-2 py-1 text-right ${pnlColor(p.unrealized_pct)}`}>{pct(p.unrealized_pct)}</td>
                  </tr>
                ))}
                {(paperState?.positions.length ?? 0) === 0 && (
                  <tr>
                    <td colSpan={7} className="px-2 py-3 text-center text-subtle italic">
                      No positions. Submit a buy order to open one.
                    </td>
                  </tr>
                )}
              </tbody>
            </table>
          </div>
        </div>

        {/* Recent orders */}
        <div className="bg-panel border border-border rounded flex flex-col overflow-hidden">
          <div className="px-3 py-1.5 border-b border-border text-xs uppercase tracking-widest font-bold text-subtle">
            Recent Orders
          </div>
          <div className="overflow-x-auto max-h-64 overflow-y-auto">
            <table className="w-full text-xs font-mono">
              <thead className="bg-surface text-subtle uppercase tracking-widest text-[10px] sticky top-0">
                <tr>
                  <th className="text-left  px-2 py-1">Time</th>
                  <th className="text-left  px-2 py-1">Symbol</th>
                  <th className="text-left  px-2 py-1">Side</th>
                  <th className="text-right px-2 py-1">Qty</th>
                  <th className="text-right px-2 py-1">Fill</th>
                  <th className="text-right px-2 py-1">Cash Δ</th>
                </tr>
              </thead>
              <tbody>
                {(paperState?.orders ?? []).map((o: PaperOrder) => (
                  <tr key={o.id} className="border-t border-border">
                    <td className="px-2 py-1 text-subtle">
                      {new Date(o.created_at).toLocaleString()}
                    </td>
                    <td className="px-2 py-1 font-bold">{o.symbol}</td>
                    <td className={`px-2 py-1 uppercase ${o.side === 'buy' ? 'text-bull' : 'text-bear'}`}>
                      {o.side}
                    </td>
                    <td className="px-2 py-1 text-right">{qty(o.qty)}</td>
                    <td className="px-2 py-1 text-right">{money(o.fill_price)}</td>
                    <td className={`px-2 py-1 text-right ${pnlColor(o.cash_delta)}`}>{signedMoney(o.cash_delta)}</td>
                  </tr>
                ))}
                {(paperState?.orders.length ?? 0) === 0 && (
                  <tr>
                    <td colSpan={6} className="px-2 py-3 text-center text-subtle italic">
                      No orders yet.
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
