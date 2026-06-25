import { useEffect, useRef, useState } from 'react';
import { useStore, type VolSurface } from '../store/useStore';

/*
 * HestonSurfacePanel  –  Model-implied Black-Scholes volatility surface
 * over (strike, maturity), rendered as a shaded 3D wireframe using a
 * hand-rolled orthographic projection (no three.js).
 *
 * Axes (world frame):
 *   X = strike (left = OTM put, right = OTM call)
 *   Y = maturity (front = short, back = long)
 *   Z = implied vol (up)
 *
 * Rendering: rotate by yaw around Z + pitch around X, project to 2D,
 * draw quads back-to-front via painter's algorithm with a per-face
 * shading factor (face normal · light direction). Smile / skew /
 * term structure all jump out of the shape.
 */

function fmtPctVol(v: number) {
  if (v == null || Number.isNaN(v)) return '—';
  return `${(v * 100).toFixed(1)}%`;
}

/* Vol-themed color ramp: cool blue for low vol → magenta → hot orange. */
function ivColor(t: number): [number, number, number] {
  const stops: Array<{ t: number; rgb: [number, number, number] }> = [
    { t: 0.0, rgb: [25,  60, 120] },
    { t: 0.35, rgb: [60, 180, 200] },
    { t: 0.65, rgb: [220, 120, 180] },
    { t: 1.0,  rgb: [255, 180,  60] },
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

interface Vec3 { x: number; y: number; z: number; }

/* Apply yaw (rotate around z) then pitch (rotate around x). */
function rotate(v: Vec3, yaw: number, pitch: number): Vec3 {
  const cy = Math.cos(yaw),  sy = Math.sin(yaw);
  const cp = Math.cos(pitch), sp = Math.sin(pitch);
  const x1 =  v.x * cy - v.y * sy;
  const y1 =  v.x * sy + v.y * cy;
  const z1 =  v.z;
  const x2 = x1;
  const y2 = y1 * cp - z1 * sp;
  const z2 = y1 * sp + z1 * cp;
  return { x: x2, y: y2, z: z2 };
}

interface QuadIdx { i: number; j: number; depth: number; }

function drawSurface(canvas: HTMLCanvasElement, s: VolSurface,
                     yaw: number, pitch: number) {
  const dpr = window.devicePixelRatio || 1;
  const cssW = canvas.clientWidth;
  const cssH = canvas.clientHeight;
  canvas.width  = Math.floor(cssW * dpr);
  canvas.height = Math.floor(cssH * dpr);

  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, cssW, cssH);

  ctx.fillStyle = '#0a0e12';
  ctx.fillRect(0, 0, cssW, cssH);

  const nS = s.n_strikes;
  const nM = s.n_maturities;
  if (!nS || !nM) return;

  /* Build world-space grid normalised to [-0.5, 0.5] on each axis. */
  const ivLo = s.iv_min, ivHi = Math.max(s.iv_max, ivLo + 1e-6);
  const ivSpan = ivHi - ivLo;

  // moneyness axis [-0.5, 0.5]
  const mLo = s.moneyness[0];
  const mHi = s.moneyness[nS - 1];
  // maturity axis [-0.5, 0.5]
  const tLo = s.maturities_days[0];
  const tHi = s.maturities_days[nM - 1];

  function world(i: number, j: number): Vec3 {
    const x = (s.moneyness[i]      - mLo) / (mHi - mLo) - 0.5;
    const y = (s.maturities_days[j] - tLo) / (tHi - tLo) - 0.5;
    const ivVal = s.iv[i]?.[j] ?? 0;
    const z = (ivVal - ivLo) / ivSpan * 0.55 - 0.25;
    return { x, y, z };
  }

  /* Project rotated world coords to screen. */
  const cx = cssW * 0.5;
  const cy = cssH * 0.55;
  const scale = Math.min(cssW, cssH) * 0.62;
  function project(w: Vec3): { sx: number; sy: number; depth: number } {
    const r = rotate(w, yaw, pitch);
    return {
      sx: cx + r.x * scale,
      sy: cy - r.z * scale - r.y * scale * 0.55,  // y also slightly drops down for depth feel
      depth: -r.y,                                  // smaller depth = farther from camera
    };
  }

  /* Pre-project every grid point + cache color. */
  const proj: { sx: number; sy: number; depth: number }[][] = [];
  for (let i = 0; i < nS; i++) {
    proj.push([]);
    for (let j = 0; j < nM; j++) {
      proj[i].push(project(world(i, j)));
    }
  }

  /* Build quad list with centroid depth for painter's algorithm. */
  const quads: QuadIdx[] = [];
  for (let i = 0; i < nS - 1; i++) {
    for (let j = 0; j < nM - 1; j++) {
      const d = (proj[i][j].depth + proj[i + 1][j].depth +
                 proj[i + 1][j + 1].depth + proj[i][j + 1].depth) / 4;
      quads.push({ i, j, depth: d });
    }
  }
  quads.sort((a, b) => a.depth - b.depth);

  /* Light direction (world frame) - normalised. */
  const lightW = { x: -0.4, y: -0.5, z: 0.8 };
  const lightLen = Math.hypot(lightW.x, lightW.y, lightW.z);
  const light = { x: lightW.x / lightLen, y: lightW.y / lightLen, z: lightW.z / lightLen };

  /* Draw the back-grid floor/walls so the surface has reference planes. */
  function drawGridLine(a: Vec3, b: Vec3, color: string, dashed = false) {
    const pa = project(a), pb = project(b);
    ctx!.strokeStyle = color;
    ctx!.lineWidth = 1;
    if (dashed) ctx!.setLineDash([3, 3]); else ctx!.setLineDash([]);
    ctx!.beginPath();
    ctx!.moveTo(pa.sx, pa.sy);
    ctx!.lineTo(pb.sx, pb.sy);
    ctx!.stroke();
    ctx!.setLineDash([]);
  }

  // Floor outline at z = base
  const z0 = -0.25;
  drawGridLine({ x: -0.5, y: -0.5, z: z0 }, { x:  0.5, y: -0.5, z: z0 }, '#2a3038');
  drawGridLine({ x:  0.5, y: -0.5, z: z0 }, { x:  0.5, y:  0.5, z: z0 }, '#2a3038');
  drawGridLine({ x:  0.5, y:  0.5, z: z0 }, { x: -0.5, y:  0.5, z: z0 }, '#2a3038');
  drawGridLine({ x: -0.5, y:  0.5, z: z0 }, { x: -0.5, y: -0.5, z: z0 }, '#2a3038');
  // back wall verticals
  drawGridLine({ x: -0.5, y:  0.5, z: z0 }, { x: -0.5, y:  0.5, z:  0.35 }, '#2a3038');
  drawGridLine({ x:  0.5, y:  0.5, z: z0 }, { x:  0.5, y:  0.5, z:  0.35 }, '#2a3038');

  /* Draw quads back-to-front. */
  for (const q of quads) {
    const { i, j } = q;
    const p00 = proj[i][j];
    const p10 = proj[i + 1][j];
    const p11 = proj[i + 1][j + 1];
    const p01 = proj[i][j + 1];

    // World vectors for normal calculation
    const w00 = world(i,     j);
    const w10 = world(i + 1, j);
    const w01 = world(i,     j + 1);
    const ax = w10.x - w00.x, ay = w10.y - w00.y, az = w10.z - w00.z;
    const bx = w01.x - w00.x, by = w01.y - w00.y, bz = w01.z - w00.z;
    let nx = ay * bz - az * by;
    let ny = az * bx - ax * bz;
    let nz = ax * by - ay * bx;
    const nlen = Math.hypot(nx, ny, nz) || 1;
    nx /= nlen; ny /= nlen; nz /= nlen;
    // ensure normal points "up"
    if (nz < 0) { nx = -nx; ny = -ny; nz = -nz; }

    const dot = nx * light.x + ny * light.y + nz * light.z;
    const shade = 0.45 + 0.55 * Math.max(0, dot);   // ambient 0.45 + diffuse

    // Avg IV for color
    const ivAvg = (s.iv[i][j] + s.iv[i + 1][j] + s.iv[i + 1][j + 1] + s.iv[i][j + 1]) / 4;
    const t = (ivAvg - ivLo) / (ivSpan || 1);
    const [r, g, b] = ivColor(Math.max(0, Math.min(1, t)));
    const rr = Math.round(r * shade);
    const gg = Math.round(g * shade);
    const bb = Math.round(b * shade);

    ctx.fillStyle = `rgb(${rr},${gg},${bb})`;
    ctx.strokeStyle = `rgba(${Math.min(255, rr + 40)},${Math.min(255, gg + 40)},${Math.min(255, bb + 40)},0.65)`;
    ctx.lineWidth = 0.7;
    ctx.beginPath();
    ctx.moveTo(p00.sx, p00.sy);
    ctx.lineTo(p10.sx, p10.sy);
    ctx.lineTo(p11.sx, p11.sy);
    ctx.lineTo(p01.sx, p01.sy);
    ctx.closePath();
    ctx.fill();
    ctx.stroke();
  }

  /* ATM reference line (moneyness = 1.0) across maturities. */
  if (mLo <= 1 && mHi >= 1) {
    ctx.strokeStyle = 'rgba(255,255,255,0.45)';
    ctx.setLineDash([4, 3]);
    ctx.lineWidth = 1.2;
    ctx.beginPath();
    for (let j = 0; j < nM; j++) {
      const tAtm = (1.0 - mLo) / (mHi - mLo);
      const yj   = (s.maturities_days[j] - tLo) / (tHi - tLo) - 0.5;
      // interpolate IV at ATM along i
      const iLow = Math.max(0, Math.floor(tAtm * (nS - 1)));
      const iHi  = Math.min(nS - 1, iLow + 1);
      const f    = (tAtm * (nS - 1)) - iLow;
      const ivJ  = s.iv[iLow][j] * (1 - f) + s.iv[iHi][j] * f;
      const z    = (ivJ - ivLo) / ivSpan * 0.55 - 0.25;
      const p    = project({ x: 0.0, y: yj, z });
      if (j === 0) ctx.moveTo(p.sx, p.sy); else ctx.lineTo(p.sx, p.sy);
    }
    ctx.stroke();
    ctx.setLineDash([]);
  }

  /* Axis labels. */
  ctx.fillStyle = '#cfd6db';
  ctx.font = '11px "SF Mono", Consolas, monospace';

  function label(w: Vec3, text: string, align: CanvasTextAlign, baseline: CanvasTextBaseline,
                 color = '#cfd6db') {
    const p = project(w);
    ctx!.fillStyle = color;
    ctx!.textAlign = align;
    ctx!.textBaseline = baseline;
    ctx!.fillText(text, p.sx, p.sy);
  }
  // Strike (moneyness) labels along the front edge.
  label({ x: -0.5, y: -0.55, z: z0 - 0.05 }, `${mLo.toFixed(2)}×`, 'right', 'top');
  label({ x:  0.0, y: -0.55, z: z0 - 0.05 }, 'spot',                'center', 'top', '#cfd6db');
  label({ x:  0.5, y: -0.55, z: z0 - 0.05 }, `${mHi.toFixed(2)}×`, 'left',  'top');
  // Maturity labels along the right side at the back.
  label({ x:  0.55, y: -0.5, z: z0 - 0.05 }, `${Math.round(tLo)}d`, 'left',  'top');
  label({ x:  0.55, y:  0.5, z: z0 - 0.05 }, `${Math.round(tHi)}d`, 'left',  'top');
  // IV labels on the back wall.
  label({ x: -0.55, y:  0.5, z:  0.30 }, fmtPctVol(ivHi), 'right', 'middle');
  label({ x: -0.55, y:  0.5, z: -0.20 }, fmtPctVol(ivLo), 'right', 'middle');

  // Title
  ctx.fillStyle = '#9aa3a8';
  ctx.font = '10px "SF Mono", Consolas, monospace';
  ctx.textAlign = 'left';
  ctx.textBaseline = 'top';
  ctx.fillText(`spot $${s.spot.toFixed(2)} · r=${(s.r * 100).toFixed(1)}%`, 8, 8);
}

/* ── Component ───────────────────────────────────────── */

export default function HestonSurfacePanel() {
  const {
    selectedSymbol, volSurfaces, volSurfaceBusy,
    runVolSurface, selectSymbol,
  } = useStore();
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [yaw,   setYaw]   = useState(-Math.PI / 6);    // -30°
  const [pitch, setPitch] = useState( Math.PI / 7);    // ~25°
  const dragRef = useRef<{ x: number; y: number; yaw: number; pitch: number } | null>(null);

  const surface = selectedSymbol ? volSurfaces[selectedSymbol] : undefined;

  useEffect(() => {
    if (!surface || !canvasRef.current) return;
    const c = canvasRef.current;
    drawSurface(c, surface, yaw, pitch);
    const onResize = () => drawSurface(c, surface, yaw, pitch);
    window.addEventListener('resize', onResize);
    return () => window.removeEventListener('resize', onResize);
  }, [surface, yaw, pitch]);

  function onMouseDown(e: React.MouseEvent) {
    dragRef.current = { x: e.clientX, y: e.clientY, yaw, pitch };
  }
  function onMouseMove(e: React.MouseEvent) {
    if (!dragRef.current) return;
    const dx = e.clientX - dragRef.current.x;
    const dy = e.clientY - dragRef.current.y;
    setYaw(dragRef.current.yaw + dx * 0.008);
    let p = dragRef.current.pitch + dy * 0.005;
    if (p < -Math.PI / 2 + 0.1) p = -Math.PI / 2 + 0.1;
    if (p >  Math.PI / 2 - 0.1) p =  Math.PI / 2 - 0.1;
    setPitch(p);
  }
  function onMouseUp() { dragRef.current = null; }

  if (!selectedSymbol) {
    return (
      <div className="bg-panel border border-border rounded p-3 text-xs italic text-subtle">
        Click a symbol to render its Heston-implied vol surface.
      </div>
    );
  }

  return (
    <div className="bg-panel border border-border rounded overflow-hidden">
      <div className="px-3 py-2 border-b border-border flex items-center justify-between">
        <div className="text-xs text-subtle uppercase tracking-widest font-bold">
          Vol Surface · {selectedSymbol}
        </div>
        <div className="flex items-center gap-2 text-xs font-mono">
          {surface && (
            <span className="text-subtle">
              {surface.n_strikes}×{surface.n_maturities}
              {surface.n_failed > 0 && (
                <span className="text-yellow-400 ml-2">{surface.n_failed} solver misses</span>
              )}
            </span>
          )}
          <button
            onClick={() => selectedSymbol && runVolSurface(selectedSymbol)}
            disabled={volSurfaceBusy}
            className="text-accent hover:underline disabled:opacity-50"
          >
            {volSurfaceBusy ? '…' : 'refresh'}
          </button>
          <button
            onClick={() => selectSymbol(null)}
            className="text-subtle hover:text-ink"
          >✕</button>
        </div>
      </div>

      <div className="relative" onMouseDown={onMouseDown} onMouseMove={onMouseMove} onMouseUp={onMouseUp} onMouseLeave={onMouseUp}>
        <canvas
          ref={canvasRef}
          className="block w-full cursor-grab active:cursor-grabbing"
          style={{ height: 320, background: '#0a0e12' }}
        />
        {volSurfaceBusy && !surface && (
          <div className="absolute inset-0 flex items-center justify-center text-xs text-subtle">
            pricing options…
          </div>
        )}
        <div className="absolute bottom-1 right-2 text-[10px] text-subtle pointer-events-none">
          drag to rotate
        </div>
      </div>

      {surface && (
        <div className="px-3 py-2 border-t border-border text-xs font-mono grid grid-cols-3 gap-2">
          <div>
            <div className="text-[10px] text-subtle uppercase tracking-widest">vol range</div>
            <div className="text-ink">{fmtPctVol(surface.iv_min)} – {fmtPctVol(surface.iv_max)}</div>
          </div>
          <div>
            <div className="text-[10px] text-subtle uppercase tracking-widest">moneyness</div>
            <div className="text-ink">
              {surface.moneyness[0].toFixed(2)} – {surface.moneyness[surface.n_strikes - 1].toFixed(2)}
            </div>
          </div>
          <div>
            <div className="text-[10px] text-subtle uppercase tracking-widest">maturities</div>
            <div className="text-ink">
              {Math.round(surface.maturities_days[0])}d –
              {' '}{Math.round(surface.maturities_days[surface.n_maturities - 1])}d
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
