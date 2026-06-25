import { useMemo, useState } from 'react';
import { useStore, type RebalanceEvent, type RankingResult } from '../store/useStore';
import MCPathBundlePanel from './MCPathBundlePanel';
import HestonSurfacePanel from './HestonSurfacePanel';
import HestonDiagnosticsPanel from './HestonDiagnosticsPanel';

/*
 * TickEvaluationPanel  –  The per-symbol evaluation workspace.
 *
 *   - For *held* positions: shows rebalance_check results (auto/notify
 *     /escalate decision, obscurity bar, primary driver, suggested
 *     action) once "Re-evaluate" is hit.
 *
 *   - For *any selected symbol* (held or not): computes the obscurity
 *     score client-side from the latest blended ranking, plus the
 *     would-be decision/driver. The same formula as rebalance.c on the
 *     backend, but it can fire on any symbol the user clicks — no
 *     need for the user to be holding it.
 *
 *   - MC Path Bundle + Heston Vol Surface render side-by-side below
 *     the eval table so the panel is a one-stop view of whatever
 *     ticker is currently selected.
 */

/* ── Client-side obscurity ────────────────────────────────────── */
/* Mirror of rebalance.c so a freshly-selected symbol gets evaluated
 * immediately, without round-tripping through rebalance_check. */

const W_GAP     = 0.35;
const W_LLM     = 0.25;
const W_BREACH  = 0.25;
const W_HORIZON = 0.15;

const AUTO_THRESHOLD     = 0.25;
const ESCALATE_THRESHOLD = 0.60;
const ES_RISK_LIMIT      = -0.10;

type EvalDecision = 'auto' | 'notify' | 'escalate' | 'hold';

interface SymbolEval {
  symbol: string;
  rank: number;
  blended_score: number;
  exit_threshold: number;
  score_gap_clarity: number;
  llm_agreement: number;
  heston_breach: number;
  horizon_maturity: number;
  clarity: number;
  obscurity: number;
  primary_driver: string;
  decision: EvalDecision;

  bot_count: number;
  bot_disagreement: number;
  z_bot: number;
  z_heston: number;
  expected_return: number;
  forward_vol: number;
  es_95: number;
  prob_loss: number;
}

function clip01(x: number) {
  return Math.max(0, Math.min(1, x));
}

function evaluateSymbol(
  ranking: RankingResult,
  symbol: string,
  daysHeld: number,
  intendedHoldDays: number,
  k: number,
): SymbolEval | null {
  const entryIdx = ranking.ranked.findIndex((r) => r.symbol === symbol);
  if (entryIdx < 0) return null;
  const e = ranking.ranked[entryIdx];

  // exit threshold = score at the band edge (rank 2*K). Below this we
  // consider the position no longer in the trading band.
  const bandEdge = Math.min(ranking.ranked.length - 1, Math.max(0, 2 * k - 1));
  const exit_threshold = ranking.ranked[bandEdge].blended_score;

  const sigma = ranking.sigma_blend > 1e-9 ? ranking.sigma_blend : 1.0;
  const gap = clip01(Math.abs(e.blended_score - exit_threshold) / sigma);
  const llm = clip01(1 - e.bot_disagreement);
  const breach = e.es_95 <= ES_RISK_LIMIT ? 1 : 0;
  const horizon = intendedHoldDays > 0 ? clip01(daysHeld / intendedHoldDays) : 0;

  const clarity = W_GAP * gap + W_LLM * llm + W_BREACH * breach + W_HORIZON * horizon;
  const obscurity = 1 - clarity;

  // Primary driver = weighted clarity component with the LOWEST value
  // (i.e. the biggest drag on confidence).
  const contribs: Array<[string, number]> = [
    ['score_gap',       W_GAP     * gap],
    ['llm_agreement',   W_LLM     * llm],
    ['heston_breach',   W_BREACH  * breach],
    ['horizon_maturity',W_HORIZON * horizon],
  ];
  contribs.sort((a, b) => a[1] - b[1]);
  const primary_driver = contribs[0][0];

  // Decision routing — same thresholds as the backend.
  let decision: EvalDecision;
  if (e.blended_score >= exit_threshold) {
    decision = 'hold';
  } else if (obscurity < AUTO_THRESHOLD) {
    decision = 'auto';
  } else if (obscurity < ESCALATE_THRESHOLD) {
    decision = 'notify';
  } else {
    decision = 'escalate';
  }

  return {
    symbol: e.symbol,
    rank: entryIdx + 1,
    blended_score: e.blended_score,
    exit_threshold,
    score_gap_clarity: gap,
    llm_agreement: llm,
    heston_breach: breach,
    horizon_maturity: horizon,
    clarity,
    obscurity,
    primary_driver,
    decision,
    bot_count: e.bot_count,
    bot_disagreement: e.bot_disagreement,
    z_bot: e.z_bot,
    z_heston: e.z_heston,
    expected_return: e.expected_return,
    forward_vol: e.forward_vol,
    es_95: e.es_95,
    prob_loss: e.prob_loss,
  };
}

