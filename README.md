# Quorum

as of 6/25/26, implementing Jump Diffusion to model Bates (1996), going from 5 params to 8

An AI-powered stock research platform that combines a **C analytics backend**, a **local-LLM bot ensemble**, a **Node.js bridge**, and a **React frontend** to aggregate LLM-driven stock picks, weight them by a Heston stochastic-vol risk model, and surface a rebalance-ready portfolio in real time.

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                                                              │
│  React (Vite + TS)  ◄── WS :3001 ──►  Node Bridge  ◄── TCP :8765 ──►  C Backend
│  • Holdings panel                      • WS↔TCP mux                    • Polygon.io WS + REST
│  • Compilation (consensus + result)                                    • PostgreSQL persistence
│  • Tick Evaluation:                                                    • Bot aggregation engine
│      Heston MC path bundle             Local vLLM bots ──► :8765       • Heston MC + risk scoring
│      Implied-vol surface               • N persona-bots / k-seed runs  • Rebalance engine
│      Risk / obscurity score            • OpenAI-compatible API         • Convergence detector
│      Rebalance trade list              • No API budget — GPU-bound     • News crawler
│                                                                        • S&P 500 universe
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## Architecture

| Layer | Language | Role |
|---|---|---|
| **Backend** | C11 | Market data (Polygon.io), PostgreSQL persistence, IPC server, bot aggregation, Heston MC + risk scoring, rebalance engine, backtesting, convergence detection, news crawler |
| **Bots** | Python 3.11+ | Persona bots that hit a local vLLM server to pick S&P 500 stocks; supports k-seed parallel ensembles with cross-ensemble disagreement scoring |
| **Bridge** | Node.js 18+ | WebSocket-to-TCP multiplexer so the browser can talk to the C backend |
| **Frontend** | React 18 + TypeScript | Real-time UI: holdings, compilation/consensus, per-symbol Heston diagnostics, MC path bundles, vol surface, rebalance |

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
│       ├── ipc_research.c/h     Bot ensemble, Heston, rebalance, backtest, crawler dispatch
│       ├── db.c/h               PostgreSQL persistence layer
│       ├── bot_picks.c/h        Bot run & pick storage
│       ├── aggregation.c/h      Cross-bot ticker aggregation (FNV-1a hash → top-K)
│       ├── heston.c/h           Heston SV Monte Carlo path generator
│       ├── heston_surface.c/h   Implied-vol surface fit / lookup
│       ├── risk_score.c/h       Per-symbol risk / obscurity score from Heston paths
│       ├── rebalance.c/h        Target-weight + trade-list engine
│       ├── backtest.c/h         Equal-weight backtester (return, Sharpe, max DD, hit rate)
│       ├── convergence.c/h      Ensemble stability detection (Jaccard similarity)
│       ├── crawler.c/h          News crawler + digest builder for bot context
│       ├── sp500_universe.c/h   Static S&P 500 ticker universe
│       └── cJSON.c/h            Vendored JSON parser
├── bots/
│   ├── bot_runner.py            Live ensemble: persona bots over a local vLLM server
│   ├── bot_runner_backtest.py   Historical-window variant with date-bounded news
│   ├── kseed_runner.py          Parallel k-seed ensembles with cross-ensemble disagreement
│   ├── backfill.py              Primes price_cache with historical OHLCV data
│   └── requirements.txt
├── bridge/
│   ├── bridge.js                WebSocket ↔ TCP multiplexer
│   └── package.json
├── frontend-react/
│   ├── index.html
│   ├── src/
│   │   ├── main.tsx
│   │   ├── App.tsx                       Live / Backtest tabs
│   │   ├── index.css
│   │   ├── store/useStore.ts             Zustand global store
│   │   ├── data/gicsSectors.ts           GICS sector lookup for diversity weighting
│   │   ├── lib/diversity.ts              Portfolio diversity helpers
│   │   └── components/
│   │       ├── StatusBar.tsx             Connection status indicator
│   │       ├── PortfolioPanel.tsx        Holdings management
│   │       ├── CompilationPanel.tsx      Ensemble run + consensus result
│   │       ├── TickEvaluationPanel.tsx   Per-symbol workspace (host of the panels below)
│   │       ├── MCPathBundlePanel.tsx     Heston MC path fan chart
│   │       ├── HestonSurfacePanel.tsx    Implied-vol surface viewer
│   │       └── HestonDiagnosticsPanel.tsx Calibration diagnostics
│   ├── package.json
│   ├── tailwind.config.js
│   └── vite.config.ts
├── Makefile                     Top-level: builds all components; `make run` orchestrates; `make vllm` launches the LLM server
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
| `openai` SDK | `>=1.40.0` (used as the OpenAI-compatible client for vLLM) |
| vLLM server | Any version that serves the OpenAI-compatible `/v1` endpoint. A 24 GB GPU (e.g. RTX 3090) runs Qwen 2.5 14B Instruct AWQ comfortably. |

