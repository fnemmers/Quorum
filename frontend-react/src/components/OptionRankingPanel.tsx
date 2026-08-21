import { useMemo, useState } from 'react';
import { useStore, type OptionRankedRow } from '../store/useStore';

/*
 * OptionRankingPanel  --  Phase 4 fused option ranking.
 *
 * Same math the whiteboard argues for: Q vs P gap operationalized as
 *   blended_option_score = w_edge·z_bates_edge     (per expiry)
 *                        + w_news·z_news_jump      (per universe)
 *                        + w_convex·z_convexity    (per expiry)
 *
 * Positive scores → sell rich vol; negative → buy cheap vol. The sign
 * flip comes from signed_edge inherited from the Bates row.
 */

function fmtPct(x: number, digits = 2) {
  if (x == null || Number.isNaN(x)) return '—';
  return `${(x * 100).toFixed(digits)}%`;
}
function fmtSigned(x: number, digits = 2) {
  if (x == null || Number.isNaN(x)) return '—';
  return `${x >= 0 ? '+' : ''}${x.toFixed(digits)}`;
}

function zColor(z: number) {
  if (z >  1.0) return 'text-bull';
  if (z < -1.0) return 'text-bear';
  return 'text-ink';
}

function ZBar({ z }: { z: number }) {
  const clamped = Math.max(-3, Math.min(3, z));
  const w = Math.abs(clamped) * (100 / 3) * 0.5;
  const positive = clamped >= 0;
  return (
    <div className="relative h-2 w-16 bg-surface border border-border rounded">
      <div className="absolute inset-y-0 left-1/2 w-px bg-border" />
      <div
        className="absolute inset-y-0"
        style={{
          width: `${w}%`,
          background: positive ? '#0a0' : '#d11',
          left: positive ? '50%' : `${50 - w}%`,
        }}
      />
    </div>
  );
}

const SP500_TOP_10 = 'AAPL,MSFT,NVDA,GOOGL,AMZN,META,LLY,AVGO,TSLA,JPM';

