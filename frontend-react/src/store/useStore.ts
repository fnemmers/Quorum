import { create } from 'zustand';

export interface OHLCBar {
  t: number; // unix ms
  o: number;
  h: number;
  l: number;
  c: number;
  v: number;
}

export interface Quote {
  symbol: string;
  price: number;
  bid: number;
  ask: number;
  volume: number;
  ts: number;
}

export interface Holding {
  symbol: string;
  shares: number;
  avg_price: number;
  current: number;
}

export interface Alert {
  id: number;
  symbol: string;
  condition: 'above' | 'below';
  price: number;
}

export interface BlotterEntry {
  id: number;
  symbol: string;
  side: 'buy' | 'sell';
  quantity: number;
  price: number;
  ts: number;
  strategy: string;
}

/* ── Research types ───────────────────────────────────────────── */

export interface BotRun {
  id: number;
  label: string;
  n_bots_target: number;
  n_bots_actual: number;
  hold_days: number;
  started_at: number;
  finished_at: number;
}

export interface AggPick {
  symbol: string;
  count: number;
}

export interface AggregateResult {
  run_id: number;
  n_picks_total: number;
  top: AggPick[];
}

export interface PaperPosition {
  symbol:  string;
  shares:  number;
  avgCost: number;
}

export interface PaperTrade {
  id:     number;
  ts:     number;
  symbol: string;
  side:   'buy' | 'sell';
  shares: number;
  price:  number;
}

export const PAPER_STARTING_CASH = 10_000_000;

export interface BacktestResult {
  run_id: number;
  start_date: string;
  end_date: string;
  hold_days: number;
  n_used: number;
  n_skipped: number;
  port_return: number;
  bench_return: number;
  alpha: number;
  sharpe: number;
  max_dd: number;
  hit_rate: number;
}

export interface RankedEntry {
  rank: number;
  symbol: string;
  blended_score: number;
  z_bot: number;
  z_heston: number;
  bot_count: number;
  bot_disagreement: number;
  expected_return: number;
  forward_vol: number;
  es_95: number;
  prob_loss: number;
}

export interface RankingResult {
  run_id: number;
  sigma_blend: number;
  w_bot: number;
  w_heston: number;
  ranked: RankedEntry[];
}

export interface RebalanceEvent {
  event_id: number;
  symbol: string;
  decision: 'auto' | 'notify' | 'escalate' | 'hold';
  suggested_action: 'sell' | 'trim' | 'flip' | 'none';
  old_blend: number;
  new_blend: number;
  exit_threshold: number;
  obscurity: number;
  clarity: number;
  primary_driver: string;
  score_gap_clarity: number;
  llm_agreement: number;
  heston_breach: number;
  horizon_maturity: number;
  days_held: number;
  intended_hold_days: number;
  debrief: string;
}

export interface RebalanceCheckResult {
  run_id: number;
  sigma_blend: number;
  exit_threshold: number;
  events: RebalanceEvent[];
}

export interface PathBundle {
  symbol: string;
  horizon_days: number;
  n_paths: number;
  n_steps: number;
  n_buckets: number;
  spot: number;
  price_min: number;
  price_max: number;
  expected_return: number;
  es_95: number;
  time_days: number[];      // length n_steps + 1
  density: number[][];      // [n_buckets][n_steps+1] - row 0 is the LOWEST price bucket
  p05: number[];            // length n_steps + 1
  p50: number[];
  p95: number[];
}

export interface HestonDiagnostics {
  symbol: string;
  n_history_bars: number;
  hist_window_years: number;
  n_paths_used: number;
  params: {
    v0: number; theta: number; kappa: number;
    sigma_v: number; rho: number;
  };
  feller: { lhs: number; rhs: number; ok: boolean };
  historical: {
    mean_ann: number; std_ann: number;
    skew: number; kurt_excess: number;
  };
  simulated: {
    mean_ann: number; std_ann: number;
    skew: number; kurt_excess: number;
  };
  realized_vol: {
    rv21_mean_vol: number; rv21_std_vol: number;
    sqrt_theta: number; empirical_vol_of_vol: number;
  };
  scores: {
    moment_match: number; mean_reversion: number; overall: number;
  };
}

export interface VolSurface {
  symbol: string;
  spot: number;
  n_strikes: number;
  n_maturities: number;
  iv_min: number;
  iv_max: number;
  n_failed: number;
  r: number;
  strikes: number[];           // length n_strikes (absolute prices)
  moneyness: number[];         // length n_strikes (strike / spot)
  maturities_days: number[];   // length n_maturities
  maturities_years: number[];  // length n_maturities
  iv: number[][];              // [n_strikes][n_maturities]
}

