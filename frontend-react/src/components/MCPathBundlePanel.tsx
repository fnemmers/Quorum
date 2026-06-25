import { useEffect, useRef } from 'react';
import { useStore, type PathBundle } from '../store/useStore';

/*
 * MCPathBundlePanel  –  Density-colored Monte Carlo path bundle for one
 * stock. Renders the backend's density grid as a heatmap (time x price
 * cell counts), overlays 5/50/95 quantile lines, marks spot, and prints
 * ES_95 / expected return numerically.
 *
 * Heatmap convention:
 *   X axis = time (left = now, right = horizon)
 *   Y axis = price (bottom = price_min, top = price_max)
 *   Color  = path-count density (dark = sparse, bright = where the mass
 *            of paths actually goes)
 *
 * The dark band at the bottom is the visible ES_95 tail.
 */

/* Dark → bright color ramp tuned to the IBM workstation theme. */
function rampColor(t: number): [number, number, number] {
  // t in [0, 1]; output is RGB triples.
  // Hand-rolled 4-stop ramp: deep blue → teal → amber → white.
  const stops: Array<{ t: number; rgb: [number, number, number] }> = [
    { t: 0.00, rgb: [10, 20, 50]    },
    { t: 0.30, rgb: [10, 80, 130]   },
    { t: 0.65, rgb: [200, 170, 30]  },
    { t: 1.00, rgb: [255, 245, 220] },
  ];
  if (t <= stops[0].t) return stops[0].rgb;
  if (t >= stops[stops.length - 1].t) return stops[stops.length - 1].rgb;
  for (let i = 1; i < stops.length; i++) {
    if (t <= stops[i].t) {
      const a = stops[i - 1], b = stops[i];
      const f = (t - a.t) / (b.t - a.t);
      return [
        Math.round(a.rgb[0] + (b.rgb[0] - a.rgb[0]) * f),
        Math.round(a.rgb[1] + (b.rgb[1] - a.rgb[1]) * f),
        Math.round(a.rgb[2] + (b.rgb[2] - a.rgb[2]) * f),
      ];
    }
  }
  return stops[stops.length - 1].rgb;
}

function fmtPct(x: number) {
  if (x == null || Number.isNaN(x)) return '—';
  return `${x >= 0 ? '+' : ''}${(x * 100).toFixed(2)}%`;
}

interface DrawDims { width: number; height: number; padL: number; padR: number; padT: number; padB: number; plotW: number; plotH: number; }

function drawBundle(canvas: HTMLCanvasElement, b: PathBundle) {
  const dpr = window.devicePixelRatio || 1;
  const cssW = canvas.clientWidth;
  const cssH = canvas.clientHeight;
  canvas.width  = Math.floor(cssW * dpr);
  canvas.height = Math.floor(cssH * dpr);

  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, cssW, cssH);

  const d: DrawDims = {
    width: cssW, height: cssH,
    padL: 52, padR: 14, padT: 10, padB: 22,
    plotW: 0, plotH: 0,
  };
  d.plotW = d.width  - d.padL - d.padR;
  d.plotH = d.height - d.padT - d.padB;

  // Background plot area
  ctx.fillStyle = '#101418';
  ctx.fillRect(d.padL, d.padT, d.plotW, d.plotH);

  if (!b.density.length) return;

  const nBuckets = b.n_buckets;
  const nCols    = b.density[0]?.length ?? (b.n_steps + 1);

  // Find max density for normalization
  let maxD = 0;
  for (let row = 0; row < nBuckets; row++) {
    const r = b.density[row];
    for (let s = 0; s < nCols; s++) if (r[s] > maxD) maxD = r[s];
  }
  const norm = maxD > 0 ? 1 / maxD : 1;

  // Mapping helpers
  const xAt = (s: number) => d.padL + (s / (nCols - 1)) * d.plotW;
  const yAt = (price: number) => {
    // bucket 0 = lowest price = bottom of canvas
    const frac = (price - b.price_min) / (b.price_max - b.price_min);
    return d.padT + (1 - Math.max(0, Math.min(1, frac))) * d.plotH;
  };

  // Cell size
  const cellW = d.plotW / (nCols - 1);
  const cellH = d.plotH / nBuckets;

  // ── Density heatmap ────────────────────────────────
  // Compress brightness with sqrt so the medium-density cells are visible.
  for (let row = 0; row < nBuckets; row++) {
    const yTop = d.padT + (1 - (row + 1) / nBuckets) * d.plotH;
    const r = b.density[row];
    for (let s = 0; s < nCols; s++) {
      const v = r[s] * norm;
      if (v <= 0) continue;
      const t = Math.sqrt(v);
      const [rr, gg, bb] = rampColor(t);
      const xL = d.padL + (s - 0.5) * cellW;
      ctx.fillStyle = `rgb(${rr},${gg},${bb})`;
      ctx.fillRect(xL, yTop, cellW + 1, cellH + 1);
    }
  }

  // ── Spot reference line ─────────────────────────────
  ctx.strokeStyle = 'rgba(255,255,255,0.35)';
  ctx.setLineDash([4, 3]);
  ctx.lineWidth = 1;
  ctx.beginPath();
  const ySpot = yAt(b.spot);
  ctx.moveTo(d.padL, ySpot);
  ctx.lineTo(d.padL + d.plotW, ySpot);
  ctx.stroke();
  ctx.setLineDash([]);

  // ── Quantile overlays ───────────────────────────────
  function plotLine(arr: number[], color: string, dashed: boolean, lw: number) {
    ctx!.strokeStyle = color;
    ctx!.lineWidth = lw;
    if (dashed) ctx!.setLineDash([3, 3]); else ctx!.setLineDash([]);
    ctx!.beginPath();
    for (let s = 0; s < arr.length; s++) {
      const x = xAt(s);
      const y = yAt(arr[s]);
      if (s === 0) ctx!.moveTo(x, y); else ctx!.lineTo(x, y);
    }
    ctx!.stroke();
  }
  plotLine(b.p05, '#ff5050', true,  1.5);   // tail edge — bear red
  plotLine(b.p50, '#5da9ff', false, 2.0);   // median — accent blue
  plotLine(b.p95, '#5dff8a', true,  1.5);   // upside band — bull green
  ctx.setLineDash([]);

  // ── Axes labels ─────────────────────────────────────
  ctx.fillStyle = '#9aa3a8';
  ctx.font = '10px "SF Mono", Consolas, monospace';
  ctx.textBaseline = 'middle';

  // Price ticks: min, spot, max
  const ticks = [b.price_min, b.spot, b.price_max];
  const tickLabels = ['min', 'spot', 'max'];
  for (let i = 0; i < ticks.length; i++) {
    const y = yAt(ticks[i]);
    ctx.textAlign = 'right';
    ctx.fillText(`$${ticks[i].toFixed(2)}`, d.padL - 4, y);
    ctx.textAlign = 'left';
    ctx.fillText(tickLabels[i], d.padL + d.plotW + 2, y);
  }

  // Time ticks: 0 .. horizon
  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  const horizon = b.time_days[b.time_days.length - 1] ?? 0;
  const labels = [0, Math.round(horizon / 2), Math.round(horizon)];
  for (const td of labels) {
    const sIdx = Math.round((td / horizon) * (nCols - 1));
    const x = xAt(sIdx);
    ctx.fillText(`${td}d`, x, d.padT + d.plotH + 4);
  }

  // ── Legend (top-right corner) ───────────────────────
  ctx.textAlign = 'left';
  ctx.textBaseline = 'top';
  const legX = d.padL + 8;
  let legY = d.padT + 6;
  function legendItem(color: string, label: string, dashed: boolean) {
    ctx!.strokeStyle = color;
    ctx!.lineWidth = 2;
    if (dashed) ctx!.setLineDash([3, 3]); else ctx!.setLineDash([]);
    ctx!.beginPath();
    ctx!.moveTo(legX,     legY + 5);
    ctx!.lineTo(legX + 16, legY + 5);
    ctx!.stroke();
    ctx!.setLineDash([]);
    ctx!.fillStyle = '#cfd6db';
    ctx!.fillText(label, legX + 20, legY);
    legY += 13;
  }
  legendItem('#ff5050', 'p05 (tail)', true);
  legendItem('#5da9ff', 'p50',        false);
  legendItem('#5dff8a', 'p95',        true);
}

