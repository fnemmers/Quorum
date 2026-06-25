import { useStore } from '../store/useStore';

/*
 * HestonDiagnosticsPanel  –  Layer-1 calibration sanity checks for the
 * currently-selected symbol. Three blocks:
 *
 *   1. Feller condition (pass/fail badge + lhs vs rhs)
 *   2. Moment match — historical daily log-return moments vs the
 *      same moments computed from N Monte-Carlo paths under the
 *      calibrated dynamics. Big spreads in std / skew / kurt are the
 *      signal that κ, σ_v, or ρ defaults are wrong for this ticker.
 *   3. Realized-vol stability — 21-day rolling realized vol vs sqrt(θ),
 *      plus an empirical vol-of-vol estimate the user can compare to
 *      the literature default σ_v = 0.4.
 *
 * Overall score in [0,1] at the top combines the three.
 */

function scoreColor(s: number) {
  if (s >= 0.7) return 'text-bull';
  if (s >= 0.4) return 'text-yellow-400';
  return 'text-bear';
}

function passBadge(ok: boolean) {
  const base = 'px-1.5 py-0.5 rounded text-[10px] uppercase tracking-widest font-bold';
  return ok
    ? <span className={`${base} bg-bull/30 text-bull border border-bull`}>PASS</span>
    : <span className={`${base} bg-bear/30 text-bear border border-bear`}>FAIL</span>;
}

function fmtPct(x: number, digits = 2) {
  if (x == null || Number.isNaN(x)) return '—';
  return `${x >= 0 ? '+' : ''}${(x * 100).toFixed(digits)}%`;
}

function fmtNum(x: number, digits = 2) {
  if (x == null || Number.isNaN(x)) return '—';
  return x.toFixed(digits);
}

/* Bar showing the gap between a model value and a target. */
function MomentRow({ label, hist, sim, fmt = fmtNum }:
  { label: string; hist: number; sim: number; fmt?: (x: number) => string }) {
  const denom = Math.max(Math.abs(hist), 1e-6);
  const rel = (sim - hist) / denom;
  const goodish = Math.abs(rel) < 0.25;
  return (
    <tr className="border-t border-border/40">
      <td className="px-2 py-1 text-subtle uppercase tracking-widest text-[10px]">
        {label}
      </td>
      <td className="px-2 py-1 text-right text-ink">{fmt(hist)}</td>
      <td className="px-2 py-1 text-right text-ink">{fmt(sim)}</td>
      <td className={`px-2 py-1 text-right font-mono ${
        goodish ? 'text-bull' : Math.abs(rel) > 0.5 ? 'text-bear' : 'text-yellow-400'
      }`}>
        {(rel * 100).toFixed(0)}%
      </td>
    </tr>
  );
}

