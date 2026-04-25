# StockApp

An AI-powered stock analysis platform that combines a **C backend**, **Python bot ensemble**, **Node.js bridge**, and **React frontend** to aggregate LLM-driven stock picks, backtest them, and display results in real time.

```
┌──────────────────────────────────────────────────────────────────────────┐
│                                                                          │
│  React (Vite + TS)  ◄── WS :3001 ──►  Node Bridge  ◄── TCP ──►  C Backend
│  • Candlestick chart                   • WS↔TCP mux              • Polygon.io WS + REST
│  • Portfolio tracker                                              • PostgreSQL persistence
│  • Price alerts                                                   • Bot aggregation engine
│  • Risk metrics                     Python Bots ── TCP :8765 ──► • Backtester
│  • Trade blotter                    • 500 Claude Haiku instances  • Convergence detector
│                                     • Budget-aware ($50/mo cap)  • S&P 500 universe
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

---

## Architecture

| Layer | Language | Role |
|---|---|---|
| **Backend** | C | Market data (Polygon.io), PostgreSQL persistence, IPC server, bot aggregation, backtesting, convergence detection |
| **Bots** | Python | Spawns 500 Claude Haiku instances with different investment personas to pick S&P 500 stocks |
| **Bridge** | Node.js | WebSocket-to-TCP multiplexer so the browser can talk to the C backend |
| **Frontend** | React + TypeScript | Real-time UI with charts, portfolio management, alerts, and risk metrics |

---

## Requirements

### C Backend
| Dependency | macOS (Homebrew) | Ubuntu/Debian |
|---|---|---|
| GCC >= 11 | `brew install gcc` | `apt install gcc` |
| OpenSSL | `brew install openssl` | `apt install libssl-dev` |
| libcurl | `brew install curl` | `apt install libcurl4-openssl-dev` |
| PostgreSQL (libpq) | `brew install postgresql@17` | `apt install libpq-dev` |
| pthreads | included | included |

### Python Bots
| Requirement | Version |
|---|---|
| Python | 3.11+ |
| anthropic SDK | see `bots/requirements.txt` |

### Node Bridge
| Requirement | Version |
|---|---|
| Node.js | 18+ |
| ws | see `bridge/node_modules/` |

### React Frontend
| Requirement | Version |
|---|---|
| Node.js | 18+ |
| npm | 9+ |

---

## Setup

### 1. Get API Keys

- **Polygon.io** — Sign up at [polygon.io](https://polygon.io). The free tier provides 15-minute delayed data.
- **Anthropic** — Get a Claude API key for the bot ensemble.

### 2. Download cJSON

```bash
cd backend/src/
curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h
curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c
```

### 3. Set Up PostgreSQL

Create a database for the backend. The C backend will auto-create tables (`portfolio`, `alerts`, `price_cache`, `order_history`, `bot_runs`, `bot_picks`, `backtest_results`) on first connection.

### 4. Install Dependencies

```bash
# Frontend
cd frontend-react && npm install

# Bridge
cd bridge && npm install

# Bots
cd bots && pip install -r requirements.txt
```

---

## Build & Run

### C Backend
```bash
cd backend
make
# Produces: stock-backend (macOS/Linux) or stock-backend.exe (Windows)

