# Quorum

An AI-powered stock research platform that combines a **C analytics backend**, a **Python bot ensemble**, a **Node.js bridge**, and a **React frontend** to aggregate LLM-driven stock picks, backtest the consensus, and display results in real time.

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                                                              │
│  React (Vite + TS)  ◄── WS :3001 ──►  Node Bridge  ◄── TCP :8765 ──►  C Backend
│  • Candlestick chart                   • WS↔TCP mux                    • Polygon.io WS + REST
│  • Portfolio tracker                                                   • PostgreSQL persistence
│  • Price alerts                                                        • Bot aggregation engine
│  • Risk metrics                       Python Bots ── TCP :8765 ──►     • Backtester
│  • Research / consensus               • N Claude Haiku personas        • Convergence detector
│  • Paper-trail audit log              • Budget-aware ($50/mo cap)      • News crawler
│                                                                        • S&P 500 universe
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## Architecture

| Layer | Language | Role |
|---|---|---|
| **Backend** | C11 | Market data (Polygon.io), PostgreSQL persistence, IPC server, bot aggregation, backtesting, convergence detection, news crawler |
| **Bots** | Python 3.11+ | Spawns Claude Haiku personas to pick S&P 500 stocks; budget-tracked |
| **Bridge** | Node.js 18+ | WebSocket-to-TCP multiplexer so the browser can talk to the C backend |
| **Frontend** | React 18 + TypeScript | Real-time UI: charts, portfolio, alerts, risk, research, audit log |

All inter-process communication is **newline-delimited JSON** over TCP `:8765`.

---

## Project Structure

```
quorum/
├── backend/
│   ├── Makefile
│   └── src/
│       ├── main.c               Entry point; initializes all subsystems
│       ├── market_data.c/h      Global price state, thread-safe ring buffers
│       ├── polygon_ws.c/h       WebSocket client for Polygon.io real-time feed
│       ├── polygon_rest.c/h     REST client for Polygon.io historical data
│       ├── ipc_server.c/h       TCP server (:8765); market & portfolio/alert dispatch
│       ├── ipc_research.c/h     Bot ensemble, backtest, and crawler dispatch
│       ├── db.c/h               PostgreSQL persistence layer
│       ├── bot_picks.c/h        Bot run & pick storage
│       ├── aggregation.c/h      Cross-bot ticker aggregation (FNV-1a hash → top-K)
│       ├── backtest.c/h         Equal-weight backtester (return, Sharpe, max DD, hit rate)
│       ├── convergence.c/h      Ensemble stability detection (Jaccard similarity)
│       ├── crawler.c/h          News crawler + digest builder for bot context
│       ├── sp500_universe.c/h   Static S&P 500 ticker universe
│       └── cJSON.c/h            Vendored JSON parser
├── bots/
│   ├── bot_runner.py            Live ensemble: spawns Claude Haiku personas
│   ├── bot_runner_backtest.py   Historical-window variant for backtesting
│   ├── backfill.py              Primes price_cache with historical OHLCV data
│   ├── budget.py                Persistent monthly API spend tracker ($50/mo cap)
│   └── requirements.txt
├── bridge/
│   ├── bridge.js                WebSocket ↔ TCP multiplexer
│   └── package.json
├── frontend-react/
│   ├── index.html
│   ├── src/
│   │   ├── main.tsx
│   │   ├── App.tsx
│   │   ├── index.css
│   │   ├── store/
│   │   │   └── useStore.ts          Zustand global store
│   │   └── components/
│   │       ├── Chart.tsx            Candlestick chart (lightweight-charts)
│   │       ├── QuotePanel.tsx       Real-time price ticker
│   │       ├── PortfolioPanel.tsx   Holdings management
│   │       ├── AlertPanel.tsx       Price alert CRUD
│   │       ├── RiskPanel.tsx        Risk / performance metrics
│   │       ├── ResearchPanel.tsx    Bot consensus + backtest results
│   │       ├── PaperTrailPanel.tsx  Audit log of bot runs
│   │       ├── TradeBlotter.tsx     Order history log
│   │       └── StatusBar.tsx        Connection status indicator
│   ├── package.json
│   ├── tailwind.config.js
│   └── vite.config.ts
├── Makefile                     Top-level: builds all components, `make run` orchestrates
└── NOTES.md                     Design notes & decisions log
```