There is no longer a hosted-API dependency or budget cap — concurrency is bounded by the GPU.

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

No LLM API key is required: the bots talk to a local vLLM server.

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

The C backend auto-creates its tables on first connection: `portfolio`, `alerts`, `price_cache`, `order_history`, `bot_runs`, `bot_picks`, `backtest_results`, plus crawler, Heston, and rebalance tables.

### 4. Create `.env` at the repo root

```
POLYGON_API_KEY=your_polygon_key_here
DB_PASSWORD=postgres
# Optional vLLM overrides (defaults: localhost:8000, Qwen2.5-14B-Instruct-AWQ)
# VLLM_BASE_URL=http://localhost:8000/v1
# VLLM_MODEL=Qwen/Qwen2.5-14B-Instruct-AWQ
```

`make run` reads `POLYGON_API_KEY` and `DB_PASSWORD` from this file.

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
make vllm       # in one terminal: serves the local LLM on :8000
make run        # in another terminal: backend + bridge + frontend dev server
                # Ctrl-C stops all three.
```

### Manually (separate terminals)

```bash
# Local LLM server
make vllm
# Or: vllm serve Qwen/Qwen2.5-14B-Instruct-AWQ --host 0.0.0.0 --port 8000 \
#        --enable-prefix-caching --max-model-len 8192

# C backend
cd backend && ./quorum-backend YOUR_POLYGON_API_KEY [DB_PASSWORD]
# DB_PASSWORD defaults to "postgres". IPC server listens on :8765.

# Node bridge
cd bridge && node bridge.js
# Connects to backend on :8765, serves WebSocket on :3001.

# React frontend
cd frontend-react && npm run dev
# Vite dev server; connects to bridge on :3001.

# Python bots
cd bots && python bot_runner.py
# Persona bots hit vLLM, send picks to backend via TCP :8765.

# Parallel k-seed ensembles + cross-ensemble disagreement:
python bots/kseed_runner.py --k 4 --wave-size 32 --n-bots-max 200

# Historical backtest run:
python bots/bot_runner_backtest.py
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

### Research, bots, Heston & rebalance (`ipc_research.c`)

```json
{"cmd":"sp500_list"}
{"cmd":"bot_runs_list"}
{"cmd":"bot_run_create",     "label":"nightly-2026-04-07","n_bots_target":500,"hold_days":30}
{"cmd":"bot_picks_ingest",   "run_id":42,"bot_index":17,"persona":"value","picks":["AAPL","MSFT","NVDA"]}
{"cmd":"bot_run_finish",     "run_id":42,"n_bots_actual":487}
{"cmd":"aggregate_run",      "run_id":42,"k":20}
{"cmd":"convergence_check",  "run_id":42,"k":20}
{"cmd":"heston_score_run",   "run_id":42,"k":20}
{"cmd":"ranking_blend",      "run_id":42,"k":20}
{"cmd":"rebalance_check",    "run_id":42,"k":20,"horizon_days":21}
{"cmd":"rebalance_resolve",  "event_id":17,"action":"sell"}
{"cmd":"heston_path_bundle", "symbol":"NVDA"}
{"cmd":"heston_surface",     "symbol":"NVDA"}
{"cmd":"heston_diagnostics", "symbol":"NVDA","n_paths":4000}
{"cmd":"backtest_run",       "run_id":42,"k":20,"hold_days":30}
{"cmd":"crawl_news",         "limit":50}
{"cmd":"get_news_digest",    "max_chars":32000,"days":7}
```

### Backend → client events