/* ── Component ───────────────────────────────────────── */

export default function MCPathBundlePanel() {
  const {
    selectedSymbol, pathBundles, pathBundleBusy,
    runPathBundle, selectSymbol,
  } = useStore();
  const canvasRef = useRef<HTMLCanvasElement>(null);

  const bundle = selectedSymbol ? pathBundles[selectedSymbol] : undefined;

  useEffect(() => {
    if (!bundle || !canvasRef.current) return;
    const c = canvasRef.current;
    drawBundle(c, bundle);

    const onResize = () => drawBundle(c, bundle);
    window.addEventListener('resize', onResize);
    return () => window.removeEventListener('resize', onResize);
  }, [bundle]);

  if (!selectedSymbol) {
    return (
      <div className="bg-panel border border-border rounded p-3 text-xs italic text-subtle">
        Click a symbol in Result or Tick Evaluation to render its
        Monte Carlo path bundle.
      </div>
    );
  }

  return (
    <div className="bg-panel border border-border rounded overflow-hidden">
      <div className="px-3 py-2 border-b border-border flex items-center justify-between">
        <div className="text-xs text-subtle uppercase tracking-widest font-bold">
          MC Paths · {selectedSymbol}
        </div>
        <div className="flex items-center gap-2 text-xs font-mono">
          {bundle && (
            <span className="text-subtle">
              {bundle.n_paths} paths · {bundle.horizon_days}d
            </span>
          )}
          <button
            onClick={() =>
              selectedSymbol && runPathBundle(selectedSymbol, bundle?.horizon_days ?? 21)
            }
            disabled={pathBundleBusy}
            className="text-accent hover:underline disabled:opacity-50"
          >
            {pathBundleBusy ? '…' : 'refresh'}
          </button>
          <button
            onClick={() => selectSymbol(null)}
            className="text-subtle hover:text-ink"
          >✕</button>
        </div>
      </div>

      <div className="relative">
        <canvas
          ref={canvasRef}
          className="block w-full"
          style={{ height: 300, background: '#0a0e12' }}
        />
        {pathBundleBusy && !bundle && (
          <div className="absolute inset-0 flex items-center justify-center text-xs text-subtle">
            simulating…
          </div>
        )}
      </div>

      {bundle && (
        <div className="px-3 py-2 border-t border-border text-xs font-mono grid grid-cols-3 gap-2">
          <div>
            <div className="text-[10px] text-subtle uppercase tracking-widest">spot</div>
            <div className="text-ink">${bundle.spot.toFixed(2)}</div>
          </div>
          <div>
            <div className="text-[10px] text-subtle uppercase tracking-widest">E[r] (horizon)</div>
            <div className={bundle.expected_return >= 0 ? 'text-bull' : 'text-bear'}>
              {fmtPct(bundle.expected_return)}
            </div>
          </div>
          <div>
            <div className="text-[10px] text-subtle uppercase tracking-widest">ES_95 (tail)</div>
            <div className="text-bear">{fmtPct(bundle.es_95)}</div>
          </div>
        </div>
      )}
    </div>
  );
}