./stock-backend YOUR_POLYGON_API_KEY
# Starts IPC server on port 8765
```

### Node Bridge
```bash
cd bridge
node bridge.js
# Connects to backend on :8765, serves WebSocket on :3001
```

### React Frontend
```bash
cd frontend-react
npm run dev
# Vite dev server, connects to bridge on :3001
```

### Python Bots
```bash
cd bots
python bot_runner.py
# Spawns 500 Claude Haiku bots, sends picks to backend via TCP :8765
```

---

## Project Structure

```
stock-app/
├── backend/
│   ├── Makefile
│   └── src/
│       ├── main.c              Entry point, initializes all subsystems
│       ├── market_data.c/h     Global price state, thread-safe ring buffers
│       ├── polygon_ws.c/h      WebSocket client for Polygon.io real-time feed
│       ├── polygon_rest.c/h    REST client for Polygon.io historical data
│       ├── ipc_server.c/h      TCP server (:8765), command dispatcher
│       ├── ipc_research.c/h    Bot ensemble & backtest command handlers
│       ├── db.c/h              PostgreSQL persistence layer
│       ├── bot_picks.c/h       Bot run & pick storage
│       ├── aggregation.c/h     Cross-bot ticker aggregation (top-K picks)
│       ├── backtest.c/h        Portfolio backtester (Sharpe, drawdown, hit rate)
│       ├── convergence.c/h     Ensemble stability detection (Jaccard similarity)
│       ├── sp500_universe.c/h  S&P 500 ticker universe
│       └── cJSON.c/h           Vendored JSON parser
├── bots/
│   ├── bot_runner.py           Orchestrator: 500 Claude Haiku stock pickers
│   ├── backfill.py             Primes price_cache with historical OHLCV data
│   ├── budget.py               Monthly API spend tracker ($50/mo cap)
│   └── requirements.txt
├── bridge/
│   └── bridge.js               WebSocket ↔ TCP multiplexer
├── frontend-react/
│   ├── src/
│   │   ├── App.tsx
│   │   └── components/
│   │       ├── Chart.tsx           Candlestick chart (lightweight-charts)
│   │       ├── QuotePanel.tsx      Real-time price ticker
│   │       ├── PortfolioPanel.tsx  Holdings management
│   │       ├── AlertPanel.tsx      Price alert CRUD
│   │       ├── RiskPanel.tsx       Risk/performance metrics
│   │       ├── TradeBlotter.tsx    Order history log
│   │       └── StatusBar.tsx       Connection status indicator
│   ├── package.json
│   ├── tailwind.config.js
│   └── vite.config.ts
└── frontend/                   Legacy Java/JavaFX frontend (archived)
```

---

## IPC Protocol (TCP :8765)

The C backend speaks **newline-delimited JSON** over TCP.

### Client → Backend (commands)
```json
{"cmd":"subscribe",        "symbol":"AAPL"}
{"cmd":"unsubscribe",      "symbol":"AAPL"}
{"cmd":"history",          "symbol":"AAPL","multiplier":1,"timespan":"day","from":"2024-01-01","to":"2024-12-31"}
{"cmd":"snapshot",         "symbol":"AAPL"}
{"cmd":"portfolio_add",    "symbol":"AAPL","shares":10,"price":150.0}
{"cmd":"portfolio_remove", "symbol":"AAPL"}
{"cmd":"portfolio_get"}
{"cmd":"alert_add",        "symbol":"AAPL","condition":"above","price":160.0}
{"cmd":"alert_remove",     "id":3}
{"cmd":"alert_list"}
{"cmd":"bot_run",          "bot_count":500}
{"cmd":"backtest",         "run_id":1,"hold_days":30}
```

### Backend → Client (events)
```json
{"type":"quote",      "symbol":"AAPL","price":150.25,"bid":150.20,"ask":150.30,"volume":123456,"ts":1700000000000}
{"type":"history",    "symbol":"AAPL","bars":[{"t":...,"o":...,"h":...,"l":...,"c":...,"v":...}]}
{"type":"alert",      "id":3,"symbol":"AAPL","condition":"above","trigger":160.0,"price":161.5}
{"type":"portfolio",  "holdings":[{"symbol":"AAPL","shares":10,"avg_price":150.0,"current":161.5}]}
{"type":"bot_result", "run_id":1,"top_picks":[...]}
{"type":"backtest",   "run_id":1,"return":0.12,"sharpe":1.5,"max_drawdown":-0.08,"hit_rate":0.65}
{"type":"error",      "message":"..."}
```

---

## How the Bot Ensemble Works

1. **bot_runner.py** spawns 500 Claude Haiku instances, each with a unique investment persona (value, growth, momentum, contrarian, etc.)
2. Each bot independently selects ~10 stocks from the S&P 500 universe
3. Picks are sent to the C backend, which **aggregates** them — tickers picked by the most bots rise to the top
4. The **convergence detector** monitors stability across aggregation windows using Jaccard similarity
5. The **backtester** simulates an equal-weighted portfolio of the top picks over a configurable holding period, computing return, Sharpe ratio, max drawdown, and hit rate vs SPY
6. **Budget tracking** enforces a $50/month cap on Anthropic API spend

---

## Tech Stack

| Component | Technology |
|---|---|
| Backend | C11, OpenSSL, libcurl, libpq, pthreads |
| Database | PostgreSQL |
| Bots | Python, Anthropic Claude API |
| Bridge | Node.js, ws |
| Frontend | React 18, TypeScript, Vite, Zustand, Tailwind CSS, lightweight-charts |