export default function HestonDiagnosticsPanel() {
  const { selectedSymbol, hestonDiagnostics, hestonDiagnosticsBusy,
          runHestonDiagnostics } = useStore();

  const diag = selectedSymbol ? hestonDiagnostics[selectedSymbol] : undefined;

  if (!selectedSymbol) {
    return (
      <div className="bg-panel border border-border rounded p-3 text-xs italic text-subtle">
        Click a symbol to compute Heston calibration diagnostics.
      </div>
    );
  }

  return (
    <div className="bg-panel border border-border rounded overflow-hidden">
      <div className="px-3 py-2 border-b border-border flex items-center justify-between">
        <div className="text-xs text-subtle uppercase tracking-widest font-bold">
          Heston Calibration · {selectedSymbol}
        </div>
        <div className="flex items-center gap-3 text-xs font-mono">
          {diag && (
            <span className="text-subtle">
              <span className={scoreColor(diag.scores.overall)}>
                {diag.scores.overall.toFixed(2)}
              </span>
              <span className="ml-1">overall</span>
              <span className="ml-3">{diag.n_paths_used} paths · {diag.n_history_bars} bars</span>
            </span>
          )}
          <button
            onClick={() => runHestonDiagnostics(selectedSymbol)}
            disabled={hestonDiagnosticsBusy}
            className="text-accent hover:underline disabled:opacity-50"
          >
            {hestonDiagnosticsBusy ? '…' : 'refresh'}
          </button>
        </div>
      </div>

      {!diag && hestonDiagnosticsBusy && (
        <div className="px-3 py-3 text-xs italic text-subtle">computing…</div>
      )}

      {diag && (
        <div className="p-3 space-y-3">

          {/* ── Calibrated params ─────────────────────────── */}
          <div className="grid grid-cols-5 gap-2 text-xs font-mono">
            <Stat label="v₀"    value={fmtNum(diag.params.v0,      4)} />
            <Stat label="θ"     value={fmtNum(diag.params.theta,   4)} />
            <Stat label="κ"     value={fmtNum(diag.params.kappa,   2)} />
            <Stat label="σᵥ"    value={fmtNum(diag.params.sigma_v, 2)} />
            <Stat label="ρ"     value={fmtNum(diag.params.rho,     2)} />
          </div>

          {/* ── Feller ────────────────────────────────────── */}
          <div className="border border-border rounded p-2 flex items-center justify-between">
            <div className="text-xs">
              <span className="text-subtle uppercase tracking-widest font-bold mr-3">
                Feller
              </span>
              <span className="font-mono">2κθ = {fmtNum(diag.feller.lhs, 3)}</span>
              <span className="text-subtle mx-2">vs</span>
              <span className="font-mono">σᵥ² = {fmtNum(diag.feller.rhs, 3)}</span>
            </div>
            {passBadge(diag.feller.ok)}
          </div>

          {/* ── Moment match ──────────────────────────────── */}
          <div className="border border-border rounded">
            <div className="px-2 py-1 border-b border-border bg-surface flex items-center justify-between">
              <div className="text-[10px] text-subtle uppercase tracking-widest font-bold">
                Daily-log-return Moments · 1 year sim
              </div>
              <div className={`text-xs font-mono ${scoreColor(diag.scores.moment_match)}`}>
                {diag.scores.moment_match.toFixed(2)}
              </div>
            </div>
            <table className="w-full text-xs font-mono">
              <thead className="text-subtle text-[10px] uppercase tracking-widest">
                <tr>
                  <th className="text-left  px-2 py-1"></th>
                  <th className="text-right px-2 py-1">Historical</th>
                  <th className="text-right px-2 py-1">Simulated</th>
                  <th className="text-right px-2 py-1">Δ</th>
                </tr>
              </thead>
              <tbody>
                <MomentRow label="mean ann" hist={diag.historical.mean_ann}
                                            sim={diag.simulated.mean_ann}
                                            fmt={(x) => fmtPct(x)} />
                <MomentRow label="std ann"  hist={diag.historical.std_ann}
                                            sim={diag.simulated.std_ann}
                                            fmt={(x) => fmtPct(x)} />
                <MomentRow label="skew"     hist={diag.historical.skew}
                                            sim={diag.simulated.skew} />
                <MomentRow label="kurt ex"  hist={diag.historical.kurt_excess}
                                            sim={diag.simulated.kurt_excess} />
              </tbody>
            </table>
          </div>

          {/* ── Realized vol stability ────────────────────── */}
          <div className="border border-border rounded">
            <div className="px-2 py-1 border-b border-border bg-surface flex items-center justify-between">
              <div className="text-[10px] text-subtle uppercase tracking-widest font-bold">
                Realized Vol · 21d rolling
              </div>
              <div className={`text-xs font-mono ${scoreColor(diag.scores.mean_reversion)}`}>
                {diag.scores.mean_reversion.toFixed(2)}
              </div>
            </div>
            <div className="grid grid-cols-4 gap-2 p-2 text-xs font-mono">
              <Stat label="RV mean"     value={fmtPct(diag.realized_vol.rv21_mean_vol)} />
              <Stat label="RV stdev"    value={fmtPct(diag.realized_vol.rv21_std_vol)} />
              <Stat label="√θ"          value={fmtPct(diag.realized_vol.sqrt_theta)} />
              <Stat label="emp σᵥ"     value={fmtNum(diag.realized_vol.empirical_vol_of_vol, 2)}
                                       hint={`vs default ${diag.params.sigma_v.toFixed(2)}`} />
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

function Stat({ label, value, hint }:
              { label: string; value: string; hint?: string }) {
  return (
    <div className="border border-border rounded p-1.5 bg-panel">
      <div className="text-[10px] text-subtle uppercase tracking-widest">{label}</div>
      <div className="text-ink">{value}</div>
      {hint && <div className="text-[9px] text-subtle mt-0.5">{hint}</div>}
    </div>
  );
}