interface State {
  bridgeConnected: boolean;
  backendConnected: boolean;

  symbol: string;
  bars: OHLCBar[];
  quotes: Record<string, Quote>;
  holdings: Holding[];
  alerts: Alert[];
  blotter: BlotterEntry[];

  /* risk panel (UI-only state) */
  maxPositionPct: number;
  maxDrawdownPct: number;
  killSwitch: boolean;

  /* research */
  botRuns: BotRun[];
  aggregates: Record<number, AggregateResult>;  // keyed by run_id
  backtests: Record<number, BacktestResult>;    // keyed by run_id (latest wins)
  rankings: Record<number, RankingResult>;      // keyed by run_id (blended)
  rebalanceCheck: RebalanceCheckResult | null;
  /* Monte Carlo path bundles per symbol (latest run wins). */
  pathBundles: Record<string, PathBundle>;
  pathBundleBusy: boolean;
  /* Heston-implied vol surfaces per symbol. */
  volSurfaces: Record<string, VolSurface>;
  volSurfaceBusy: boolean;
  /* Heston calibration diagnostics per symbol. */
  hestonDiagnostics: Record<string, HestonDiagnostics>;
  hestonDiagnosticsBusy: boolean;
  /* Currently-selected symbol for the MC path bundle viewer. */
  selectedSymbol: string | null;
  researchBusy: boolean;
  researchError: string | null;
  /* Local-only: when each holding was opened, ms epoch. Persists in
   * localStorage so days_held survives page reloads. */
  heldSince: Record<string, number>;

  /* paper trail (simulated portfolio, persisted to localStorage) */
  paperCash:      number;
  paperPositions: PaperPosition[];
  paperTrades:    PaperTrade[];

  _ws: WebSocket | null;
  _nextBlotterId: number;
  _nextPaperTradeId: number;

  /* actions */
  setSymbol: (s: string) => void;
  send: (cmd: object) => void;
  fetchHistory: (sym: string) => void;
  fetchPortfolio: () => void;
  fetchAlerts: () => void;
  addHolding: (symbol: string, shares: number, price: number) => void;
  removeHolding: (symbol: string) => void;
  addAlert: (symbol: string, condition: 'above' | 'below', price: number) => void;
  removeAlert: (id: number) => void;
  addBlotterEntry: (e: Omit<BlotterEntry, 'id'>) => void;
  toggleKillSwitch: () => void;
  setMaxPosition: (v: number) => void;
  setMaxDrawdown: (v: number) => void;

  /* research actions */
  fetchBotRuns: (limit?: number) => void;
  runAggregate: (runId: number, k: number) => void;
  runBacktest: (runId: number, startDate: string, holdDays: number, k: number) => void;
  runRankingBlend: (runId: number, k: number, horizonDays: number,
                    disagreement?: Record<string, number>) => void;
  runRebalanceCheck: (runId: number, k: number, horizonDays: number,
                      disagreement?: Record<string, number>,
                      intendedHoldDays?: number) => void;
  selectSymbol: (sym: string | null, horizonDays?: number) => void;
  runPathBundle: (sym: string, horizonDays: number, nPaths?: number,
                  nBuckets?: number) => void;
  runVolSurface: (sym: string, opts?: {
    nStrikes?: number; nMaturities?: number;
    moneynessLo?: number; moneynessHi?: number;
    maxMatDays?: number; r?: number;
  }) => void;
  runHestonDiagnostics: (sym: string, nPaths?: number) => void;
  clearResearchError: () => void;

  /* paper trail actions — return null on success, error string on failure */
  paperBuy:   (symbol: string, shares: number, price: number) => string | null;
  paperSell:  (symbol: string, shares: number, price: number) => string | null;
  paperReset: () => void;
}

const WS_URL = 'ws://localhost:3001';

