import { useMemo, useState } from 'react';
import { useStore, type BatesBacktestRow } from '../store/useStore';

/*
 * BatesBacktestPanel  --  Backtest tab body.
 *
 *   Left column  — config (bot run, K, expiries, hedge horizon,
 *                  synthetic noise, MC paths)
 *   Right column — summary tiles + edge/PnL histograms
 *                  + full result table (sortable-ish, top-40 shown)
 *
 * Option data is synthetic (market_iv = bates_iv + N(0, σ) + skew).
 * Interpret expected_hedged_pnl as: "if you sold this option at
 * market_iv and delta-hedged daily under the calibrated Bates
 * dynamics, this is what your MC bundle says the average P&L would
 * be." Positive edge → positive expected P&L should correlate.
 */

function fmtPct(x: number, digits = 2) {
  if (x == null || Number.isNaN(x)) return '—';
  return `${(x * 100).toFixed(digits)}%`;
}
function fmtSigned(x: number, digits = 3) {
  if (x == null || Number.isNaN(x)) return '—';
  return `${x >= 0 ? '+' : ''}${x.toFixed(digits)}`;
}
function fmtNum(x: number, digits = 2) {
  if (x == null || Number.isNaN(x)) return '—';
  return x.toFixed(digits);
}

/* Simple SVG histogram for a numeric array. */
function Histogram({ values, bins = 20, color = '#3b82f6', label }:
  { values: number[]; bins?: number; color?: string; label: string }) {
  if (values.length === 0) return <div className="text-xs text-subtle italic">no data</div>;
  const lo = Math.min(...values);
  const hi = Math.max(...values);
  const width = hi > lo ? hi - lo : 1;
  const counts = new Array<number>(bins).fill(0);
  for (const v of values) {
    let b = Math.floor(((v - lo) / width) * bins);
    if (b < 0) b = 0;
    if (b >= bins) b = bins - 1;
    counts[b]++;
  }
  const maxC = Math.max(...counts, 1);
  const w = 200, h = 60;
  const bw = w / bins;
  return (
    <div className="flex flex-col gap-1">
      <div className="text-[10px] text-subtle uppercase tracking-widest">{label}</div>
      <svg width={w} height={h} className="bg-surface rounded">
        {counts.map((c, i) => {
          const bh = (c / maxC) * h;
          return (
            <rect key={i}
              x={i * bw + 1} y={h - bh}
              width={Math.max(1, bw - 1)} height={bh}
              fill={color} opacity={0.8}
            />
          );
        })}
        {/* zero line */}
        {lo < 0 && hi > 0 && (
          <line
            x1={((0 - lo) / width) * w} y1={0}
            x2={((0 - lo) / width) * w} y2={h}
            stroke="#8b949e" strokeDasharray="2,2"
          />
        )}
      </svg>
      <div className="flex justify-between text-[9px] text-subtle font-mono">
        <span>{lo.toFixed(3)}</span>
        <span>{hi.toFixed(3)}</span>
      </div>
    </div>
  );
}

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

const SP500_TOP_10 = 'AAPL,MSFT,NVDA,GOOGL,AMZN,META,LLY,AVGO,TSLA,JPM';

