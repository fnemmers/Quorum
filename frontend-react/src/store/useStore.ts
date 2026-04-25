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
  researchBusy: boolean;
  researchError: string | null;

  _ws: WebSocket | null;
  _nextBlotterId: number;

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
  clearResearchError: () => void;
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

export const useStore = create<State>((set, get) => {
  setTimeout(() => connect(set, get), 0);

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
    researchBusy: false,
    researchError: null,

    _ws: null,
    _nextBlotterId: 1,

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
      setTimeout(() => get().fetchPortfolio(), 200);
    },

    removeHolding: (symbol) => {
      get().send({ cmd: 'portfolio_remove', symbol });
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

    clearResearchError: () => set({ researchError: null }),
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