function connect(set: (partial: Partial<State> | ((s: State) => Partial<State>)) => void,
                 get: () => State) {
  const ws = new WebSocket(WS_URL);

  ws.onopen = () => {
    set({ _ws: ws });
    get().fetchHistory(get().symbol);
    get().fetchPortfolio();
    get().fetchAlerts();
    get().fetchBotRuns();
    for (const p of get().paperPositions) {
      get().send({ cmd: 'subscribe', symbol: p.symbol });
    }
  };

  ws.onclose = () => {
    set({ _ws: null, bridgeConnected: false });
    setTimeout(() => connect(set, get), 3000);
  };

  ws.onerror = () => ws.close();

  ws.onmessage = (ev) => {
    let msg: Record<string, unknown>;
    try { msg = JSON.parse(ev.data); } catch { return; }

    const type = msg.type as string;

    if (type === 'bridge_status') {
      set({ bridgeConnected: msg.connected as boolean });
      return;
    }

    if (type === 'quote') {
      const q = msg as unknown as Quote;
      set((s) => ({ quotes: { ...s.quotes, [q.symbol]: q } }));
      return;
    }

    if (type === 'history') {
      if (msg.symbol === get().symbol)
        set({ bars: (msg.bars as OHLCBar[]) ?? [] });
      return;
    }

    if (type === 'portfolio') {
      set({ holdings: (msg.holdings as Holding[]) ?? [] });
      return;
    }

    if (type === 'alert_list') {
      set({ alerts: (msg.alerts as Alert[]) ?? [] });
      return;
    }

    if (type === 'alert') {
      const firedId = msg.id as number;
      set((s) => ({ alerts: s.alerts.filter((a) => a.id !== firedId) }));
      return;
    }

    if (type === 'alert_added') {
      const a = msg as unknown as Alert;
      set((s) => ({ alerts: [...s.alerts, a] }));
      return;
    }

    if (type === 'bot_runs_list') {
      set({ botRuns: (msg.runs as BotRun[]) ?? [] });
      return;
    }

    if (type === 'aggregate_result') {
      const r: AggregateResult = {
        run_id:        msg.run_id as number,
        n_picks_total: msg.n_picks_total as number,
        top:           (msg.top as AggPick[]) ?? [],
      };
      set((s) => ({
        aggregates:   { ...s.aggregates, [r.run_id]: r },
        researchBusy: false,
      }));
      return;
    }

    if (type === 'ranking_blended') {
      const r: RankingResult = {
        run_id:      msg.run_id      as number,
        sigma_blend: msg.sigma_blend as number,
        w_bot:       msg.w_bot       as number,
        w_heston:    msg.w_heston    as number,
        ranked:      (msg.ranked as RankedEntry[]) ?? [],
      };
      set((s) => ({
        rankings:     { ...s.rankings, [r.run_id]: r },
        researchBusy: false,
      }));
      return;
    }

    if (type === 'rebalance_check') {
      const r: RebalanceCheckResult = {
        run_id:         msg.run_id         as number,
        sigma_blend:    msg.sigma_blend    as number,
        exit_threshold: msg.exit_threshold as number,
        events:         (msg.events as RebalanceEvent[]) ?? [],
      };
      set({ rebalanceCheck: r, researchBusy: false });
      return;
    }

    if (type === 'heston_diagnostics') {
      const d = msg as unknown as HestonDiagnostics;
      set((st) => ({
        hestonDiagnostics: { ...st.hestonDiagnostics, [d.symbol]: d },
        hestonDiagnosticsBusy: false,
      }));
      return;
    }

    if (type === 'heston_surface') {
      const s: VolSurface = {
        symbol:           msg.symbol           as string,
        spot:             msg.spot             as number,
        n_strikes:        msg.n_strikes        as number,
        n_maturities:     msg.n_maturities     as number,
        iv_min:           msg.iv_min           as number,
        iv_max:           msg.iv_max           as number,
        n_failed:         msg.n_failed         as number,
        r:                msg.r                as number,
        strikes:          (msg.strikes          as number[]) ?? [],
        moneyness:        (msg.moneyness        as number[]) ?? [],
        maturities_days:  (msg.maturities_days  as number[]) ?? [],
        maturities_years: (msg.maturities_years as number[]) ?? [],
        iv:               (msg.iv               as number[][]) ?? [],
      };
      set((st) => ({
        volSurfaces:    { ...st.volSurfaces, [s.symbol]: s },
        volSurfaceBusy: false,
      }));
      return;
    }

    if (type === 'heston_path_bundle') {
      const b: PathBundle = {
        symbol:          msg.symbol          as string,
        horizon_days:    msg.horizon_days    as number,
        n_paths:         msg.n_paths         as number,
        n_steps:         msg.n_steps         as number,
        n_buckets:       msg.n_buckets       as number,
        spot:            msg.spot            as number,
        price_min:       msg.price_min       as number,
        price_max:       msg.price_max       as number,
        expected_return: msg.expected_return as number,
        es_95:           msg.es_95           as number,
        time_days:       (msg.time_days as number[]) ?? [],
        density:         (msg.density   as number[][]) ?? [],
        p05:             (msg.p05       as number[]) ?? [],
        p50:             (msg.p50       as number[]) ?? [],
        p95:             (msg.p95       as number[]) ?? [],
      };
      set((s) => ({
        pathBundles:    { ...s.pathBundles, [b.symbol]: b },
        pathBundleBusy: false,
      }));
      return;
    }

    if (type === 'backtest_result') {
      const r: BacktestResult = {
        run_id:       msg.run_id as number,
        start_date:   msg.start_date as string,
        end_date:     msg.end_date as string,
        hold_days:    msg.hold_days as number,
        n_used:       msg.n_used as number,
        n_skipped:    msg.n_skipped as number,
        port_return:  msg.port_return as number,
        bench_return: msg.bench_return as number,
        alpha:        msg.alpha as number,
        sharpe:       msg.sharpe as number,
        max_dd:       msg.max_dd as number,
        hit_rate:     msg.hit_rate as number,
      };
      set((s) => ({
        backtests:    { ...s.backtests, [r.run_id]: r },
        researchBusy: false,
      }));
      return;
    }

    if (type === 'error') {
      set({
        researchError: (msg.message as string) ?? 'unknown error',
        researchBusy:  false,
      });
      return;
    }
  };
}

