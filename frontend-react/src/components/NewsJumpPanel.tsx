import { useMemo } from 'react';
import { useStore, type NewsJumpSignal } from '../store/useStore';

/*
 * NewsJumpPanel  --  Per-ticker news → Bates-jump-parameter overlay.
 *
 * For every symbol in the current top-K compiled ranking (plus the
 * user's holdings and the selected symbol), shows the freshest
 * news_jump_signal row: article count, keyword event class, sentiment,
 * and the resulting (Δλ, Δμ_J, Δσ_J) that get folded into the Bates
 * pricer.
 */

function eventBadge(cls: string) {
  if (!cls) return <span className="text-subtle">—</span>;
  const negatives = ['earn', 'guide', 'down', 'lit', 'layoffs'];
  const positives = ['beat', 'up', 'buyback', 'ma'];
  const base = 'px-1.5 py-0.5 rounded text-[10px] uppercase tracking-widest font-bold';
  if (negatives.includes(cls))
    return <span className={`${base} bg-bear/30 text-bear border border-bear`}>{cls}</span>;
  if (positives.includes(cls))
    return <span className={`${base} bg-bull/30 text-bull border border-bull`}>{cls}</span>;
  return <span className={`${base} bg-surface text-subtle border border-border`}>{cls}</span>;
}

function sentimentBar(x: number) {
  const clamped = Math.max(-1, Math.min(1, x));
  const w = Math.abs(clamped) * 50;
  const color = clamped >= 0 ? '#0a0' : '#d11';
  return (
    <div className="flex items-center gap-2">
      <div className="h-2 w-20 bg-surface border border-border rounded relative">
        <div className="absolute inset-y-0 left-1/2 w-px bg-border" />
        <div
          className="absolute inset-y-0"
          style={{
            width: `${w}%`,
            background: color,
            left: clamped >= 0 ? '50%' : `${50 - w}%`,
          }}
        />
      </div>
      <span className="text-xs font-mono w-10 text-right">
        {clamped >= 0 ? '+' : ''}{clamped.toFixed(2)}
      </span>
    </div>
  );
}

function fmtSigned(x: number, digits = 3) {
  if (x == null || Number.isNaN(x)) return '—';
  return `${x >= 0 ? '+' : ''}${x.toFixed(digits)}`;
}

export default function NewsJumpPanel() {
  const {
    newsJumpSignals, newsJumpBusy, newsJumpAsOf, newsJumpWindowHours,
    runNewsJumpStatus,
    selectedSymbol, optionRanking, batesBacktest,
  } = useStore();

  /* Symbols we want to display: selected ∪ current option-ranking universe. */
  const symbols = useMemo(() => {
    const s = new Set<string>();
    if (selectedSymbol) s.add(selectedSymbol);
    for (const r of optionRanking?.ranked ?? []) s.add(r.symbol);
    for (const r of batesBacktest?.rows    ?? []) s.add(r.symbol);
    return Array.from(s).sort();
  }, [selectedSymbol, optionRanking, batesBacktest]);

  const rows: NewsJumpSignal[] = symbols
    .map((sym) => newsJumpSignals[sym])
    .filter((r): r is NewsJumpSignal => !!r);

  const asOfStr = newsJumpAsOf
    ? new Date(newsJumpAsOf).toLocaleTimeString()
    : '—';

  return (
    <div className="bg-panel border border-border rounded overflow-hidden">
      <div className="px-3 py-2 border-b border-border flex items-center justify-between">
        <div className="text-xs text-subtle uppercase tracking-widest font-bold">
          News → Jump Params
          <span className="ml-2 normal-case text-subtle">
            · {newsJumpWindowHours}h window · as of {asOfStr}
          </span>
        </div>
        <button
          onClick={() => runNewsJumpStatus(symbols, newsJumpWindowHours)}
          disabled={newsJumpBusy || symbols.length === 0}
          className="text-accent hover:underline text-xs disabled:opacity-50"
        >
          {newsJumpBusy ? '…' : 'refresh'}
        </button>
      </div>

      {symbols.length === 0 ? (
        <div className="px-3 py-3 text-xs italic text-subtle">
          Compile a bot run or add a holding, then hit refresh to see per-ticker
          jump overlays.
        </div>
      ) : rows.length === 0 ? (
        <div className="px-3 py-3 text-xs italic text-subtle">
          No news_jump_signal rows yet. Hit refresh to trigger a rollup —
          empty result means no cached articles tag these symbols in the
          current window.
        </div>
      ) : (
        <div className="overflow-y-auto max-h-64 text-xs font-mono">
          <table className="w-full">
            <thead className="sticky top-0 bg-panel text-subtle text-[10px] uppercase tracking-widest">
              <tr>
                <th className="text-left  px-2 py-1">Sym</th>
                <th className="text-right px-2 py-1">Art</th>
                <th className="text-left  px-2 py-1">Event</th>
                <th className="text-left  px-2 py-1 w-40">Sentiment</th>
                <th className="text-right px-2 py-1">Δλ</th>
                <th className="text-right px-2 py-1">Δμ_J</th>
                <th className="text-right px-2 py-1">Δσ_J</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((r) => {
                const isSel = r.symbol === selectedSymbol;
                return (
                  <tr
                    key={r.symbol}
                    className={`border-t border-border/40 ${
                      isSel ? 'bg-accent/20' : ''
                    }`}
                  >
                    <td className="px-2 py-1 text-ink font-bold">{r.symbol}</td>
                    <td className="px-2 py-1 text-right">{r.n_articles}</td>
                    <td className="px-2 py-1">{eventBadge(r.event_class)}</td>
                    <td className="px-2 py-1">{sentimentBar(r.sentiment_avg)}</td>
                    <td className="px-2 py-1 text-right">{fmtSigned(r.lam_bump, 2)}</td>
                    <td className={`px-2 py-1 text-right ${
                      r.mu_j_bias > 0 ? 'text-bull'
                      : r.mu_j_bias < 0 ? 'text-bear' : 'text-subtle'
                    }`}>
                      {fmtSigned(r.mu_j_bias, 3)}
                    </td>
                    <td className="px-2 py-1 text-right">{fmtSigned(r.sigma_j_bump, 3)}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}
