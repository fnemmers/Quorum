import { useEffect, useMemo, useState } from 'react';
import { useStore, type RankedEntry } from '../store/useStore';
import { computeDiversity, type Diversity } from '../lib/diversity';

/*
 * CompilationPanel  –  Trigger the ranking pipeline on a chosen bot_run
 * and show the blended Result table beneath the controls.
 *
 * Compilation = aggregate -> heston_score_run -> ranking_blend on the
 * backend (the IPC handler does all three internally). Pick a run,
 * hit Compile, watch the Result panel populate.
 */

function fmtPct(x: number) {
  if (x == null || Number.isNaN(x)) return '—';
  return `${x >= 0 ? '+' : ''}${(x * 100).toFixed(2)}%`;
}

function scoreClass(x: number) {
  if (x == null || Number.isNaN(x)) return 'text-ink';
  return x >= 0 ? 'text-bull' : 'text-bear';
}

function disBar(x: number) {
  const w = Math.max(0, Math.min(1, x)) * 100;
  return (
    <div className="h-1.5 w-12 bg-surface border border-border rounded overflow-hidden inline-block align-middle">
      <div
        className="h-full"
        style={{
          width: `${w}%`,
          background: x > 0.6 ? 'var(--c-bear, #d11)' : x > 0.3 ? '#d9a900' : 'var(--c-bull, #0a0)',
        }}
      />
    </div>
  );
}

/* ── Diversity meter ─────────────────────────────────────────── */

function diversityColor(score: number) {
  if (score >= 0.6) return 'text-bull';
  if (score >= 0.3) return 'text-yellow-400';
  return 'text-bear';
}

function DiversityBar({ div, k }: { div: Diversity; k: number }) {
  if (div.total === 0) return null;
  const showOther = div.unmappedCount > 0;
  return (
    <div className="space-y-1">
      <div className="flex items-center gap-3 text-[10px] font-mono">
        <div className="text-subtle uppercase tracking-widest font-bold">
          Diversity
        </div>
        <div className={`text-base font-bold ${diversityColor(div.score)}`}>
          {div.score.toFixed(2)}
        </div>
        <div className="text-subtle">
          N<sub>eff</sub> {div.effectiveSectors.toFixed(1)}
          <span className="px-1">·</span>
          HHI {div.hhi.toFixed(2)}
          <span className="px-1">·</span>
          top: {div.topSector ?? '—'} ({(div.topSectorWeight * 100).toFixed(0)}%)
          {showOther && (
            <span className="px-1 text-yellow-500">
              · {div.unmappedCount} unmapped
            </span>
          )}
        </div>
      </div>

      <div className="flex h-2 w-full overflow-hidden rounded border border-border"
           title={`Top-${k} sector breakdown`}>
        {div.breakdown.map((s) => (
          <div
            key={s.sector}
            style={{ width: `${s.weight * 100}%`, background: s.color }}
            title={`${s.sector}: ${s.count} (${(s.weight * 100).toFixed(0)}%)`}
          />
        ))}
      </div>

      <div className="flex flex-wrap gap-x-3 gap-y-0.5 text-[10px] font-mono text-subtle">
        {div.breakdown.slice(0, 6).map((s) => (
          <span key={s.sector} className="flex items-center gap-1">
            <span
              className="inline-block w-2 h-2 rounded-sm"
              style={{ background: s.color }}
            />
            {s.sector.replace('Information Technology', 'Info Tech')
                     .replace('Communication Services', 'Comm Svcs')
                     .replace('Consumer Discretionary',  'Cons Disc')
                     .replace('Consumer Staples',         'Cons Stpl')}
            {' '}{(s.weight * 100).toFixed(0)}%
          </span>
        ))}
      </div>
    </div>
  );
}

/* ── Result table (under compilation controls) ────────────────── */