/* ── UI helpers ──────────────────────────────────────────────── */

function decisionBadge(d: EvalDecision | RebalanceEvent['decision']) {
  const base = 'px-1.5 py-0.5 rounded text-[10px] uppercase tracking-widest font-bold';
  switch (d) {
    case 'auto':     return <span className={`${base} bg-bull/30 text-bull border border-bull`}>AUTO</span>;
    case 'notify':   return <span className={`${base} bg-yellow-500/20 text-yellow-400 border border-yellow-500`}>NOTIFY</span>;
    case 'escalate': return <span className={`${base} bg-bear/30 text-bear border border-bear`}>ESCALATE</span>;
    case 'hold':     return <span className={`${base} bg-surface text-subtle border border-border`}>HOLD</span>;
    default:         return <span className={base}>{d}</span>;
  }
}

function actionBadge(a: RebalanceEvent['suggested_action']) {
  if (a === 'none') return <span className="text-subtle text-xs">—</span>;
  const cls = a === 'sell' ? 'text-bear' : a === 'flip' ? 'text-bear' : 'text-yellow-400';
  return <span className={`text-xs font-bold uppercase ${cls}`}>{a}</span>;
}

function obscBar(x: number) {
  const w = clip01(x) * 100;
  const color = x >= ESCALATE_THRESHOLD ? '#d11'
              : x >= AUTO_THRESHOLD     ? '#d9a900'
              :                           '#0a0';
  return (
    <div className="flex items-center gap-2">
      <div className="h-2 w-20 bg-surface border border-border rounded overflow-hidden">
        <div className="h-full" style={{ width: `${w}%`, background: color }} />
      </div>
      <span className="text-xs font-mono w-10 text-right">{x.toFixed(2)}</span>
    </div>
  );
}

function fmtPct(x: number) {
  if (x == null || Number.isNaN(x)) return '—';
  return `${x >= 0 ? '+' : ''}${(x * 100).toFixed(2)}%`;
}

/* ── Selected symbol detail card ─────────────────────────────── */

function SelectedSymbolCard({ symEval, isHeld, daysHeld, intendedHoldDays }:
  { symEval: SymbolEval; isHeld: boolean; daysHeld: number; intendedHoldDays: number }) {

  const components: Array<[string, number]> = [
    ['score_gap',        symEval.score_gap_clarity],
    ['llm_agreement',    symEval.llm_agreement],
    ['heston_breach',    symEval.heston_breach],
    ['horizon_maturity', symEval.horizon_maturity],
  ];

  return (
    <div className="border-t border-border bg-surface/40 px-3 py-3 space-y-3">
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-3">
          <div className="text-sm font-bold text-ink">{symEval.symbol}</div>
          <div className="text-xs text-subtle">rank #{symEval.rank}</div>
          <div className="text-xs text-subtle">{isHeld ? 'HELD' : 'not held'}</div>
          {decisionBadge(symEval.decision)}
        </div>
        <div className="text-xs font-mono">
          obscurity <span className="text-ink font-bold">{symEval.obscurity.toFixed(2)}</span>
          <span className="text-subtle"> · driver </span>
          <span className="text-yellow-400">{symEval.primary_driver}</span>
        </div>
      </div>

      <div className="grid grid-cols-4 gap-2 text-xs font-mono">
        {components.map(([name, v]) => (
          <div key={name} className="border border-border rounded p-2 bg-panel">
            <div className="text-[10px] text-subtle uppercase tracking-widest">
              {name.replace(/_/g, ' ')}
            </div>
            <div className="text-ink">{v.toFixed(2)}</div>
            <div className="h-1 mt-1 bg-surface rounded">
              <div className="h-full bg-accent rounded" style={{ width: `${clip01(v) * 100}%` }} />
            </div>
          </div>
        ))}
      </div>

      <div className="grid grid-cols-6 gap-2 text-xs font-mono">
        <Metric label="blend"    value={symEval.blended_score.toFixed(3)} />
        <Metric label="exit"     value={symEval.exit_threshold.toFixed(3)} />
        <Metric label="z bot"    value={symEval.z_bot.toFixed(2)} />
        <Metric label="z hest"   value={symEval.z_heston.toFixed(2)} />
        <Metric label="bot dis"  value={symEval.bot_disagreement.toFixed(2)} />
        <Metric label="bots"     value={`${symEval.bot_count}`} />
        <Metric label="E[r]"     value={fmtPct(symEval.expected_return)} />
        <Metric label="vol"      value={fmtPct(symEval.forward_vol)} />
        <Metric label="ES95"     value={fmtPct(symEval.es_95)} cls="text-bear" />
        <Metric label="P(loss>5%)" value={fmtPct(symEval.prob_loss)} cls="text-bear" />
        <Metric label="days held" value={isHeld ? `${daysHeld}/${intendedHoldDays}` : '—'} />
        <Metric label="clarity"  value={symEval.clarity.toFixed(2)} />
      </div>
    </div>
  );
}