---

## Requirements

### C Backend

| Dependency | macOS (Homebrew) | Ubuntu/Debian | Windows (MSYS2 MinGW64) |
|---|---|---|---|
| GCC ≥ 11 | `brew install gcc` | `apt install gcc` | `pacman -S mingw-w64-x86_64-gcc` |
| OpenSSL | `brew install openssl` | `apt install libssl-dev` | `pacman -S mingw-w64-x86_64-openssl` |
| libcurl | `brew install curl` | `apt install libcurl4-openssl-dev` | `pacman -S mingw-w64-x86_64-curl` |
| PostgreSQL (libpq) | `brew install postgresql@17` | `apt install libpq-dev` | `pacman -S mingw-w64-x86_64-postgresql` |
| pthreads | included | included | included |

### Python Bots

| Requirement | Version |
|---|---|
| Python | 3.11+ |
| `anthropic` SDK | `>=0.40.0` (see `bots/requirements.txt`) |

### Node Bridge & React Frontend

| Requirement | Version |
|---|---|
| Node.js | 18+ |
| npm | 9+ |

Frontend deps (`frontend-react/package.json`): React 18, TypeScript, Vite 5, Zustand, Tailwind CSS, `lightweight-charts`.

---

## Setup

### 1. Get API keys

- **Polygon.io** — sign up at [polygon.io](https://polygon.io). The free tier provides 15-minute delayed data.
- **Anthropic** — get a Claude API key for the bot ensemble.

### 2. Vendor cJSON (only if missing)

`cJSON.c/h` is committed under `backend/src/`. If you delete it for any reason, restore it via:

```bash
cd backend/src
curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h
curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c
```

### 3. Set up PostgreSQL

```bash
createdb stockapp
```

The C backend auto-creates its tables on first connection: `portfolio`, `alerts`, `price_cache`, `order_history`, `bot_runs`, `bot_picks`, `backtest_results`, plus crawler tables.

### 4. Create `.env` at the repo root

```
POLYGON_API_KEY=your_polygon_key_here
ANTHROPIC_API_KEY=your_anthropic_key_here
```

`make run` reads `POLYGON_API_KEY` from this file. The Python bots read `ANTHROPIC_API_KEY` from the environment.

### 5. Install dependencies

From the repo root:

```bash
make            # builds backend + installs bridge/bot/frontend deps
```

Or component-by-component:

```bash
make backend    # C backend only
make bridge     # Node bridge deps
make bots       # Python bot venv + deps
make frontend   # React production bundle
```

---

## Build & Run

### Everything at once

```bash
make run
# Launches backend + bridge + frontend dev server.
# Ctrl-C stops all three.
```

### Manually (separate terminals)

```bash
# C backend
cd backend && ./quorum-backend YOUR_POLYGON_API_KEY [DB_PASSWORD]
# DB_PASSWORD defaults to "postgres". IPC server listens on :8765.

# Node bridge
cd bridge && node bridge.js
# Connects to backend on :8765, serves WebSocket on :3001.

# React frontend
cd frontend-react && npm run dev
# Vite dev server; connects to bridge on :3001.

# Python bots (separate terminal)
cd bots && python bot_runner.py
# Spawns Claude Haiku bots, sends picks to backend via TCP :8765.

# Or for a historical backtest:
python bot_runner_backtest.py
```

### Clean

```bash
make clean      # removes C objects, frontend dist/node_modules, bridge node_modules
                # (Python venv is left intact — remove bots/venv manually if desired)
```

---

## IPC Protocol (TCP :8765)

The C backend speaks **newline-delimited JSON** over TCP.

### Market data & user state (`ipc_server.c`)

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
```

### Research, bots & backtesting (`ipc_research.c`)

```json
{"cmd":"sp500_list"}
{"cmd":"bot_runs_list"}
{"cmd":"bot_run_create",   "label":"nightly-2026-04-07","n_bots_target":500,"hold_days":30}
{"cmd":"bot_picks_ingest", "run_id":42,"bot_index":17,"persona":"value","picks":["AAPL","MSFT","NVDA"]}
{"cmd":"bot_run_finish",   "run_id":42,"n_bots_actual":487}
{"cmd":"aggregate_run",    "run_id":42,"k":20}
{"cmd":"backtest_run",     "run_id":42,"k":20,"hold_days":30}
{"cmd":"crawl_news",       "limit":50}
{"cmd":"get_news_digest",  "max_chars":32000,"days":7}
```

### Backend → client events

```json
{"type":"quote",           "symbol":"AAPL","price":150.25,"bid":150.20,"ask":150.30,"volume":123456,"ts":1700000000000}
{"type":"history",         "symbol":"AAPL","bars":[{"t":...,"o":...,"h":...,"l":...,"c":...,"v":...}]}
{"type":"alert",           "id":3,"symbol":"AAPL","condition":"above","trigger":160.0,"price":161.5}
{"type":"portfolio",       "holdings":[{"symbol":"AAPL","shares":10,"avg_price":150.0,"current":161.5}]}
{"type":"sp500_list",      "tickers":["A","AAL", ...]}
{"type":"bot_run_created", "run_id":42}
{"type":"bot_picks_ack",   "run_id":42,"bot_index":17,"n":3,"n_dropped":0}
{"type":"aggregate_result","run_id":42,"top":[{"ticker":"NVDA","count":312}, ...]}
{"type":"backtest_result", "run_id":42,"return":0.12,"sharpe":1.5,"max_drawdown":-0.08,"hit_rate":0.65}
{"type":"news_digest",     "days":7,"text":"..."}
{"type":"error",           "message":"..."}
```

You can poke the protocol manually with `nc localhost 8765`.

---

## How the Bot Ensemble Works

1. **`bot_runner.py`** spawns N Claude Haiku instances, each with a unique investment persona (value, growth, momentum, contrarian, sector specialist, etc.).
2. Each bot independently selects a small basket of stocks from the S&P 500 universe. Optional crawled news context is fed to bots via prompt caching so the expensive base context is reused across the ensemble (~10× cost savings at scale).
3. Picks stream into the C backend via `bot_picks_ingest`.
4. `aggregate_run` runs the picks through an FNV-1a hash counter — tickers picked by the most bots rise to the top-K.
5. The **convergence detector** monitors stability across aggregation windows using Jaccard similarity to decide when adding more bots stops changing the consensus.
6. The **backtester** simulates an equal-weighted portfolio of the top picks over a configurable holding period, computing total return, Sharpe ratio, max drawdown, hit rate, and alpha vs SPY.
7. **`budget.py`** enforces a persistent monthly cap on Anthropic API spend (default $50/mo) and refuses to launch if exceeded.

For backtesting historical windows, use `bot_runner_backtest.py` — it pins bots to a date range and pulls cached OHLCV instead of live quotes.

---

## Honest Limitations

- **Survivorship bias** — the S&P 500 list is the *current* snapshot, not point-in-time. Backtests crossing constituent rebalances are slightly optimistic.
- **LLM training-cutoff leakage** — the model knows what happened to a given ticker, even if you tell it the date is earlier. Honest backtests must use data *after* the model's training cutoff.
- **Transaction cost** is a flat 0.2% — no bid-ask spread, market impact, or taxes modeled.
- **Risk-free rate = 0** in the Sharpe calc.

See `NOTES.md` for the full design rationale and decisions log.

---

## Tech Stack

| Component | Technology |
|---|---|
| Backend | C11, OpenSSL, libcurl, libpq, pthreads |
| Database | PostgreSQL 17 |
| Bots | Python 3.11+, Anthropic Claude API (Haiku) |
| Bridge | Node.js 18+, `ws` |
| Frontend | React 18, TypeScript, Vite 5, Zustand, Tailwind CSS, lightweight-charts |