export default function BatesBacktestPanel() {
  const { batesBacktest, batesBacktestBusy, runBatesBacktest, sp500Tickers } = useStore();

  const [symbolsTxt,  setSymbolsTxt]  = useState(SP500_TOP_10);
  const [horizonDays, setHorizonDays] = useState(30);
  const [nStrikes,    setNStrikes]    = useState(7);
  const [expiriesTxt, setExpiriesTxt] = useState('7,30,90');
  const [nPaths,      setNPaths]      = useState(256);
  const [noiseSigma,  setNoiseSigma]  = useState(0.015);
  const [rRate,       setRRate]       = useState(0.045);
  const [sortBy,      setSortBy]      = useState<'edge' | 'pnl' | 'sharpe' | 'dollar'>('edge');
  const [sortDesc,    setSortDesc]    = useState(true);

  const result = batesBacktest;

  const parseSymbols  = (txt: string) =>
    txt.split(',').map((s) => s.trim().toUpperCase()).filter((s) => s.length > 0);
  const parseExpiries = (txt: string) =>
    txt.split(',').map((s) => parseInt(s.trim(), 10)).filter((n) => n > 0);

  const symbols = useMemo(() => parseSymbols(symbolsTxt), [symbolsTxt]);

  const sortedRows = useMemo(() => {
    if (!result) return [];
    const rows = [...result.rows];
    const pickVal = (r: BatesBacktestRow) =>
      sortBy === 'edge'   ? r.edge_vol_pts
    : sortBy === 'pnl'    ? r.expected_hedged_pnl
    : sortBy === 'sharpe' ? r.sharpe_daily
                          : r.dollar_edge;
    rows.sort((a, b) => sortDesc ? pickVal(b) - pickVal(a) : pickVal(a) - pickVal(b));
    return rows.slice(0, 40);
  }, [result, sortBy, sortDesc]);

  const kickOff = () => {
    if (symbols.length === 0) return;
    runBatesBacktest({
      symbols,
      horizonDays, nStrikes,
      expiriesDays:  parseExpiries(expiriesTxt),
      nPaths, noiseSigmaVol: noiseSigma, r: rRate,
    });
  };

  const edgeValues = result?.rows.map((r: BatesBacktestRow) => r.edge_vol_pts) ?? [];
  const pnlValues  = result?.rows.map((r: BatesBacktestRow) => r.expected_hedged_pnl) ?? [];

  const edgeColor = (e: number) =>
    e > 0.02 ? 'text-bull'
    : e < -0.02 ? 'text-bear'
    : 'text-subtle';

  const pnlColor = (p: number) =>
    p > 0 ? 'text-bull' : p < 0 ? 'text-bear' : 'text-subtle';

  return (
    <div className="flex-1 flex gap-2 p-2 overflow-y-auto">
      {/* ── Left: config ────────────────────────────────────── */}
      <div className="w-72 flex flex-col gap-2">
        <div className="bg-panel border border-border rounded p-3 space-y-3">
          <div className="text-xs text-subtle uppercase tracking-widest font-bold">
            Bates Backtest Config
          </div>

          <label className="block text-xs">
            <div className="text-subtle mb-1 flex items-center justify-between">
              <span>Symbols ({symbols.length})</span>
              <button
                onClick={() => setSymbolsTxt(SP500_TOP_10)}
                className="text-accent hover:underline"
              >top10</button>
              <button
                onClick={() => setSymbolsTxt(sp500Tickers.slice(0, 30).join(','))}
                className="text-accent hover:underline"
                disabled={sp500Tickers.length === 0}
              >sp30</button>
            </div>
            <textarea
              value={symbolsTxt}
              onChange={(e) => setSymbolsTxt(e.target.value)}
              rows={3}
              className="w-full bg-surface border border-border rounded px-2 py-1 text-ink font-mono text-[11px]"
              placeholder="AAPL,MSFT,NVDA,..."
            />
          </label>

          <div className="grid grid-cols-2 gap-2 text-xs">
            <label>
              <div className="text-subtle mb-1">Hedge horizon (d)</div>
              <input type="number" min={5} max={120} value={horizonDays}
                onChange={(e) => setHorizonDays(Number(e.target.value))}
                className="w-full bg-surface border border-border rounded px-2 py-1 text-ink" />
            </label>
            <label>
              <div className="text-subtle mb-1"># strikes</div>
              <input type="number" min={2} max={31} value={nStrikes}
                onChange={(e) => setNStrikes(Number(e.target.value))}
                className="w-full bg-surface border border-border rounded px-2 py-1 text-ink" />
            </label>
            <label>
              <div className="text-subtle mb-1"># MC paths</div>
              <input type="number" min={16} max={4096} value={nPaths}
                onChange={(e) => setNPaths(Number(e.target.value))}
                className="w-full bg-surface border border-border rounded px-2 py-1 text-ink" />
            </label>
          </div>

          <label className="block text-xs">
            <div className="text-subtle mb-1">Expiries (DTE, csv)</div>
            <input type="text" value={expiriesTxt}
              onChange={(e) => setExpiriesTxt(e.target.value)}
              className="w-full bg-surface border border-border rounded px-2 py-1 text-ink font-mono" />
          </label>

          <div className="grid grid-cols-2 gap-2 text-xs">
            <label>
              <div className="text-subtle mb-1">Synth σ (vol pts)</div>
              <input type="number" min={0} max={0.10} step={0.001} value={noiseSigma}
                onChange={(e) => setNoiseSigma(Number(e.target.value))}
                className="w-full bg-surface border border-border rounded px-2 py-1 text-ink" />
            </label>
            <label>
              <div className="text-subtle mb-1">r (rf, decimal)</div>
              <input type="number" min={0} max={0.15} step={0.005} value={rRate}
                onChange={(e) => setRRate(Number(e.target.value))}
                className="w-full bg-surface border border-border rounded px-2 py-1 text-ink" />
            </label>
          </div>

          <button
            onClick={kickOff}
            disabled={batesBacktestBusy || symbols.length === 0}
            className="w-full bg-accent text-black text-xs font-bold px-3 py-2 rounded disabled:opacity-50"
          >
            {batesBacktestBusy ? 'Running…' : 'Run Bates Backtest'}
          </button>

          <div className="text-[10px] text-subtle italic leading-snug">
            Options data is synthetic:
            <span className="font-mono"> market_iv = bates_iv + N(0,σ) + skew(K/S,T)</span>.
            Positive edge_vol_pts → we're selling rich vol; expected_hedged_pnl
            should track that under the calibrated Bates dynamics.
          </div>
        </div>
      </div>

      {/* ── Right: results ─────────────────────────────────── */}
      <div className="flex-1 flex flex-col gap-2 min-w-0">
        {!result ? (
          <div className="flex-1 flex items-center justify-center border border-border rounded bg-panel text-xs text-subtle italic">
            {batesBacktestBusy
              ? 'Pricing options + simulating hedged paths…'
              : 'Pick a bot run and hit "Run Bates Backtest" to see the option grid.'}
          </div>
        ) : (
          <>
            {/* Summary tiles */}
            <div className="grid grid-cols-5 gap-2">
              <Tile label="Rows" value={String(result.summary.n_rows)}
                sub={`${result.summary.n_symbols} sym · ${result.summary.n_strikes}K · ${result.summary.n_expiries}T`} />
              <Tile label="Mean edge (vol)"
                value={fmtSigned(result.summary.mean_edge_vol_pts, 4)}
                sub={`σ ${result.summary.std_edge_vol_pts.toFixed(4)}`}
                color={edgeColor(result.summary.mean_edge_vol_pts)} />
              <Tile label="Hit rate |edge|>1v"
                value={fmtPct(result.summary.hit_rate_edge_gt_1vol, 1)} />
              <Tile label="Avg hedged P&L"
                value={fmtSigned(result.summary.avg_hedged_pnl, 3)}
                color={pnlColor(result.summary.avg_hedged_pnl)} />
              <Tile label="Avg Sharpe (daily)"
                value={fmtSigned(result.summary.avg_sharpe_daily, 3)}
                color={pnlColor(result.summary.avg_sharpe_daily)} />
            </div>

            {/* Distributions */}
            <div className="flex gap-4 bg-panel border border-border rounded p-3">
              <Histogram values={edgeValues}   color="#3b82f6" label="edge_vol_pts distribution" />
              <Histogram values={pnlValues}    color="#22c55e" label="expected_hedged_pnl distribution" />
              <div className="flex-1 text-[10px] text-subtle leading-snug pl-3">
                Blue: market vs model IV gap. Green: MC-mean of hedged P&L per option,
                in $ per contract. Correlation of the two is the mathematical
                sanity check — sign of edge should predict sign of P&L for
                a fair-underlying-dynamics Bates.
              </div>
            </div>

            {/* Result table */}
            <div className="bg-panel border border-border rounded overflow-hidden">
              <div className="px-3 py-2 border-b border-border flex items-center justify-between">
                <div className="text-xs text-subtle uppercase tracking-widest font-bold">
                  Option grid — top 40 by
                  <button onClick={() => { setSortBy('edge');   setSortDesc(!sortDesc); }}
                    className={`ml-2 ${sortBy==='edge'?'text-accent':'text-subtle hover:text-ink'}`}>edge</button>
                  <button onClick={() => { setSortBy('pnl');    setSortDesc(!sortDesc); }}
                    className={`ml-2 ${sortBy==='pnl'?'text-accent':'text-subtle hover:text-ink'}`}>pnl</button>
                  <button onClick={() => { setSortBy('sharpe'); setSortDesc(!sortDesc); }}
                    className={`ml-2 ${sortBy==='sharpe'?'text-accent':'text-subtle hover:text-ink'}`}>sharpe</button>
                  <button onClick={() => { setSortBy('dollar'); setSortDesc(!sortDesc); }}
                    className={`ml-2 ${sortBy==='dollar'?'text-accent':'text-subtle hover:text-ink'}`}>$edge</button>
                </div>
                <div className="text-[10px] text-subtle font-mono">
                  {result.summary.n_paths} paths · {result.summary.horizon_days}d hedge · σ_synth {result.summary.noise_sigma_vol.toFixed(3)}
                </div>
              </div>
              <div className="overflow-y-auto max-h-96 text-xs font-mono">
                <table className="w-full">
                  <thead className="sticky top-0 bg-panel text-subtle text-[10px] uppercase tracking-widest">
                    <tr>
                      <th className="text-left  px-2 py-1">Sym</th>
                      <th className="text-right px-2 py-1">K</th>
                      <th className="text-right px-2 py-1">DTE</th>
                      <th className="text-left  px-2 py-1">CP</th>
                      <th className="text-right px-2 py-1">Model IV</th>
                      <th className="text-right px-2 py-1">Mkt IV</th>
                      <th className="text-right px-2 py-1">Edge (vp)</th>
                      <th className="text-right px-2 py-1">Prem</th>
                      <th className="text-right px-2 py-1">Δ</th>
                      <th className="text-right px-2 py-1">Γ</th>
                      <th className="text-right px-2 py-1">E[P&L]</th>
                      <th className="text-right px-2 py-1">σ(P&L)</th>
                      <th className="text-right px-2 py-1">Sharpe</th>
                      <th className="text-right px-2 py-1">$Edge</th>
                    </tr>
                  </thead>
                  <tbody>
                    {sortedRows.map((r, i) => (
                      <tr key={i} className="border-t border-border/40">
                        <td className="px-2 py-1 text-ink font-bold">{r.symbol}</td>
                        <td className="px-2 py-1 text-right">{r.strike.toFixed(1)}</td>
                        <td className="px-2 py-1 text-right">{r.dte_days}</td>
                        <td className={`px-2 py-1 ${r.right==='C'?'text-bull':'text-bear'}`}>{r.right}</td>
                        <td className="px-2 py-1 text-right">{fmtPct(r.model_iv, 1)}</td>
                        <td className="px-2 py-1 text-right">{fmtPct(r.market_iv, 1)}</td>
                        <td className={`px-2 py-1 text-right ${edgeColor(r.edge_vol_pts)}`}>
                          {fmtSigned(r.edge_vol_pts * 100, 2)}
                        </td>
                        <td className="px-2 py-1 text-right">{r.premium.toFixed(2)}</td>
                        <td className="px-2 py-1 text-right">{r.delta.toFixed(2)}</td>
                        <td className="px-2 py-1 text-right">{r.gamma.toFixed(4)}</td>
                        <td className={`px-2 py-1 text-right ${pnlColor(r.expected_hedged_pnl)}`}>
                          {fmtSigned(r.expected_hedged_pnl, 3)}
                        </td>
                        <td className="px-2 py-1 text-right">{fmtNum(r.std_hedged_pnl, 3)}</td>
                        <td className={`px-2 py-1 text-right ${pnlColor(r.sharpe_daily)}`}>
                          {fmtSigned(r.sharpe_daily, 3)}
                        </td>
                        <td className={`px-2 py-1 text-right ${pnlColor(r.dollar_edge)}`}>
                          {fmtSigned(r.dollar_edge, 2)}
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </div>
          </>
        )}
      </div>
    </div>
  );
}
