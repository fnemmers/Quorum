# Quorum

**A market-maker-style option research platform.**

A C11 analytics backend calibrates a Bates stochastic-volatility-with-jumps model per underlying, overlays a news-driven jump signal, prices an option grid via the Lewis (2001) Fourier integral, and ranks the grid by the $\mathbb{Q}$-vs-$\mathbb{P}$ edge — the gap between Bates-implied model IV and market IV. A React frontend surfaces the ranking, Monte Carlo path bundles, implied-vol surfaces, and calibration diagnostics in real time.

> **Status (2026-08-21):** Seeking historical option chain data to properly fit Bates before live execution. Inquiries: `fnemmers@icloud.com`

---

## Table of Contents

- [Core Idea](#core-idea)
- [The Model](#the-model)
- [Pricing](#pricing)
- [The News-Jump Overlay](#the-news-jump-overlay)
- [Scoring and Ranking](#scoring-and-ranking)
- [Delta-Hedged P&L Validation](#delta-hedged-pl-validation)
- [Architecture](#architecture)
- [Project Structure](#project-structure)
- [Requirements](#requirements)
- [Setup](#setup)
- [Build and Run](#build-and-run)
- [IPC Protocol](#ipc-protocol)
- [Honest Limitations](#honest-limitations)

---

## Core Idea

The pipeline calibrates Bates **twice** and rates each contract by the disagreement between the two calibrations and the market.

| Variant | Input | Output |
|---|---|---|
| **Market-aligned Bates** | $\approx 180$ daily bars only. Pure historical calibration, no overlays. | $\sigma_{\mathrm{model}}$ reflecting the underlying's realized-vol and jump behavior over the calibration window. |
| **News-influenced Bates** | Same base calibration, then `news_jump_apply()` mutates $(\lambda, \mu_J, \sigma_J)$ from the freshest per-symbol news signal. | $\sigma_{\mathrm{model}}$ reflecting the same underlying *conditioned on the current news window* — earnings, guidance cuts, M&A, macro shocks. |

Each variant yields a Bates characteristic function; the Lewis integral prices the option; the price is inverted to a Black–Scholes-equivalent IV. The edge is the residual:

$$
e \;=\; \sigma_{\mathrm{mkt}} \;-\; \sigma_{\mathrm{model}}
$$

- **Market-aligned edge** asks: is this option mispriced relative to the underlying's own historical dynamics?
- **News-influenced edge** asks: is it mispriced *after* absorbing what today's news implies about the jump distribution?

---

## The Model

### Bates dynamics

Under the risk-neutral measure $\mathbb{Q}$, the underlying follows Heston stochastic volatility with Merton lognormal jumps:

$$
\frac{dS_t}{S_{t^-}} \;=\; \big(r - q - \lambda \bar{k}\big)\,dt \;+\; \sqrt{v_t}\,dW_t^{S} \;+\; \big(e^{J}-1\big)\,dN_t
$$

$$
dv_t \;=\; \kappa\big(\theta - v_t\big)\,dt \;+\; \sigma_v \sqrt{v_t}\,dW_t^{v}
$$

$$
d\langle W^{S}, W^{v}\rangle_t \;=\; \rho\,dt
$$

where $N_t$ is a Poisson process with intensity $\lambda$, jump sizes are $J \sim \mathcal{N}(\mu_J, \sigma_J^2)$, and the compensator

$$
\bar{k} \;=\; \mathbb{E}\big[e^{J}-1\big] \;=\; e^{\mu_J + \frac{1}{2}\sigma_J^2} - 1
$$

is subtracted from the drift so that $e^{-(r-q)t} S_t$ remains a martingale.

### Notation

| Symbol | Meaning |
|:---:|---|
| $\kappa$ | Mean-reversion speed of variance |
| $\theta$ | Long-run variance level |
| $\sigma_v$ | Volatility of variance |
| $\rho$ | Spot–variance correlation |
| $v_0$ | Initial variance |
| $\lambda$ | Jump intensity (jumps per year) |
| $\mu_J,\ \sigma_J$ | Mean and stdev of log jump size |
| $\bar{k}$ | Jump compensator |
| $\Gamma,\ \mathcal{V}$ | Gamma and vega |

The Feller condition $2\kappa\theta > \sigma_v^2$ keeps $v_t$ strictly positive; the calibrator enforces it as a bound.

### Characteristic function

The Bates CF factorizes into the Heston CF and an independent jump term:

$$
\phi_T(u) \;=\; \phi_T^{\mathrm{H}}(u)\cdot \exp\Big\{ \lambda T\Big( e^{\,iu\mu_J - \frac{1}{2}u^2\sigma_J^2} - 1 - iu\bar{k} \Big)\Big\}
$$

with

$$
\phi_T^{\mathrm{H}}(u) \;=\; \exp\Big\{ iu\big(\ln S_0 + (r-q)T\big) \;+\; \frac{\kappa\theta}{\sigma_v^{2}}\Big[(\kappa - i\rho\sigma_v u - d)T - 2\ln\frac{1 - g e^{-dT}}{1-g}\Big] \;+\; \frac{v_0}{\sigma_v^{2}}\,(\kappa - i\rho\sigma_v u - d)\,\frac{1 - e^{-dT}}{1 - g e^{-dT}} \Big\}
$$

$$
d \;=\; \sqrt{(\rho\sigma_v i u - \kappa)^2 + \sigma_v^2\,(iu + u^2)}, \qquad g \;=\; \frac{\kappa - i\rho\sigma_v u - d}{\kappa - i\rho\sigma_v u + d}
$$

The $(\kappa - i\rho\sigma_v u - d)$ branch is used rather than $(+d)$ to avoid the branch-cut discontinuity in the complex logarithm at long maturities.

---

## Pricing

### Lewis (2001) Fourier integral

For a European call with log-moneyness $k = \ln(S_0/K) + (r-q)T$:

$$
C(S_0, K, T) \;=\; S_0 e^{-qT} \;-\; \frac{\sqrt{S_0 K}\,e^{-rT}}{\pi}\int_0^{\infty} \mathrm{Re}\Big[ e^{\,iuk}\,\phi_T\!\Big(u - \tfrac{i}{2}\Big) \Big]\,\frac{du}{u^{2} + \tfrac{1}{4}}
$$

The $u^2 + \tfrac14$ denominator gives the integrand $O(u^{-2})$ decay, so a truncated Gauss–Legendre quadrature converges quickly without the oscillation problems of the original Heston two-integral formulation.

### Implied-vol inversion

The model price is inverted to a Black–Scholes-equivalent volatility by solving

$$
C_{\mathrm{BS}}\big(S_0, K, T, \sigma_{\mathrm{model}}\big) \;=\; C_{\mathrm{Bates}}\big(S_0, K, T\big)
$$

for $\sigma_{\mathrm{model}}$. Working in IV space rather than price space makes the edge comparable across strikes and maturities.

---

## The News-Jump Overlay

`crawler.c` pulls Polygon headlines into `news_cache`. `news_jump_recompute()` rolls a window (default $48$h) into one signal row per symbol: article count, mean sentiment, event class (`earn`, `guide`, `down`, `up`, `ma`, `lit`, `macro`, `beat`, `buyback`), and three parameter deltas.

`news_jump_apply()` then perturbs only the jump block of the calibrated parameter vector:

$$
\lambda' = \lambda + \Delta\lambda, \qquad \mu_J' = \mu_J + \delta_{\mu}, \qquad \sigma_J' = \max\big(0,\; \sigma_J + \Delta\sigma_J\big)
$$

The diffusive parameters $(\kappa, \theta, \sigma_v, \rho, v_0)$ are left untouched. The premise is that news moves the market's view of *jump* risk on a much faster timescale than it moves the diffusion.

---

## Scoring and Ranking

`option_score.c` fuses three standardized components into one sortable score:

$$
\mathcal{S} \;=\; w_e\, z_e \;+\; w_n\, z_n \;+\; w_c\, z_c, \qquad (w_e, w_n, w_c) = (0.60,\ 0.27,\ 0.13)
$$

| Component | Definition | Normalization |
|---|---|---|
| $z_e$ | Market-aligned $\mathbb{Q}$-vs-$\mathbb{P}$ edge $e$ | Per expiry bucket |
| $z_n$ | News-driven jump adjustment | Per universe |
| $z_c$ | Convexity $c = \Gamma / \mathcal{V}$ | Per expiry bucket |

Standardization is bucketed so that short-dated contracts do not dominate on raw vol scale:

$$
z_e^{(i)} \;=\; \frac{e^{(i)} - \bar{e}_{B(i)}}{s_{e,\,B(i)}}, \qquad B(i) = \text{expiry bucket of contract } i
$$

Direction is carried separately in `signed_edge` $\in \{+1, -1\}$ — sell rich vol, buy cheap vol — so the frontend reads $|\mathcal{S}|$ as conviction and the sign as side.

---

## Delta-Hedged P&L Validation

For each contract the backtester simulates $n$ forward Bates paths under the calibrated dynamics and rebalances a Black–Scholes delta hedge daily at the frozen $\sigma_{\mathrm{mkt}}$. The classical result for a continuously delta-hedged option is

$$
\Pi \;=\; \frac{1}{2}\int_0^{T} \Gamma_t\, S_t^{2}\,\big(\sigma_{\mathrm{impl}}^{2} - \sigma_{\mathrm{real}}^{2}\big)\,dt
$$

which the simulator discretizes as

$$
\Pi \;\approx\; \sum_{k=0}^{n_{\text{steps}}-1} \tfrac{1}{2}\,\Gamma_{t_k} S_{t_k}^{2}\big(\sigma_{\mathrm{impl}}^{2} - \sigma_{\mathrm{real}}^{2}\big)\,\Delta t
$$

The invariant under test is $\operatorname{sign}(e) = \operatorname{sign}(\mathbb{E}[\Pi])$: if the edge says vol is rich, the short-vol hedged position should make money on average. This is a **consistency check between the pricer and the hedging simulator**, not evidence of tradeable alpha — see [Honest Limitations](#honest-limitations).

---

## Architecture

```
┌────────────────────────┐      ┌──────────────────┐      ┌─────────────────────────┐
│  React (Vite + TS)     │ ws   │   Node Bridge    │ tcp  │   C11 Backend           │
│                        │◄────►│                  │◄────►│                         │
│  • Option ranking      │ 3001 │  WebSocket ↔ TCP │ 8765 │  • Polygon.io WS + REST │
│  • Bates backtest      │      │  multiplexer     │      │  • PostgreSQL           │
│  • News-jump panel     │      │                  │      │  • Bates calibration    │
│  • Tick Evaluation:    │      └──────────────────┘      │  • Lewis pricer         │
│      MC fan chart      │                                │  • News-jump overlay    │
│      IV surface        │                                │  • Delta-hedge sim      │
│      Diagnostics       │                                │  • Z-score fusion       │
└────────────────────────┘                                └─────────────────────────┘
```

| Layer | Technology | Role |
|---|---|---|
| Backend | C11, OpenSSL, libcurl, libpq, pthreads | Market data, persistence, IPC, calibration, pricing, backtest, scoring, crawler |
| Database | PostgreSQL 17 | Price cache, news cache, calibration and backtest rows |
| Bridge | Node.js 18+, `ws` | WebSocket-to-TCP multiplexer |
| Frontend | React 18, TypeScript, Vite 5, Zustand, Tailwind, lightweight-charts | Real-time research UI |

All IPC is newline-delimited JSON over TCP `:8765`.

---

## Project Structure

```
quorum/
├── backend/
│   ├── Makefile
│   └── src/
│       ├── main.c                  Entry point; subsystem init
│       ├── market_data.c/h         Global price state, thread-safe ring buffers
│       ├── polygon_ws.c/h          WebSocket client (real-time)
│       ├── polygon_rest.c/h        REST client (historical + news)
│       ├── ipc_server.c/h          TCP server; market & portfolio dispatch
│       ├── ipc_research.c/h        Bates, news-jump, option-score dispatch
│       ├── db.c/h                  PostgreSQL persistence
│       ├── heston.c/h              Bates/Heston MC path generator + CF
│       ├── heston_surface.c/h      IV surface fit / lookup
│       ├── bates_backtest.c/h      Grid pricer + delta-hedge P&L simulator
│       ├── news_jump.c/h           News → (Δλ, δ_μ, Δσ_J) overlay
│       ├── option_score.c/h        Z-score fusion → final ranking
│       ├── crawler.c/h             Polygon news crawler + per-ticker digest
│       ├── sp500_universe.c/h      Static S&P 500 universe
│       └── cJSON.c/h               Vendored JSON parser
├── bridge/
│   ├── bridge.js                   WebSocket ↔ TCP multiplexer
│   └── package.json
├── frontend-react/
│   ├── src/
│   │   ├── App.tsx
│   │   ├── store/useStore.ts
│   │   └── components/
│   │       ├── StatusBar.tsx
│   │       ├── OptionRankingPanel.tsx      Final blended ranking
│   │       ├── BatesBacktestPanel.tsx      Per-option backtest rows
│   │       ├── NewsJumpPanel.tsx           Per-symbol overlay status
│   │       ├── TickEvaluationPanel.tsx     Per-symbol workspace
│   │       ├── MCPathBundlePanel.tsx       MC fan chart
│   │       ├── HestonSurfacePanel.tsx      IV surface viewer
│   │       ├── HestonDiagnosticsPanel.tsx  Calibration diagnostics
│   │       └── Chart.tsx
│   ├── package.json
│   ├── tailwind.config.js
│   └── vite.config.ts
├── Makefile                        Top-level build; `make run` orchestrates
└── NOTES.md                        Design notes & decisions log
```

---

## Requirements

### C backend

| Dependency | macOS (Homebrew) | Ubuntu/Debian | Windows (MSYS2 MinGW64) |
|---|---|---|---|
| GCC ≥ 11 | `brew install gcc` | `apt install gcc` | `pacman -S mingw-w64-x86_64-gcc` |
| OpenSSL | `brew install openssl` | `apt install libssl-dev` | `pacman -S mingw-w64-x86_64-openssl` |
| libcurl | `brew install curl` | `apt install libcurl4-openssl-dev` | `pacman -S mingw-w64-x86_64-curl` |
| PostgreSQL (libpq) | `brew install postgresql@17` | `apt install libpq-dev` | `pacman -S mingw-w64-x86_64-postgresql` |
| pthreads | included | included | included |

### Bridge and frontend

Node.js 18+, npm 9+.

---

## Setup

**1. API keys.** Sign up at [polygon.io](https://polygon.io). The free tier gives 15-minute delayed quotes, enough to drive the crawler and calibration. E\*TRADE sandbox/production key pairs are read from `.env` for future live execution (not yet wired).

**2. Vendor cJSON** (only if missing — it is committed under `backend/src/`):

```bash
cd backend/src
curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h
curl -LO https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c
```

**3. PostgreSQL:**

```bash
createdb stockapp
```

Tables are auto-created on first connection: `price_cache`, `portfolio`, `alerts`, `order_history`, `news_cache`, `news_jump_signal`, `bates_backtest_rows`, `option_score_rows`, and the Heston calibration tables.

**4. Create `.env` at the repo root:**

```ini
POLYGON_API_KEY=your_polygon_key_here
DB_PASSWORD=postgres

# Optional: E*TRADE (not yet wired for live orders)
# ETRADE_SANDBOX_KEY=...
# ETRADE_SANDBOX_SECRET=...
# ETRADE_LIVE_KEY=...
# ETRADE_LIVE_SECRET=...
```

**5. Install:**

```bash
make              # backend + bridge/frontend deps
make backend      # C backend only
make bridge       # Node bridge deps
make frontend     # React production bundle
```

---

## Build and Run

```bash
make run          # backend + bridge + frontend dev server; Ctrl-C stops all three
```

Or component-by-component in separate terminals:

```bash
# C backend — IPC server on :8765, DB_PASSWORD defaults to "postgres"
cd backend && ./quorum-backend YOUR_POLYGON_API_KEY [DB_PASSWORD]

# Node bridge — connects to :8765, serves WebSocket on :3001
cd bridge && node bridge.js

# React frontend — Vite dev server, connects to bridge on :3001
cd frontend-react && npm run dev
```

```bash
make clean        # removes C objects, frontend dist/node_modules, bridge node_modules
```

---

## IPC Protocol

Newline-delimited JSON over TCP `:8765`. Poke it manually with `nc localhost 8765`.

### Market data and user state — `ipc_server.c`

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

### Research — `ipc_research.c`

```json
{"cmd":"crawl_news",           "limit":50}
{"cmd":"get_news_digest",      "max_chars":32000,"days":7}
{"cmd":"news_jump_status",     "symbol":"NVDA"}
{"cmd":"heston_path_bundle",   "symbol":"NVDA"}
{"cmd":"heston_surface",       "symbol":"NVDA"}
{"cmd":"heston_diagnostics",   "symbol":"NVDA","n_paths":4000}
{"cmd":"bates_backtest_run",   "symbols":["NVDA","AAPL","MSFT"],"horizon_days":21,"n_strikes":7,
                               "expiries_days":[7,30,90],"n_paths":256,"noise_sigma_vol":0.02}
{"cmd":"option_ranking_blend", "run_id":42,"w_edge":0.60,"w_news":0.27,"w_convex":0.13}
```

### Backend → client events

```json
{"type":"quote",              "symbol":"AAPL","price":150.25,"bid":150.20,"ask":150.30,"volume":123456,"ts":1700000000000}
{"type":"history",            "symbol":"AAPL","bars":[{"t":"...","o":"...","h":"...","l":"...","c":"...","v":"..."}]}
{"type":"portfolio",          "holdings":[{"symbol":"AAPL","shares":10,"avg_price":150.0,"current":161.5}]}
{"type":"news_digest",        "days":7,"text":"..."}
{"type":"news_jump_status",   "symbol":"NVDA","n_articles":12,"sentiment_avg":-0.31,"event_class":"guide",
                              "lam_bump":3.0,"mu_j_bias":-0.06,"sigma_j_bump":0.04}
{"type":"heston_path_bundle", "symbol":"NVDA","paths":[["..."]]}
{"type":"heston_surface",     "symbol":"NVDA","grid":[["..."]]}
{"type":"bates_backtest",     "run_id":42,"rows":[{"symbol":"NVDA","strike":900,"dte_days":30,"right":"C",
                              "model_iv":0.48,"market_iv":0.52,"edge_vol_pts":0.04,
                              "expected_hedged_pnl":18.2,"sharpe_daily":0.31,"signed_edge":1}]}
{"type":"option_ranking",     "run_id":42,"ranking":[{"symbol":"NVDA","strike":900,"dte_days":30,"right":"C",
                              "z_bates_edge":1.42,"z_news_jump":0.88,"z_convexity":0.61,
                              "blended_option_score":1.05,"rank":1}]}
{"type":"error",              "message":"..."}
```

---

## Honest Limitations

**Option data is synthesized.** Market IV is constructed, not observed:

$$
\sigma_{\mathrm{mkt}} \;=\; \sigma_{\mathrm{model}} \;+\; \varepsilon \;+\; s\!\left(K/S_0,\, T\right), \qquad \varepsilon \sim \mathcal{N}\!\left(0, \sigma_{\mathrm{noise}}^{2}\right)
$$

deterministic per `(run_id, symbol, K, T)`. This makes the edge *exactly* $e = \varepsilon + s(K/S_0, T)$ by construction. The delta-hedge sign test therefore validates internal consistency between the pricer and the hedging simulator — it does not demonstrate alpha, because there is no independent market to be right about. **Wiring a real option chain is the next step and the blocking one.**

**Survivorship bias.** The S&P 500 universe is the current snapshot, not point-in-time.

**Calibration is a bounded parameter search**, not a full-surface optimizer fit. Adequate for cross-sectional ranking; not production-grade for exotics.

**News-jump keyword mapping is hand-tuned.** `news_jump.c` uses a small keyword table. An optional local-vLLM sentiment layer behind `QUORUM_NEWS_LLM=1` contributes an extra $0.03 \cdot \mathrm{sentiment}$ to $\delta_\mu$; the keyword `event_class` remains authoritative.

**Transaction costs, bid-ask spread, and market impact** are not modeled in the hedged-P&L path.

**Risk-free rate** is a single scalar passed at run start; no term structure.

See `NOTES.md` for the full design rationale and decisions log.