/* ── Paper trail localStorage persistence ─────────────────────── */

const PAPER_STORAGE_KEY = 'stock-app:paper-v1';

interface PaperSnapshot {
  cash:        number;
  positions:   PaperPosition[];
  trades:      PaperTrade[];
  nextTradeId: number;
}

function loadPaper(): PaperSnapshot {
  try {
    const raw = localStorage.getItem(PAPER_STORAGE_KEY);
    if (!raw) throw new Error('empty');
    const p = JSON.parse(raw) as Partial<PaperSnapshot>;
    return {
      cash:        typeof p.cash === 'number' ? p.cash : PAPER_STARTING_CASH,
      positions:   Array.isArray(p.positions) ? p.positions : [],
      trades:      Array.isArray(p.trades)    ? p.trades    : [],
      nextTradeId: typeof p.nextTradeId === 'number' ? p.nextTradeId : 1,
    };
  } catch {
    return { cash: PAPER_STARTING_CASH, positions: [], trades: [], nextTradeId: 1 };
  }
}

function savePaper(s: PaperSnapshot) {
  try { localStorage.setItem(PAPER_STORAGE_KEY, JSON.stringify(s)); }
  catch { /* quota / disabled — best effort */ }
}

const HELD_SINCE_KEY = 'stock-app:held-since-v1';

function loadHeldSince(): Record<string, number> {
  try {
    const raw = localStorage.getItem(HELD_SINCE_KEY);
    if (!raw) return {};
    const parsed = JSON.parse(raw);
    return typeof parsed === 'object' && parsed ? parsed : {};
  } catch { return {}; }
}

function saveHeldSince(m: Record<string, number>) {
  try { localStorage.setItem(HELD_SINCE_KEY, JSON.stringify(m)); }
  catch { /* best effort */ }
}