function ResultTable({
  ranked, horizonDays,
}: { ranked: RankedEntry[]; horizonDays: number }) {
  const { selectedSymbol, selectSymbol } = useStore();

  if (!ranked.length) {
    return (
      <div className="text-xs italic text-subtle px-2 py-3">
        No result yet. Pick a run and Compile.
      </div>
    );
  }
  return (
    <div className="overflow-y-auto max-h-[70vh] text-xs font-mono">
      <table className="w-full">
        <thead className="sticky top-0 bg-panel text-subtle text-[10px] uppercase tracking-widest">
          <tr>
            <th className="text-left  px-2 py-1">#</th>
            <th className="text-left  px-2 py-1">Sym</th>
            <th className="text-right px-2 py-1">Blend</th>
            <th className="text-right px-2 py-1">z bot</th>
            <th className="text-right px-2 py-1">z hest</th>
            <th className="text-right px-2 py-1">E[r]</th>
            <th className="text-right px-2 py-1">ES95</th>
            <th className="text-right px-2 py-1">Dis</th>
          </tr>
        </thead>
        <tbody>
          {ranked.map((r) => {
            const selected = r.symbol === selectedSymbol;
            return (
              <tr
                key={r.symbol}
                onClick={() => selectSymbol(r.symbol, horizonDays)}
                className={`border-t border-border/40 cursor-pointer transition ${
                  selected ? 'bg-accent/20' : 'hover:bg-surface'
                }`}
                title="Click to render MC path bundle"
              >
                <td className="px-2 py-0.5 text-subtle">{r.rank}</td>
                <td className="px-2 py-0.5 text-ink font-bold">{r.symbol}</td>
                <td className={`px-2 py-0.5 text-right ${scoreClass(r.blended_score)}`}>
                  {r.blended_score.toFixed(3)}
                </td>
                <td className={`px-2 py-0.5 text-right ${scoreClass(r.z_bot)}`}>
                  {r.z_bot.toFixed(2)}
                </td>
                <td className={`px-2 py-0.5 text-right ${scoreClass(r.z_heston)}`}>
                  {r.z_heston.toFixed(2)}
                </td>
                <td className={`px-2 py-0.5 text-right ${scoreClass(r.expected_return)}`}>
                  {fmtPct(r.expected_return)}
                </td>
                <td className="px-2 py-0.5 text-right text-bear">
                  {fmtPct(r.es_95)}
                </td>
                <td className="px-2 py-0.5 text-right">
                  {disBar(r.bot_disagreement)}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}

/* ── Top-level CompilationPanel ───────────────────────────────── */

export default function CompilationPanel() {
  const {
    botRuns, rankings, runRankingBlend, fetchBotRuns,
    researchBusy, researchError, clearResearchError, bridgeConnected,
  } = useStore();

  const [runId,       setRunId]       = useState<number | null>(null);
  const [k,           setK]           = useState(20);
  const [horizonDays, setHorizonDays] = useState(21);

  useEffect(() => {
    if (bridgeConnected) fetchBotRuns();
  }, [bridgeConnected, fetchBotRuns]);

  useEffect(() => {
    if (runId === null && botRuns.length > 0) setRunId(botRuns[0].id);
  }, [botRuns, runId]);

  const result    = runId !== null ? rankings[runId] : undefined;
  const lastFresh = useMemo(() => {
    if (!result) return null;
    return result.ranked.length > 0 ? 'fresh' : 'empty';
  }, [result]);

  const diversity = useMemo(() => {
    if (!result) return null;
    const topK = result.ranked.slice(0, k).map((r) => r.symbol);
    return computeDiversity(topK);
  }, [result, k]);

  return (
    <div className="bg-panel border border-border rounded flex flex-col overflow-hidden">
      {/* ── Compilation header ─────────────────────────────── */}
      <div className="px-3 py-2 border-b border-border flex items-center justify-between">
        <div className="text-xs text-subtle uppercase tracking-widest font-bold">
          Compilation
        </div>
        <div className="flex items-center gap-2 text-xs font-mono">
          <span className="text-subtle">run</span>
          <select
            value={runId ?? ''}
            onChange={(e) =>
              setRunId(e.target.value === '' ? null : Number(e.target.value))
            }
            className="bg-surface border border-border rounded px-2 py-0.5 text-ink"
          >
            <option value="">—</option>
            {botRuns.map((r) => (
              <option key={r.id} value={r.id}>
                #{r.id} {r.label}
              </option>
            ))}
          </select>

          <span className="text-subtle ml-2">K</span>
          <input
            type="number" min={5} max={50} value={k}
            onChange={(e) => setK(Number(e.target.value))}
            className="bg-surface border border-border rounded px-2 py-0.5 text-ink w-12"
          />

          <span className="text-subtle ml-2">horizon</span>
          <input
            type="number" min={5} max={120} value={horizonDays}
            onChange={(e) => setHorizonDays(Number(e.target.value))}
            className="bg-surface border border-border rounded px-2 py-0.5 text-ink w-14"
          />

          <button
            onClick={() => fetchBotRuns()}
            className="text-accent hover:underline ml-2"
          >refresh</button>

          <button
            onClick={() => runId !== null && runRankingBlend(runId, k, horizonDays)}
            disabled={runId === null || researchBusy}
            className="bg-accent text-black text-xs font-bold px-3 py-1 rounded ml-2 disabled:opacity-50"
          >
            {researchBusy ? '…' : 'Compile'}
          </button>
        </div>
      </div>

      {researchError && (
        <div className="bg-bear/20 border-b border-bear px-3 py-1 text-xs font-mono text-bear flex justify-between">
          <span>{researchError}</span>
          <button onClick={clearResearchError} className="underline">dismiss</button>
        </div>
      )}

      {/* ── Result panel (under compilation) ──────────────── */}
      <div className="border-t border-border">
        <div className="px-3 py-1 border-b border-border bg-surface flex items-center justify-between">
          <div className="text-[10px] text-subtle uppercase tracking-widest font-bold">
            Result
          </div>
          {result && (
            <div className="text-[10px] font-mono text-subtle">
              σ_blend {result.sigma_blend.toFixed(3)} ·
              w_bot {result.w_bot.toFixed(2)} · w_hest {result.w_heston.toFixed(2)} ·
              {result.ranked.length} rows
            </div>
          )}
        </div>

        {diversity && diversity.total > 0 && (
          <div className="px-3 py-2 border-b border-border">
            <DiversityBar div={diversity} k={k} />
          </div>
        )}

        <ResultTable ranked={result?.ranked ?? []} horizonDays={horizonDays} />
        {lastFresh === 'empty' && (
          <div className="text-xs italic text-subtle px-3 py-2">
            Result is empty — that run may have no picks ingested yet.
          </div>
        )}
      </div>
    </div>
  );
}