export default function OptionRankingPanel() {
  const {
    optionRanking, optionRankingBusy, runOptionRanking, sp500Tickers,
  } = useStore();

  const [symbolsTxt,  setSymbolsTxt]  = useState(SP500_TOP_10);
  const [horizonDays, setHorizonDays] = useState(30);
  const [nStrikes,    setNStrikes]    = useState(7);
  const [expiriesTxt, setExpiriesTxt] = useState('7,30,90');
  const [nPaths,      setNPaths]      = useState(256);
  const [wEdge,       setWEdge]       = useState(0.60);
  const [wNews,       setWNews]       = useState(0.27);
  const [wConvex,     setWConvex]     = useState(0.13);
  const [topN,        setTopN]        = useState(20);

  const result = optionRanking;

  const parseSymbols  = (txt: string) =>
    txt.split(',').map((s) => s.trim().toUpperCase()).filter((s) => s.length > 0);
  const parseExpiries = (txt: string) =>
    txt.split(',').map((s) => parseInt(s.trim(), 10)).filter((n) => n > 0);

  const symbols = useMemo(() => parseSymbols(symbolsTxt), [symbolsTxt]);

  const topRows = useMemo(
    () => result?.ranked.slice(0, topN) ?? [],
    [result, topN],
  );

  const kickOff = () => {
    if (symbols.length === 0) return;
    runOptionRanking({
      symbols,
      horizonDays, nStrikes,
      expiriesDays:  parseExpiries(expiriesTxt),
      nPaths,
      wEdge, wNews, wConvex,
    });
  };

  const weightsSum = wEdge + wNews + wConvex;

  const sideBadge = (r: OptionRankedRow) => {
    const base = 'px-1.5 py-0.5 rounded text-[10px] uppercase tracking-widest font-bold';
    return r.signed_edge > 0
      ? <span className={`${base} bg-bear/30 text-bear border border-bear`}>SELL VOL</span>
      : <span className={`${base} bg-bull/30 text-bull border border-bull`}>BUY VOL</span>;
  };

  return (
    <div className="flex flex-col gap-2">
      <div className="bg-panel border border-border rounded p-3 flex items-center gap-4 flex-wrap">
        <div className="text-xs text-subtle uppercase tracking-widest font-bold">
          Fused Option Ranking (Q−P)
        </div>

        <label className="text-xs flex items-center gap-2">
          <span className="text-subtle">Symbols ({symbols.length})</span>
          <input type="text" value={symbolsTxt}
            onChange={(e) => setSymbolsTxt(e.target.value)}
            className="w-60 bg-surface border border-border rounded px-2 py-1 text-ink font-mono" />
          <button
            onClick={() => setSymbolsTxt(SP500_TOP_10)}
            className="text-accent hover:underline text-[11px]"
          >top10</button>
          <button
            onClick={() => setSymbolsTxt(sp500Tickers.slice(0, 30).join(','))}
            className="text-accent hover:underline text-[11px]"
            disabled={sp500Tickers.length === 0}
          >sp30</button>
        </label>

        <label className="text-xs flex items-center gap-2">
          <span className="text-subtle">Horizon</span>
          <input type="number" min={5} max={120} value={horizonDays}
            onChange={(e) => setHorizonDays(Number(e.target.value))}
            className="w-14 bg-surface border border-border rounded px-2 py-1 text-ink" />
        </label>
        <label className="text-xs flex items-center gap-2">
          <span className="text-subtle">Strikes</span>
          <input type="number" min={2} max={31} value={nStrikes}
            onChange={(e) => setNStrikes(Number(e.target.value))}
            className="w-14 bg-surface border border-border rounded px-2 py-1 text-ink" />
        </label>
        <label className="text-xs flex items-center gap-2">
          <span className="text-subtle">Exp (DTE)</span>
          <input type="text" value={expiriesTxt}
            onChange={(e) => setExpiriesTxt(e.target.value)}
            className="w-24 bg-surface border border-border rounded px-2 py-1 text-ink font-mono" />
        </label>
        <label className="text-xs flex items-center gap-2">
          <span className="text-subtle">MC paths</span>
          <input type="number" min={16} max={4096} value={nPaths}
            onChange={(e) => setNPaths(Number(e.target.value))}
            className="w-16 bg-surface border border-border rounded px-2 py-1 text-ink" />
        </label>

        <button
          onClick={kickOff}
          disabled={optionRankingBusy || symbols.length === 0}
          className="bg-accent text-black text-xs font-bold px-3 py-2 rounded disabled:opacity-50 ml-auto"
        >
          {optionRankingBusy ? 'Fusing…' : 'Rank Options'}
        </button>
      </div>

      {/* Fusion-weight sliders */}
      <div className="bg-panel border border-border rounded p-3 flex flex-wrap gap-6 text-xs">
        <WSlider label="w_edge (Q−P)" value={wEdge}   setValue={setWEdge}   color="#3b82f6" />
        <WSlider label="w_news"       value={wNews}   setValue={setWNews}   color="#ef4444" />
        <WSlider label="w_convex"     value={wConvex} setValue={setWConvex} color="#22c55e" />
        <div className="text-subtle font-mono ml-auto">
          Σ weights = <span className={
            Math.abs(weightsSum - 1) < 0.01 ? 'text-ink' : 'text-yellow-400'
          }>{weightsSum.toFixed(2)}</span>
          {Math.abs(weightsSum - 1) > 0.01 && <span className="ml-1">(not 1.0)</span>}
        </div>
      </div>

      {/* Result */}
      {!result ? (
        <div className="flex-1 flex items-center justify-center border border-border rounded bg-panel text-xs text-subtle italic py-8">
          {optionRankingBusy
            ? 'Bates pricing + MC hedged paths + z-score fusion…'
            : 'Pick a bot run and hit "Rank Options" to see the fused option ranking.'}
        </div>
      ) : (
        <div className="bg-panel border border-border rounded overflow-hidden">
          <div className="px-3 py-2 border-b border-border flex items-center justify-between">
            <div className="text-xs text-subtle uppercase tracking-widest font-bold">
              Top <select value={topN} onChange={(e) => setTopN(Number(e.target.value))}
                className="bg-surface border border-border rounded px-1 text-ink text-xs">
                {[10, 20, 40, 80, 200].map((n) => <option key={n} value={n}>{n}</option>)}
              </select> ranked by blended_option_score
              <span className="text-subtle normal-case ml-3">
                · {result.n_rows} rows scored
                · weights {result.weights.w_edge.toFixed(2)}/{result.weights.w_news.toFixed(2)}/{result.weights.w_convex.toFixed(2)}
              </span>
            </div>
          </div>

          <div className="overflow-y-auto max-h-[60vh] text-xs font-mono">
            <table className="w-full">
              <thead className="sticky top-0 bg-panel text-subtle text-[10px] uppercase tracking-widest">
                <tr>
                  <th className="text-right px-2 py-1">#</th>
                  <th className="text-left  px-2 py-1">Sym</th>
                  <th className="text-right px-2 py-1">K</th>
                  <th className="text-right px-2 py-1">DTE</th>
                  <th className="text-left  px-2 py-1">CP</th>
                  <th className="text-left  px-2 py-1">Side</th>
                  <th className="text-right px-2 py-1">Mkt IV</th>
                  <th className="text-right px-2 py-1">Model IV</th>
                  <th className="text-right px-2 py-1">Edge</th>
                  <th className="text-right px-2 py-1">$Edge</th>
                  <th className="text-left  px-2 py-1">z_edge</th>
                  <th className="text-left  px-2 py-1">z_news</th>
                  <th className="text-left  px-2 py-1">z_cvx</th>
                  <th className="text-right px-2 py-1">Blended</th>
                </tr>
              </thead>
              <tbody>
                {topRows.map((r) => (
                  <tr key={r.rank} className="border-t border-border/40">
                    <td className="px-2 py-1 text-right text-subtle">{r.rank}</td>
                    <td className="px-2 py-1 text-ink font-bold">{r.symbol}</td>
                    <td className="px-2 py-1 text-right">{r.strike.toFixed(1)}</td>
                    <td className="px-2 py-1 text-right">{r.dte_days}</td>
                    <td className={`px-2 py-1 ${r.right==='C'?'text-bull':'text-bear'}`}>{r.right}</td>
                    <td className="px-2 py-1">{sideBadge(r)}</td>
                    <td className="px-2 py-1 text-right">{fmtPct(r.market_iv, 1)}</td>
                    <td className="px-2 py-1 text-right">{fmtPct(r.model_iv, 1)}</td>
                    <td className={`px-2 py-1 text-right ${
                      r.edge_vol_pts > 0.005 ? 'text-bull'
                      : r.edge_vol_pts < -0.005 ? 'text-bear' : 'text-subtle'
                    }`}>
                      {fmtSigned(r.edge_vol_pts * 100, 2)}
                    </td>
                    <td className={`px-2 py-1 text-right ${
                      r.dollar_edge > 0 ? 'text-bull'
                      : r.dollar_edge < 0 ? 'text-bear' : 'text-subtle'
                    }`}>
                      {fmtSigned(r.dollar_edge, 2)}
                    </td>
                    <td className="px-2 py-1"><div className="flex items-center gap-2">
                      <ZBar z={r.z_bates_edge} />
                      <span className={zColor(r.z_bates_edge)}>{r.z_bates_edge.toFixed(2)}</span>
                    </div></td>
                    <td className="px-2 py-1"><div className="flex items-center gap-2">
                      <ZBar z={r.z_news_jump} />
                      <span className={zColor(r.z_news_jump)}>{r.z_news_jump.toFixed(2)}</span>
                    </div></td>
                    <td className="px-2 py-1"><div className="flex items-center gap-2">
                      <ZBar z={r.z_convexity} />
                      <span className={zColor(r.z_convexity)}>{r.z_convexity.toFixed(2)}</span>
                    </div></td>
                    <td className={`px-2 py-1 text-right font-bold ${
                      r.blended_option_score > 0.3 ? 'text-bull'
                      : r.blended_option_score < -0.3 ? 'text-bear' : 'text-ink'
                    }`}>
                      {fmtSigned(r.blended_option_score, 3)}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      )}
    </div>
  );
}

function WSlider({ label, value, setValue, color }:
  { label: string; value: number; setValue: (n: number) => void; color: string }) {
  return (
    <label className="flex flex-col gap-1">
      <span className="text-subtle text-[10px] uppercase tracking-widest">
        {label}
        <span className="ml-1 text-ink font-mono">{value.toFixed(2)}</span>
      </span>
      <div className="flex items-center gap-2">
        <input
          type="range" min={0} max={1} step={0.05} value={value}
          onChange={(e) => setValue(Number(e.target.value))}
          className="w-28 accent-current"
          style={{ color }}
        />
      </div>
    </label>
  );
}
