# Quorum

As of 8/21, I am looking for historical option chain data to properly fit bates before live trading execution.

A market-maker-style option research platform. A **C11 analytics backend** calibrates a **Bates stochastic-vol-with-jumps** model per underlying, overlays a **news-driven jump signal**, prices an option grid via the Lewis (2001) Fourier integral, and ranks the grid by the **Q vs P edge**: the gap between the Bates-implied model IV and a synthesized market IV. A **React frontend** surfaces the ranking, MC path bundles, IV surfaces, and Bates diagnostics in real time.

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  React (Vite + TS)  ◄── WS :3001 ──►  Node Bridge  ◄── TCP :8765 ──►  C Backend
│  • Option ranking (Q vs P)                                             • Polygon.io WS + REST
│  • Bates backtest table                                                • PostgreSQL persistence
│  • News-jump panel                                                     • Bates calibration + Lewis pricer
│  • Tick Evaluation:                                                    • News-jump overlay (crawler → params)
│      Heston/Bates MC fan chart                                         • Delta-hedge P&L simulator
│      Implied-vol surface                                               • Cross-normalized z-score fusion
│      Heston diagnostics                                                • S&P 500 universe
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## The core idea: two Bates evaluations, one Q-vs-P ranking

The pipeline calibrates Bates **twice** and rates each option by the disagreement between them and the market:

| Variant | What's fed in | What comes out |
|---|---|---|
| **Market-aligned Bates** | ~180 daily bars only. Pure historical calibration; no news, no overlays. | A `model_iv` that reflects the underlying's realized-vol / jump behavior over the calibration window. |
| **Jump-parameter-influenced Bates** | Same base calibration, then `news_jump_apply()` mutates `λ`, `μ_j`, `σ_j` based on the freshest per-symbol news signal. | A `model_iv` that reflects the same underlying **conditioned on the current news window** (earnings, guidance cuts, M&A, macro shocks). |

Each variant produces a Bates characteristic function → the Lewis Fourier integral prices the option → we invert to a Black-Scholes-equivalent IV. Then:

```
edge_vol_pts = market_iv − model_iv    ← the Q vs P gap
```

- **Market-aligned edge** answers: *is this option mispriced vs the underlying's own historical dynamics?*
- **News-influenced edge** answers: *is this option mispriced vs the underlying's dynamics after we absorb what today's news implies about the jump distribution?*

`option_score.c` fuses these into a single blended, sortable **final Q-vs-P ranking**:

```
blended_option_score
    = w_edge   · z_bates_edge     (per-expiry-bucket normalization)
    + w_news   · z_news_jump      (per-universe normalization)
    + w_convex · z_convexity      (gamma / vega, per-expiry-bucket)

defaults: w_edge=0.60, w_news=0.27, w_convex=0.13
```

`z_bates_edge` is the market-aligned Q-vs-P gap, standardized within a maturity bucket so 7-DTE contracts don't dominate on raw vol scale. `z_news_jump` is the news-driven adjustment applied to the same underlying. `z_convexity` is `gamma / vega`, i.e. premium efficiency per unit vega exposure. Sign is preserved via `signed_edge` (+1 sell rich vol, −1 buy cheap vol); the frontend interprets `|blended_option_score|` as conviction.

---

## Architecture

| Layer | Language | Role |
|---|---|---|
| **Backend** | C11 | Polygon.io market data, PostgreSQL persistence, IPC server, Bates calibration + Lewis pricer, news-jump overlay, delta-hedged MC backtest, option score fusion, news crawler |
| **Bridge** | Node.js 18+ | WebSocket-to-TCP multiplexer so the browser can talk to the C backend |
| **Frontend** | React 18 + TypeScript | Real-time UI: option ranking, Bates backtest table, news-jump status, per-symbol MC path bundle, IV surface, Heston/Bates diagnostics |

All IPC is **newline-delimited JSON** over TCP `:8765`.

---

## Project Structure