function Metric({ label, value, cls }: { label: string; value: string; cls?: string }) {
  return (
    <div className="border border-border rounded p-1.5 bg-panel">
      <div className="text-[10px] text-subtle uppercase tracking-widest">{label}</div>
      <div className={`text-ink ${cls ?? ''}`}>{value}</div>
    </div>
  );
}

/* ── Top-level panel ─────────────────────────────────────────── */

export default function TickEvaluationPanel() {
  const {
    holdings, quotes, rankings, botRuns, rebalanceCheck,
    runRebalanceCheck, researchBusy,
    selectedSymbol, selectSymbol, heldSince,
  } = useStore();

  const [runId, setRunId] = useState<number | null>(null);
  const [k,     setK]     = useState(20);
  const [hdays, setHdays] = useState(21);

  const compiledRunIds = Object.keys(rankings).map(Number);
  const effectiveRun =
    runId !== null ? runId : compiledRunIds[0] ?? botRuns[0]?.id ?? null;

  const eventsBySymbol: Record<string, RebalanceEvent> = {};
  if (rebalanceCheck) {
    for (const e of rebalanceCheck.events) eventsBySymbol[e.symbol] = e;
  }

  /* Compute the selected-symbol evaluation client-side. */
  const symbolEval = useMemo(() => {
    if (!selectedSymbol || effectiveRun === null) return null;
    const ranking = rankings[effectiveRun];
    if (!ranking) return null;
    const since = heldSince[selectedSymbol];
    const days = since ? Math.max(0, Math.floor((Date.now() - since) / 86400000)) : 0;
    return evaluateSymbol(ranking, selectedSymbol, days, hdays, k);
  }, [selectedSymbol, effectiveRun, rankings, heldSince, hdays, k]);

  const isHeld = !!holdings.find((h) => h.symbol === selectedSymbol);
  const since = selectedSymbol ? heldSince[selectedSymbol] : undefined;
  const daysHeld = since ? Math.max(0, Math.floor((Date.now() - since) / 86400000)) : 0;

  return (
    <div className="bg-panel border border-border rounded flex flex-col overflow-hidden">
      <div className="px-3 py-2 border-b border-border flex items-center justify-between">
        <div className="text-xs text-subtle uppercase tracking-widest font-bold">
          Tick Evaluation
          {selectedSymbol && <span className="text-accent ml-2 normal-case">· {selectedSymbol}</span>}
        </div>
        <div className="flex items-center gap-2 text-xs font-mono">
          <span className="text-subtle">run</span>
          <select
            value={effectiveRun ?? ''}
            onChange={(e) =>
              setRunId(e.target.value === '' ? null : Number(e.target.value))
            }
            className="bg-surface border border-border rounded px-2 py-0.5 text-ink"
          >
            <option value="">—</option>
            {botRuns.map((r) => (
              <option key={r.id} value={r.id}>#{r.id} {r.label}</option>
            ))}
          </select>
          <span className="text-subtle ml-2">K</span>
          <input
            type="number" min={5} max={50} value={k}
            onChange={(e) => setK(Number(e.target.value))}
            className="bg-surface border border-border rounded px-2 py-0.5 text-ink w-12"
          />
          <span className="text-subtle ml-2">hold</span>
          <input
            type="number" min={1} max={120} value={hdays}
            onChange={(e) => setHdays(Number(e.target.value))}
            className="bg-surface border border-border rounded px-2 py-0.5 text-ink w-12"
          />
          <button
            onClick={() => effectiveRun !== null &&
                          runRebalanceCheck(effectiveRun, k, hdays, undefined, hdays)}
            disabled={effectiveRun === null || holdings.length === 0 || researchBusy}
            className="bg-accent text-black text-xs font-bold px-3 py-1 rounded ml-2 disabled:opacity-50"
          >
            {researchBusy ? '…' : 'Re-evaluate Holdings'}
          </button>
          {selectedSymbol && (
            <button
              onClick={() => selectSymbol(null)}
              className="text-subtle hover:text-ink text-xs ml-2"
            >clear</button>
          )}
        </div>
      </div>

      {/* ── Holdings rebalance table ──────────────────────── */}
      {holdings.length === 0 ? (
        <div className="px-3 py-3 text-xs italic text-subtle">
          No holdings. Click any ticker in the Result table to evaluate it
          here.
        </div>
      ) : (
        <div className="overflow-y-auto max-h-[40vh] text-xs font-mono">
          <table className="w-full">
            <thead className="sticky top-0 bg-panel text-subtle text-[10px] uppercase tracking-widest">
              <tr>
                <th className="text-left  px-2 py-1">Sym</th>
                <th className="text-right px-2 py-1">Price</th>
                <th className="text-right px-2 py-1">Old → New</th>
                <th className="text-left  px-2 py-1 w-44">Obscurity</th>
                <th className="text-left  px-2 py-1">Driver</th>
                <th className="text-left  px-2 py-1">Decision</th>
                <th className="text-left  px-2 py-1">Action</th>
              </tr>
            </thead>
            <tbody>
              {holdings.map((h) => {
                const ev    = eventsBySymbol[h.symbol];
                const price = quotes[h.symbol]?.price ?? h.current ?? h.avg_price;
                const isSel = h.symbol === selectedSymbol;
                return (
                  <tr
                    key={h.symbol}
                    onClick={() => selectSymbol(h.symbol, hdays)}
                    className={`border-t border-border/40 align-middle cursor-pointer transition ${
                      isSel ? 'bg-accent/20' : 'hover:bg-surface'
                    }`}
                    title="Click to evaluate"
                  >
                    <td className="px-2 py-1 text-ink font-bold">{h.symbol}</td>
                    <td className="px-2 py-1 text-right">${price.toFixed(2)}</td>
                    <td className="px-2 py-1 text-right">
                      {ev
                        ? `${ev.old_blend.toFixed(2)} → ${ev.new_blend.toFixed(2)}`
                        : <span className="text-subtle">—</span>}
                    </td>
                    <td className="px-2 py-1">
                      {ev
                        ? obscBar(ev.obscurity)
                        : <span className="text-subtle">—</span>}
                    </td>
                    <td className="px-2 py-1 text-subtle">
                      {ev ? ev.primary_driver : '—'}
                    </td>
                    <td className="px-2 py-1">
                      {ev ? decisionBadge(ev.decision)
                          : <span className="text-subtle">—</span>}
                    </td>
                    <td className="px-2 py-1">
                      {ev ? actionBadge(ev.suggested_action)
                          : <span className="text-subtle">—</span>}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}

      {/* ── Selected-symbol detail (client-side obscurity) ── */}
      {selectedSymbol && symbolEval && (
        <SelectedSymbolCard
          symEval={symbolEval}
          isHeld={isHeld}
          daysHeld={daysHeld}
          intendedHoldDays={hdays}
        />
      )}
      {selectedSymbol && !symbolEval && (
        <div className="border-t border-border bg-surface/40 px-3 py-2 text-xs italic text-subtle">
          {selectedSymbol} isn't in the current compiled ranking. Compile a
          run that includes it, or click a symbol from the Result table.
        </div>
      )}

      {/* ── MC Paths + Vol Surface for the selected symbol ── */}
      <div className="border-t border-border p-2 grid grid-cols-2 gap-2">
        <MCPathBundlePanel />
        <HestonSurfacePanel />
      </div>

      {/* ── Layer-1 Heston calibration diagnostics ───────── */}
      {selectedSymbol && (
        <div className="border-t border-border p-2">
          <HestonDiagnosticsPanel />
        </div>
      )}

      {/* ── Debriefs from the latest rebalance_check ──────── */}
      {rebalanceCheck && rebalanceCheck.events.some((e) => e.decision !== 'hold') && (
        <div className="border-t border-border bg-surface px-3 py-2 space-y-1 text-[11px] font-mono max-h-32 overflow-y-auto">
          <div className="text-[10px] text-subtle uppercase tracking-widest font-bold">
            Holdings debriefs · exit_threshold {rebalanceCheck.exit_threshold.toFixed(3)}
          </div>
          {rebalanceCheck.events
            .filter((e) => e.decision !== 'hold')
            .map((e) => (
              <div key={e.event_id} className="text-ink leading-snug">
                {e.debrief}
              </div>
            ))}
        </div>
      )}
    </div>
  );
}
