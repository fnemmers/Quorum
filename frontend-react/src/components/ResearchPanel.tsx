import { useEffect, useMemo, useState } from 'react';
import { useStore, jaccard, type BotRun } from '../store/useStore';

function fmtDate(ms: number): string {
  if (!ms) return '—';
  return new Date(ms).toISOString().slice(0, 16).replace('T', ' ');
}

function fmtPct(x: number | undefined): string {
  if (x === undefined || x === null || Number.isNaN(x)) return '—';
  return `${x >= 0 ? '+' : ''}${x.toFixed(2)}%`;
}

function pctClass(x: number | undefined): string {
  if (x === undefined || x === null || Number.isNaN(x)) return 'text-ink';
  return x >= 0 ? 'text-bull' : 'text-bear';
}

/* ── Runs list (left column) ──────────────────────────────────── */

function RunsList({
  runs, selectedId, onSelect, onRefresh,
}: {
  runs: BotRun[];
  selectedId: number | null;
  onSelect: (id: number) => void;
  onRefresh: () => void;
}) {
  return (
    <div className="bg-panel border border-border rounded p-3 space-y-2 w-72 flex-shrink-0">
      <div className="flex justify-between items-center">
        <div className="text-xs text-subtle uppercase tracking-widest font-bold">Bot Runs</div>
        <button
          onClick={onRefresh}
          className="text-xs text-accent hover:underline"
        >refresh</button>
      </div>
      <div className="space-y-1 max-h-[70vh] overflow-y-auto text-xs font-mono">
        {runs.length === 0 && (
          <div className="text-subtle italic">No runs yet. Kick off bot_runner.py.</div>
        )}
        {runs.map((r) => {
          const finished = r.finished_at > 0;
          const active   = r.id === selectedId;
          return (
            <button
              key={r.id}
              onClick={() => onSelect(r.id)}
              className={`w-full text-left px-2 py-1.5 rounded border transition ${
                active
                  ? 'border-accent bg-surface text-ink'
                  : 'border-border text-muted hover:border-accent hover:text-ink'
              }`}
            >
              <div className="flex justify-between">
                <span className="text-ink">#{r.id}</span>
                <span className={finished ? 'text-bull' : 'text-subtle'}>
                  {finished ? 'done' : 'live'}
                </span>
              </div>
              <div className="truncate">{r.label || '(unnamed)'}</div>
              <div className="text-subtle">
                {r.n_bots_actual || r.n_bots_target} bots · {r.hold_days}d · {fmtDate(r.started_at)}
              </div>
            </button>
          );
        })}
      </div>
    </div>
  );
}

/* ── Aggregation section ──────────────────────────────────────── */

function AggregationSection({ runId }: { runId: number | null }) {
  const { aggregates, runAggregate, researchBusy } = useStore();
  const [k, setK] = useState(20);

  const agg = runId !== null ? aggregates[runId] : undefined;

  return (
    <div className="bg-panel border border-border rounded p-3 space-y-3">
      <div className="flex justify-between items-center">
        <div className="text-xs text-subtle uppercase tracking-widest font-bold">Aggregation (Top-K)</div>
        {runId !== null && (
          <button
            onClick={() => runAggregate(runId, k)}
            disabled={researchBusy}
            className="text-xs px-3 py-1 rounded bg-accent text-surface font-bold disabled:opacity-50"
          >
            {agg ? 're-aggregate' : 'aggregate'}
          </button>
        )}
      </div>

      <div className="text-xs font-mono">
        <div className="flex justify-between mb-1">
          <span className="text-muted">K</span>
          <span className="text-ink">{k}</span>
        </div>
        <input
          type="range" min={5} max={50} value={k}
          onChange={(e) => setK(Number(e.target.value))}
          className="w-full accent-accent"
        />
      </div>

      {runId === null && (
        <div className="text-xs text-subtle italic">Select a run to aggregate.</div>
      )}

      {agg && (
        <>
          <div className="text-xs text-muted">
            {agg.n_picks_total} total picks · top {agg.top.length}
          </div>
          <div className="grid grid-cols-2 gap-1 text-xs font-mono max-h-64 overflow-y-auto">
            {agg.top.map((p, i) => (
              <div key={p.symbol} className="flex justify-between border border-border rounded px-2 py-1">
                <span className="text-subtle">{i + 1}.</span>
                <span className="text-ink flex-1 ml-2 font-bold">{p.symbol}</span>
                <span className="text-accent">{p.count}</span>
              </div>
            ))}
          </div>
        </>
      )}
    </div>
  );
}

/* ── Convergence section ──────────────────────────────────────── */