```
quorum/
├── backend/
│   ├── Makefile
│   └── src/
│       ├── main.c                 Entry point; initializes all subsystems
│       ├── market_data.c/h        Global price state, thread-safe ring buffers
│       ├── polygon_ws.c/h         WebSocket client for Polygon.io real-time
│       ├── polygon_rest.c/h       REST client for Polygon.io historical + news
│       ├── ipc_server.c/h         TCP server (:8765); market & portfolio dispatch
│       ├── ipc_research.c/h       Bates, news-jump, option-score dispatch
│       ├── db.c/h                 PostgreSQL persistence layer
│       ├── heston.c/h             Bates/Heston SV MC path generator + CF
│       ├── heston_surface.c/h     Implied-vol surface fit / lookup
│       ├── bates_backtest.c/h     Grid pricer + delta-hedge P&L simulator
│       ├── news_jump.c/h          News → (lam_bump, mu_j_bias, sigma_j_bump) overlay
│       ├── option_score.c/h       z-score fusion → final Q-vs-P ranking
│       ├── crawler.c/h            Polygon news crawler + per-ticker digest
│       ├── sp500_universe.c/h     Static S&P 500 ticker universe
│       └── cJSON.c/h              Vendored JSON parser
├── bridge/
│   ├── bridge.js                  WebSocket ↔ TCP multiplexer
│   └── package.json
├── frontend-react/
│   ├── src/
│   │   ├── App.tsx
│   │   ├── store/useStore.ts
│   │   └── components/
│   │       ├── StatusBar.tsx
│   │       ├── OptionRankingPanel.tsx     Final blended Q-vs-P ranking
│   │       ├── BatesBacktestPanel.tsx     Per-option Bates backtest rows
│   │       ├── NewsJumpPanel.tsx          Per-symbol news overlay status
│   │       ├── TickEvaluationPanel.tsx    Per-symbol workspace (host)
│   │       ├── MCPathBundlePanel.tsx      MC fan chart
│   │       ├── HestonSurfacePanel.tsx     Implied-vol surface viewer
│   │       ├── HestonDiagnosticsPanel.tsx Calibration diagnostics
│   │       └── Chart.tsx
│   ├── package.json
│   ├── tailwind.config.js
│   └── vite.config.ts
├── Makefile                       Top-level: builds all components; `make run` orchestrates
└── NOTES.md                       Design notes & decisions log
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

### Node Bridge & React Frontend

| Requirement | Version |
|---|---|
| Node.js | 18+ |
| npm | 9+ |

Frontend deps (`frontend-react/package.json`): React 18, TypeScript, Vite 5, Zustand, Tailwind CSS, `lightweight-charts`.

---

## Setup

### 1. Get API keys

- **Polygon.io**: sign up at [polygon.io](https://polygon.io). The free tier gives 15-minute delayed quotes and is enough to drive the crawler + calibration.
- **E\*TRADE** *(optional, live execution, not yet wired)*: sandbox and production key pairs are read from `.env` for future integration.

### 2. Vendor cJSON (only if missing)

`cJSON.c/h` is committed under `backend/src/`. Restore if deleted:

```bash
cd backend/src
curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h
curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c
```

### 3. Set up PostgreSQL

```bash
createdb stockapp
```

The C backend auto-creates its tables on first connection: `price_cache`, `portfolio`, `alerts`, `order_history`, plus `news_cache`, `news_jump_signal`, `bates_backtest_rows`, `option_score_rows`, and Heston calibration tables.

### 4. Create `.env` at the repo root

```
POLYGON_API_KEY=your_polygon_key_here
DB_PASSWORD=postgres

