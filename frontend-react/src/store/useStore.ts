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
}

const WS_URL = 'ws://localhost:3001';

function connect(set: (partial: Partial<State>) => void, get: () => State) {
  const ws = new WebSocket(WS_URL);

  ws.onopen = () => {
    set({ _ws: ws });
    get().fetchHistory(get().symbol);
    get().fetchPortfolio();
    get().fetchAlerts();
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
      // triggered alert — remove from list
      const firedId = msg.id as number;
      set((s) => ({ alerts: s.alerts.filter((a) => a.id !== firedId) }));
      return;
    }

    if (type === 'alert_added') {
      const a = msg as unknown as Alert;
      set((s) => ({ alerts: [...s.alerts, a] }));
      return;
    }
  };
}

export const useStore = create<State>((set, get) => {
  // Connect on store creation
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
  };
});