function ConvergenceSection() {
  const { botRuns, aggregates, runAggregate, researchBusy } = useStore();
  const [aId, setAId] = useState<number | ''>('');
  const [bId, setBId] = useState<number | ''>('');
  const [k, setK]     = useState(20);

  const aggA = aId !== '' ? aggregates[aId] : undefined;
  const aggB = bId !== '' ? aggregates[bId] : undefined;

  const fetchBoth = () => {
    if (aId !== '') runAggregate(aId, k);
    if (bId !== '' && bId !== aId) runAggregate(bId, k);
  };

  const { similarity, onlyA, onlyB, both } = useMemo(() => {
    if (!aggA || !aggB) return { similarity: 0, onlyA: [], onlyB: [], both: [] };
    const A = new Set(aggA.top.map((x) => x.symbol));
    const B = new Set(aggB.top.map((x) => x.symbol));
    const both: string[]  = [...A].filter((s) => B.has(s)).sort();
    const onlyA: string[] = [...A].filter((s) => !B.has(s)).sort();
    const onlyB: string[] = [...B].filter((s) => !A.has(s)).sort();
    return { similarity: jaccard(aggA.top, aggB.top), onlyA, onlyB, both };
  }, [aggA, aggB]);

  const stable = similarity >= 0.9;

  return (
    <div className="bg-panel border border-border rounded p-3 space-y-3">
      <div className="text-xs text-subtle uppercase tracking-widest font-bold">Convergence (Jaccard)</div>

      <div className="grid grid-cols-2 gap-2 text-xs font-mono">
        <div>
          <div className="text-muted mb-1">Run A</div>
          <select
            value={aId}
            onChange={(e) => setAId(e.target.value === '' ? '' : Number(e.target.value))}
            className="w-full bg-surface border border-border rounded px-2 py-1 text-ink"
          >
            <option value="">—</option>
            {botRuns.map((r) => (
              <option key={r.id} value={r.id}>#{r.id} {r.label}</option>
            ))}
          </select>
        </div>
        <div>
          <div className="text-muted mb-1">Run B</div>
          <select
            value={bId}
            onChange={(e) => setBId(e.target.value === '' ? '' : Number(e.target.value))}
            className="w-full bg-surface border border-border rounded px-2 py-1 text-ink"
          >
            <option value="">—</option>
            {botRuns.map((r) => (
              <option key={r.id} value={r.id}>#{r.id} {r.label}</option>
            ))}
          </select>
        </div>
      </div>

      <div className="text-xs font-mono">
        <div className="flex justify-between mb-1">
          <span className="text-muted">K (top-K set size)</span>
          <span className="text-ink">{k}</span>
        </div>
        <input
          type="range" min={5} max={50} value={k}
          onChange={(e) => setK(Number(e.target.value))}
          className="w-full accent-accent"
        />
      </div>

      <button
        onClick={fetchBoth}
        disabled={researchBusy || aId === '' || bId === ''}
        className="w-full py-1.5 rounded bg-accent text-surface font-bold text-sm disabled:opacity-50"
      >
        fetch both top-{k}s
      </button>

      {aggA && aggB && (
        <>
          <div className="border-t border-border pt-2 text-center">
            <div className="text-xs text-muted uppercase tracking-widest font-bold">Jaccard Similarity</div>
            <div className={`text-3xl font-mono ${stable ? 'text-bull' : 'text-ink'}`}>
              {similarity.toFixed(3)}
            </div>
            <div className={`text-xs ${stable ? 'text-bull' : 'text-subtle'}`}>
              {stable ? 'stable (≥ 0.9)' : 'not yet stable'}
            </div>
          </div>

          <div className="grid grid-cols-3 gap-2 text-xs font-mono">
            <div>
              <div className="text-accent mb-1">both ({both.length})</div>
              <div className="space-y-0.5 max-h-40 overflow-y-auto">
                {both.map((s) => <div key={s} className="text-ink">{s}</div>)}
              </div>
            </div>
            <div>
              <div className="text-muted mb-1">only A ({onlyA.length})</div>
              <div className="space-y-0.5 max-h-40 overflow-y-auto">
                {onlyA.map((s) => <div key={s} className="text-bear">{s}</div>)}
              </div>
            </div>
            <div>
              <div className="text-muted mb-1">only B ({onlyB.length})</div>
              <div className="space-y-0.5 max-h-40 overflow-y-auto">
                {onlyB.map((s) => <div key={s} className="text-bear">{s}</div>)}
              </div>
            </div>
          </div>
        </>
      )}
    </div>
  );
}

/* ── Backtest section ─────────────────────────────────────────── */