# Optional: E*TRADE (not yet wired for live orders)
# ETRADE_SANDBOX_KEY=...
# ETRADE_SANDBOX_SECRET=...
# ETRADE_LIVE_KEY=...
# ETRADE_LIVE_SECRET=...
```

`make run` reads `POLYGON_API_KEY` and `DB_PASSWORD`.

### 5. Install dependencies

From the repo root:

```bash
make            # builds backend + installs bridge/frontend deps
```

Or component-by-component:

```bash
make backend    # C backend only
make bridge     # Node bridge deps
make frontend   # React production bundle
```

---

## Build & Run

```bash
make run        # backend + bridge + frontend dev server; Ctrl-C stops all three
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
```

### Clean

```bash
make clean      # removes C objects, frontend dist/node_modules, bridge node_modules
```

---

## IPC Protocol (TCP :8765)

Newline-delimited JSON.

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

### Research: Bates, news-jump, option scoring (`ipc_research.c`)

```json
{"cmd":"crawl_news",           "limit":50}
{"cmd":"get_news_digest",      "max_chars":32000,"days":7}
{"cmd":"news_jump_status",     "symbol":"NVDA"}
{"cmd":"heston_path_bundle",   "symbol":"NVDA"}
{"cmd":"heston_surface",       "symbol":"NVDA"}
{"cmd":"heston_diagnostics",   "symbol":"NVDA","n_paths":4000}
{"cmd":"bates_backtest_run",   "symbols":["NVDA","AAPL","MSFT"],"horizon_days":21,"n_strikes":7,"expiries_days":[7,30,90],"n_paths":256,"noise_sigma_vol":0.02}
{"cmd":"option_ranking_blend", "run_id":42,"w_edge":0.60,"w_news":0.27,"w_convex":0.13}
```

### Backend → client events

```json
{"type":"quote",              "symbol":"AAPL","price":150.25,"bid":150.20,"ask":150.30,"volume":123456,"ts":1700000000000}
{"type":"history",            "symbol":"AAPL","bars":[{"t":...,"o":...,"h":...,"l":...,"c":...,"v":...}]}
{"type":"portfolio",          "holdings":[{"symbol":"AAPL","shares":10,"avg_price":150.0,"current":161.5}]}
{"type":"news_digest",        "days":7,"text":"..."}
{"type":"news_jump_status",   "symbol":"NVDA","n_articles":12,"sentiment_avg":-0.31,"event_class":"guide","lam_bump":3.0,"mu_j_bias":-0.06,"sigma_j_bump":0.04}
{"type":"heston_path_bundle", "symbol":"NVDA","paths":[[...], ...]}
{"type":"heston_surface",     "symbol":"NVDA","grid":[[...], ...]}
{"type":"bates_backtest",     "run_id":42,"rows":[{"symbol":"NVDA","strike":900,"dte_days":30,"right":"C","model_iv":0.48,"market_iv":0.52,"edge_vol_pts":0.04,"expected_hedged_pnl":18.2,"sharpe_daily":0.31,"signed_edge":1}, ...],"summary":{...}}
{"type":"option_ranking",     "run_id":42,"ranking":[{"symbol":"NVDA","strike":900,"dte_days":30,"right":"C","z_bates_edge":1.42,"z_news_jump":0.88,"z_convexity":0.61,"blended_option_score":1.05,"rank":1}, ...]}
{"type":"error",              "message":"..."}
```

Poke the protocol manually with `nc localhost 8765`.

---

## How the Bates Pipeline Works

1. **Crawl news**: `crawler.c` pulls Polygon news headlines/descriptions into `news_cache`, keyed by ticker + timestamp.
2. **Compute per-symbol news signal**: `news_jump_recompute()` rolls up the crawl window (default 48h) into a `news_jump_signal` row per symbol: article count, average sentiment, event class (`earn`, `guide`, `down`, `up`, `ma`, `lit`, `macro`, `beat`, `buyback`), and the three jump-parameter deltas (`lam_bump`, `mu_j_bias`, `sigma_j_bump`).
3. **Calibrate Bates**: `heston.c` fits Bates SV+jump parameters (`κ`, `θ`, `σ_v`, `ρ`, `v₀`, `λ`, `μ_j`, `σ_j`) to the underlying's ~180 daily bars. This is the **market-aligned** Bates.
4. **Overlay news**: `news_jump_apply()` mutates the calibrated params into the **news-influenced** Bates: `p->lam += lam_bump; p->mu_j += mu_j_bias; p->sigma_j += sigma_j_bump` (zero-clipped on `sigma_j`).
5. **Price the option grid**: `bates_backtest.c` walks a (symbol × strike × expiry × right) grid. Each option is priced via the Lewis (2001) Fourier integral against the Bates characteristic function, then inverted to a Black-Scholes-equivalent `model_iv`.
6. **Synthesize a market IV**: `market_iv = model_iv + N(0, noise_sigma_vol) + skew(K/S, T)`, deterministic per (run_id, symbol, K, T). The option data is fake by design; this is a math sanity check for the pipeline, not a live trading number. See `Honest Limitations`.
7. **Delta-hedge simulator**: for each option, simulate `n_paths` forward Bates paths under the calibrated dynamics; along each path, rebalance a Black-Scholes delta hedge daily at the frozen `market_iv`. Record hedged P&L. For fair edge, `E[P&L] ≈ 0.5 · Γ · S² · (σ_impl² − σ_realized²) · Δt`, and `sign(edge_vol_pts)` should predict `sign(P&L)`.
8. **Blend into a ranking**: `option_score.c` z-scores the Bates edge (per expiry bucket), the news-jump scalar (per universe), and gamma/vega convexity (per expiry bucket), and fuses them with the default 0.60 / 0.27 / 0.13 weights. Output is sorted descending by `blended_option_score` with `rank` filled: the final **Q vs P ranking**.

The React **Option Ranking** panel is the terminal view; click a row to open Tick Evaluation for the underlying and inspect the MC fan chart, IV surface, and Heston diagnostics that produced the edge.

---

## Honest Limitations

- **Option data is synthesized**: `market_iv` is `model_iv + noise + smile skew`. This exists to validate the math (P&L sign should track edge sign), not to make trading claims. Wiring a real option chain (E\*TRADE, Polygon options) is the next step.
- **Survivorship bias**: the S&P 500 list is the *current* snapshot, not point-in-time.
- **Heston/Bates calibration** uses a bounded parameter search, not a full-optimizer surface fit. Fine for ranking, not production-grade for exotics pricing.
- **News-jump keyword mapping is hand-tuned**: `news_jump.c` uses a small keyword table. An optional local-vLLM sentiment layer is gated behind `QUORUM_NEWS_LLM=1` and contributes an extra `+0.03 · sentiment` to `mu_j_bias`; the keyword `event_class` stays authoritative.
- **Transaction cost / bid-ask / market impact** are not modeled in the hedged-P&L path.
- **Risk-free rate is a single scalar** passed at run start.

See `NOTES.md` for the full design rationale and decisions log.

---

## Tech Stack

| Component | Technology |
|---|---|
| Backend | C11, OpenSSL, libcurl, libpq, pthreads |
| Database | PostgreSQL 17 |
| Bridge | Node.js 18+, `ws` |
| Frontend | React 18, TypeScript, Vite 5, Zustand, Tailwind CSS, lightweight-charts |