export const useStore = create<State>((set, get) => {
  setTimeout(() => connect(set, get), 0);

  const paper = loadPaper();

  return {
    bridgeConnected: false,
    backendConnected: false,
    symbol: 'AAPL',
    bars: [],
    quotes: {},
    holdings: [],
    alerts: [],
    blotter: [],
    maxPositionPct: 10,
    maxDrawdownPct: 5,
    killSwitch: false,

    botRuns: [],
    aggregates: {},
    backtests: {},
    rankings: {},
    rebalanceCheck: null,
    pathBundles: {},
    pathBundleBusy: false,
    volSurfaces: {},
    volSurfaceBusy: false,
    hestonDiagnostics: {},
    hestonDiagnosticsBusy: false,
    selectedSymbol: null,
    researchBusy: false,
    researchError: null,
    heldSince: loadHeldSince(),

    paperCash:      paper.cash,
    paperPositions: paper.positions,
    paperTrades:    paper.trades,

    _ws: null,
    _nextBlotterId: 1,
    _nextPaperTradeId: paper.nextTradeId,

    setSymbol: (s) => {
      set({ symbol: s, bars: [] });
      get().fetchHistory(s);
      get().send({ cmd: 'subscribe', symbol: s });
    },

    send: (cmd) => {
      const ws = get()._ws;
      if (ws && ws.readyState === WebSocket.OPEN)
        ws.send(JSON.stringify(cmd));
    },

    fetchHistory: (sym) => {
      const today = new Date().toISOString().slice(0, 10);
      get().send({
        cmd: 'history', symbol: sym,
        from: '2024-01-01', to: today,
        timespan: 'day', multiplier: 1,
      });
    },

    fetchPortfolio: () => get().send({ cmd: 'portfolio_get' }),
    fetchAlerts:    () => get().send({ cmd: 'alert_list' }),

    addHolding: (symbol, shares, price) => {
      get().send({ cmd: 'portfolio_add', symbol, shares, price });
      get().send({ cmd: 'subscribe', symbol });
      set((s) => {
        const next = { ...s.heldSince, [symbol]: s.heldSince[symbol] ?? Date.now() };
        saveHeldSince(next);
        return { heldSince: next };
      });
      setTimeout(() => get().fetchPortfolio(), 200);
    },

    removeHolding: (symbol) => {
      get().send({ cmd: 'portfolio_remove', symbol });
      set((s) => {
        const next = { ...s.heldSince };
        delete next[symbol];
        saveHeldSince(next);
        return { heldSince: next };
      });
      setTimeout(() => get().fetchPortfolio(), 200);
    },

    addAlert: (symbol, condition, price) => {
      get().send({ cmd: 'alert_add', symbol, condition, price });
    },

    removeAlert: (id) => {
      get().send({ cmd: 'alert_remove', id });
      set((s) => ({ alerts: s.alerts.filter((a) => a.id !== id) }));
    },

    addBlotterEntry: (e) => {
      set((s) => ({
        blotter: [{ ...e, id: s._nextBlotterId }, ...s.blotter],
        _nextBlotterId: s._nextBlotterId + 1,
      }));
    },

    toggleKillSwitch: () => set((s) => ({ killSwitch: !s.killSwitch })),
    setMaxPosition:   (v) => set({ maxPositionPct: v }),
    setMaxDrawdown:   (v) => set({ maxDrawdownPct: v }),

    fetchBotRuns: (limit = 50) => {
      get().send({ cmd: 'bot_runs_list', limit });
    },

    runAggregate: (runId, k) => {
      set({ researchBusy: true, researchError: null });
      get().send({ cmd: 'aggregate_run', run_id: runId, k });
    },

    runBacktest: (runId, startDate, holdDays, k) => {
      set({ researchBusy: true, researchError: null });
      get().send({
        cmd: 'backtest_run',
        run_id: runId,
        start_date: startDate,
        hold_days: holdDays,
        k,
      });
    },

    runRankingBlend: (runId, k, horizonDays, disagreement) => {
      set({ researchBusy: true, researchError: null });
      get().send({
        cmd:          'ranking_blend',
        run_id:       runId,
        k,
        horizon_days: horizonDays,
        disagreement: disagreement ?? {},
      });
    },

    selectSymbol: (sym, horizonDays = 21) => {
      set({ selectedSymbol: sym });
      if (sym) {
        get().runPathBundle(sym, horizonDays);
        get().runVolSurface(sym);
        get().runHestonDiagnostics(sym);
      }
    },

    runPathBundle: (sym, horizonDays, nPaths = 5000, nBuckets = 100) => {
      set({ pathBundleBusy: true });
      get().send({
        cmd:          'heston_path_bundle',
        symbol:       sym,
        horizon_days: horizonDays,
        n_paths:      nPaths,
        n_buckets:    nBuckets,
      });
    },

    runVolSurface: (sym, opts = {}) => {
      set({ volSurfaceBusy: true });
      get().send({
        cmd:           'heston_surface',
        symbol:        sym,
        n_strikes:     opts.nStrikes     ?? 21,
        n_maturities:  opts.nMaturities  ?? 12,
        moneyness_lo:  opts.moneynessLo  ?? 0.7,
        moneyness_hi:  opts.moneynessHi  ?? 1.3,
        max_mat_days:  opts.maxMatDays   ?? 180,
        r:             opts.r            ?? 0.04,
      });
    },

    runHestonDiagnostics: (sym, nPaths = 4000) => {
      set({ hestonDiagnosticsBusy: true });
      get().send({
        cmd:    'heston_diagnostics',
        symbol: sym,
        n_paths: nPaths,
      });
    },

    runRebalanceCheck: (runId, k, horizonDays, disagreement, intendedHoldDays = 21) => {
      const s = get();
      const now = Date.now();
      const holdings = s.holdings.map((h) => {
        const since = s.heldSince[h.symbol] ?? now;
        const days_held = Math.max(0, Math.floor((now - since) / 86400000));
        const old_blend = s.rankings[runId]?.ranked
          .find((r) => r.symbol === h.symbol)?.blended_score ?? 0;
        return {
          symbol:             h.symbol,
          old_blend,
          days_held,
          intended_hold_days: intendedHoldDays,
        };
      });
      set({ researchBusy: true, researchError: null });
      get().send({
        cmd:          'rebalance_check',
        run_id:       runId,
        k,
        horizon_days: horizonDays,
        disagreement: disagreement ?? {},
        holdings,
        exit_rank_band: 2 * k,
      });
    },

    clearResearchError: () => set({ researchError: null }),

    paperBuy: (symbol, shares, price) => {
      symbol = symbol.trim().toUpperCase();
      if (!symbol)        return 'symbol required';
      if (shares <= 0)    return 'shares must be > 0';
      if (price  <= 0)    return 'price must be > 0';
      const cost = shares * price;
      const s = get();
      if (cost > s.paperCash) {
        return `insufficient cash: need $${cost.toFixed(2)}, have $${s.paperCash.toFixed(2)}`;
      }

      const existing = s.paperPositions.find((p) => p.symbol === symbol);
      const newPositions = existing
        ? s.paperPositions.map((p) => p.symbol === symbol
            ? {
                ...p,
                shares:  p.shares + shares,
                avgCost: (p.avgCost * p.shares + price * shares) / (p.shares + shares),
              }
            : p)
        : [...s.paperPositions, { symbol, shares, avgCost: price }];

      const trade: PaperTrade = {
        id:     s._nextPaperTradeId,
        ts:     Date.now(),
        symbol, side: 'buy', shares, price,
      };
      const newCash = s.paperCash - cost;
      const newTrades = [trade, ...s.paperTrades];

      set({
        paperCash:         newCash,
        paperPositions:    newPositions,
        paperTrades:       newTrades,
        _nextPaperTradeId: s._nextPaperTradeId + 1,
      });
      savePaper({
        cash: newCash, positions: newPositions, trades: newTrades,
        nextTradeId: s._nextPaperTradeId + 1,
      });
      get().send({ cmd: 'subscribe', symbol });
      return null;
    },

    paperSell: (symbol, shares, price) => {
      symbol = symbol.trim().toUpperCase();
      if (!symbol)     return 'symbol required';
      if (shares <= 0) return 'shares must be > 0';
      if (price  <= 0) return 'price must be > 0';
      const s = get();
      const existing = s.paperPositions.find((p) => p.symbol === symbol);
      if (!existing)             return `no position in ${symbol}`;
      if (shares > existing.shares) {
        return `selling ${shares} but only hold ${existing.shares}`;
      }

      const proceeds = shares * price;
      const remaining = existing.shares - shares;
      const newPositions = remaining > 0
        ? s.paperPositions.map((p) => p.symbol === symbol
            ? { ...p, shares: remaining }
            : p)
        : s.paperPositions.filter((p) => p.symbol !== symbol);

      const trade: PaperTrade = {
        id:     s._nextPaperTradeId,
        ts:     Date.now(),
        symbol, side: 'sell', shares, price,
      };
      const newCash = s.paperCash + proceeds;
      const newTrades = [trade, ...s.paperTrades];

      set({
        paperCash:         newCash,
        paperPositions:    newPositions,
        paperTrades:       newTrades,
        _nextPaperTradeId: s._nextPaperTradeId + 1,
      });
      savePaper({
        cash: newCash, positions: newPositions, trades: newTrades,
        nextTradeId: s._nextPaperTradeId + 1,
      });
      return null;
    },

    paperReset: () => {
      set({
        paperCash:         PAPER_STARTING_CASH,
        paperPositions:    [],
        paperTrades:       [],
        _nextPaperTradeId: 1,
      });
      savePaper({
        cash: PAPER_STARTING_CASH, positions: [], trades: [], nextTradeId: 1,
      });
    },
  };
});

/* Jaccard similarity of two top-K ticker lists.
 * Matches the semantics of convergence.c on the backend: set-based, ignores counts. */
export function jaccard(a: AggPick[], b: AggPick[]): number {
  if (!a.length && !b.length) return 0;
  const A = new Set(a.map((x) => x.symbol));
  const B = new Set(b.map((x) => x.symbol));
  let inter = 0;
  for (const s of A) if (B.has(s)) inter++;
  const union = A.size + B.size - inter;
  return union === 0 ? 0 : inter / union;
}