function BacktestSection({ runId }: { runId: number | null }) {
  const { backtests, runBacktest, researchBusy } = useStore();
  const [startDate, setStartDate] = useState('2025-07-01');
  const [holdDays,  setHoldDays]  = useState(61);
  const [k,         setK]         = useState(20);

  const bt = runId !== null ? backtests[runId] : undefined;

  return (
    <div className="bg-panel border border-border rounded p-3 space-y-3">
      <div className="text-xs text-subtle uppercase tracking-widest font-bold">Backtest</div>

      {runId === null && (
        <div className="text-xs text-subtle italic">Select a run to backtest.</div>
      )}

      {runId !== null && (
        <>
          <div className="grid grid-cols-3 gap-2 text-xs font-mono">
            <div>
              <div className="text-muted mb-1">start</div>
              <input
                type="date"
                value={startDate}
                onChange={(e) => setStartDate(e.target.value)}
                className="w-full bg-surface border border-border rounded px-2 py-1 text-ink"
              />
            </div>
            <div>
              <div className="text-muted mb-1">hold (days)</div>
              <input
                type="number" min={1} max={365}
                value={holdDays}
                onChange={(e) => setHoldDays(Number(e.target.value))}
                className="w-full bg-surface border border-border rounded px-2 py-1 text-ink"
              />
            </div>
            <div>
              <div className="text-muted mb-1">K</div>
              <input
                type="number" min={1} max={50}
                value={k}
                onChange={(e) => setK(Number(e.target.value))}
                className="w-full bg-surface border border-border rounded px-2 py-1 text-ink"
              />
            </div>
          </div>

          <button
            onClick={() => runBacktest(runId, startDate, holdDays, k)}
            disabled={researchBusy}
            className="w-full py-1.5 rounded bg-accent text-surface font-bold text-sm disabled:opacity-50"
          >
            run backtest
          </button>

          {bt && (
            <div className="border-t border-border pt-3 space-y-2 text-xs font-mono">
              <div className="text-muted">
                {bt.start_date} → {bt.end_date} ({bt.hold_days}d) · {bt.n_used} used / {bt.n_skipped} skipped
              </div>
              <div className="grid grid-cols-3 gap-2">
                <Metric label="portfolio"  value={fmtPct(bt.port_return)}  cls={pctClass(bt.port_return)} />
                <Metric label="SPY"        value={fmtPct(bt.bench_return)} cls={pctClass(bt.bench_return)} />
                <Metric label="alpha"      value={fmtPct(bt.alpha)}        cls={pctClass(bt.alpha)} />
                <Metric label="Sharpe"     value={bt.sharpe.toFixed(2)}    cls={bt.sharpe >= 1 ? 'text-bull' : 'text-ink'} />
                <Metric label="max DD"     value={fmtPct(bt.max_dd)}       cls="text-bear" />
                <Metric label="hit rate"   value={`${bt.hit_rate.toFixed(1)}%`} cls="text-ink" />
              </div>
            </div>
          )}
        </>
      )}
    </div>
  );
}

function Metric({ label, value, cls }: { label: string; value: string; cls: string }) {
  return (
    <div className="border border-border rounded p-2">
      <div className="text-subtle text-[10px] uppercase tracking-widest font-bold">{label}</div>
      <div className={`text-base ${cls}`}>{value}</div>
    </div>
  );
}

/* ── Top-level panel ──────────────────────────────────────────── */

export default function ResearchPanel() {
  const { botRuns, fetchBotRuns, researchError, clearResearchError, bridgeConnected } = useStore();
  const [selectedId, setSelectedId] = useState<number | null>(null);

  useEffect(() => {
    if (bridgeConnected) fetchBotRuns();
  }, [bridgeConnected, fetchBotRuns]);

  useEffect(() => {
    if (selectedId === null && botRuns.length > 0) setSelectedId(botRuns[0].id);
  }, [botRuns, selectedId]);

  return (
    <div className="flex flex-1 gap-2 p-2 overflow-hidden">
      <RunsList
        runs={botRuns}
        selectedId={selectedId}
        onSelect={setSelectedId}
        onRefresh={() => fetchBotRuns()}
      />

      <div className="flex-1 flex flex-col gap-2 overflow-y-auto">
        {researchError && (
          <div className="bg-bear/20 border border-bear rounded p-2 text-xs font-mono text-bear flex justify-between">
            <span>{researchError}</span>
            <button onClick={clearResearchError} className="underline">dismiss</button>
          </div>
        )}
        <AggregationSection runId={selectedId} />
        <ConvergenceSection />
        <BacktestSection    runId={selectedId} />
      </div>
    </div>
  );
}