```json
{"type":"quote",             "symbol":"AAPL","price":150.25,"bid":150.20,"ask":150.30,"volume":123456,"ts":1700000000000}
{"type":"history",           "symbol":"AAPL","bars":[{"t":...,"o":...,"h":...,"l":...,"c":...,"v":...}]}
{"type":"portfolio",         "holdings":[{"symbol":"AAPL","shares":10,"avg_price":150.0,"current":161.5}]}
{"type":"sp500_list",        "tickers":["A","AAL", ...]}
{"type":"bot_run_created",   "run_id":42}
{"type":"bot_picks_ack",     "run_id":42,"bot_index":17,"n":3,"n_dropped":0}
{"type":"aggregate_result",  "run_id":42,"top":[{"ticker":"NVDA","count":312}, ...]}
{"type":"heston_score",      "run_id":42,"scores":[{"ticker":"NVDA","risk":0.42}, ...]}
{"type":"ranking_blend",     "run_id":42,"ranking":[{"ticker":"NVDA","blended":0.81}, ...]}
{"type":"rebalance_check",   "run_id":42,"events":[{"event_id":17,"ticker":"AAPL","action":"sell","reason":"..."}, ...]}
{"type":"heston_path_bundle","symbol":"NVDA","paths":[[...], ...]}
{"type":"heston_surface",    "symbol":"NVDA","grid":[[...], ...]}
{"type":"backtest_result",   "run_id":42,"return":0.12,"sharpe":1.5,"max_drawdown":-0.08,"hit_rate":0.65}
{"type":"news_digest",       "days":7,"text":"..."}
{"type":"error",             "message":"..."}
```

You can poke the protocol manually with `nc localhost 8765`.

---

## How the Bot Ensemble Works

1. **`bot_runner.py`** spawns N persona bots (value, growth, momentum, contrarian, sector specialist, …) and runs them concurrently against a local vLLM OpenAI-compatible endpoint. Prefix caching means the long system prompt is shared across the ensemble at near-zero cost.
2. Each bot independently selects a small basket of stocks from the S&P 500 universe. Crawled news context is optionally injected per run.
3. Picks stream into the C backend via `bot_picks_ingest`.
4. `aggregate_run` runs the picks through an FNV-1a hash counter — tickers picked by the most bots rise to the top-K.
5. `convergence_check` monitors Jaccard stability across aggregation windows to decide when adding more bots stops changing the consensus.
6. `heston_score_run` calibrates a Heston SV model per candidate ticker and produces a risk / obscurity score from the MC path bundle. `ranking_blend` combines bot vote share with that score.
7. `rebalance_check` compares the blended ranking against current holdings and emits a trade list (buy/sell/hold events) that the operator resolves via `rebalance_resolve`.
8. The **backtester** simulates an equal-weighted portfolio of the top picks over a configurable holding period, computing total return, Sharpe ratio, max drawdown, hit rate, and alpha vs SPY.

For multi-ensemble parallelism, **`kseed_runner.py`** runs K ensembles in parallel against the same vLLM server with distinct persona seeds and sampling temperatures, and pipes the cross-ensemble disagreement variance back through `ranking_blend` as a second variance-reduction layer. For historical-window runs, **`bot_runner_backtest.py`** pins bots to a date range and pulls cached OHLCV instead of live quotes.

---

## Heston Risk Layer

The Heston stochastic-vol model is the project's risk-weighting backbone:

- **`heston.c`** generates Monte Carlo paths under the Heston SV process (`dS_t = μ S_t dt + √v_t S_t dW^S_t`, `dv_t = κ(θ − v_t) dt + σ √v_t dW^v_t`, `⟨dW^S, dW^v⟩ = ρ dt`).
- **`heston_surface.c`** fits / serves an implied-vol surface for diagnostics and calibration sanity checks.
- **`risk_score.c`** condenses each ticker's MC path bundle into a single risk / obscurity scalar that downweights overcrowded or excessively volatile picks.
- **`rebalance.c`** combines the blended ranking with holdings, sector diversity (GICS), and the risk score to emit a concrete trade list.

The React **Tick Evaluation** panel hosts the per-symbol MC fan chart, implied-vol surface, and Heston diagnostics side-by-side.

---

## Honest Limitations

- **Survivorship bias** — the S&P 500 list is the *current* snapshot, not point-in-time. Backtests crossing constituent rebalances are slightly optimistic.
- **LLM training-cutoff leakage** — the model knows what happened to a given ticker, even if you tell it the date is earlier. Honest backtests must use data *after* the model's training cutoff.
- **Transaction cost** is a flat 0.2% — no bid-ask spread, market impact, or taxes modeled.
- **Risk-free rate = 0** in the Sharpe calc.
- **Heston calibration** uses a fixed parameter grid rather than a full optimizer; the surface is informative, not production-grade for derivatives pricing.

See `NOTES.md` for the full design rationale and decisions log.

---

## Tech Stack

| Component | Technology |
|---|---|
| Backend | C11, OpenSSL, libcurl, libpq, pthreads |
| Database | PostgreSQL 17 |
| Bots | Python 3.11+, `openai` client → local vLLM (Qwen 2.5 14B Instruct AWQ default) |
| Bridge | Node.js 18+, `ws` |
| Frontend | React 18, TypeScript, Vite 5, Zustand, Tailwind CSS, lightweight-charts |
