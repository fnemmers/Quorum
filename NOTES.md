# stock-app — design notes

Living document. Update this with your own decisions and rationale as the
project evolves. Future-you (and any interviewer asking "why did you do it
this way") will thank you.

---

## Project identity (one-liner)

A multi-language stock research system: a TypeScript ensemble of LLM-driven
"analyst bots" generates independent stock picks, a C analytics backend
aggregates them and backtests the consensus against historical S&P 500 data,
results displayed in a React frontend. Built around a custom newline-JSON
TCP IPC protocol on port 8765.

## Architecture

```
┌──────────────────┐    HTTP/WS    ┌─────────────────────┐
│  React frontend  │◄──────────────►  bridge (Node)      │
│  (sliders, UI)   │                │  (existing)         │
└──────────────────┘                └─────────┬───────────┘
                                              │ TCP 8765
                                              │ newline JSON
┌──────────────────┐                          ▼
│  TS bot system   │  TCP 8765    ┌─────────────────────┐
│  (Pass 2)        │◄────────────►│  C backend           │
│  - crawler       │              │  - ipc_server.c      │
│  - personas      │              │  - ipc_research.c   ◄┐
│  - orchestrator  │              │  - aggregation.c   ◄─┤ YOU
│  - budget cap    │              │  - backtest.c      ◄─┤ WRITE
└──────────────────┘              │  - convergence.c   ◄─┘ THESE
                                  │  - bot_picks.c (DB)  │
                                  │  - sp500_universe.c  │
                                  │  - polygon_rest.c    │
                                  │  - db.c (Postgres)   │
                                  └──────────┬───────────┘
                                             │ libpq
                                             ▼
                                  ┌─────────────────────┐
                                  │  Postgres            │
                                  │  - price_cache       │
                                  │  - bot_runs          │
                                  │  - bot_picks         │
                                  │  - backtest_results  │
                                  │  - portfolio (legacy)│
                                  │  - alerts (legacy)   │
                                  └─────────────────────┘
```

## Pass 1 — what was added (this commit)

### New C files

| File                  | What it is                                          |
|-----------------------|-----------------------------------------------------|
| `aggregation.h/.c`    | **STUB** — hash-map ticker counter, top-K extractor |
| `backtest.h/.c`       | **STUB** — equal-weight backtest + Sharpe + DD      |
| `convergence.h/.c`    | **STUB** — Jaccard similarity for bot stop-rule     |
| `bot_picks.h/.c`      | Postgres CRUD for runs, picks, backtest results    |
| `sp500_universe.h/.c` | Static S&P 500 ticker list (~500 entries)          |
| `ipc_research.h/.c`   | New IPC commands (sp500_list, bot_run_*, etc.)     |

### Existing files modified

- `db.h` / `db.c` — added `db_get_conn()` accessor + new schema (bot_runs,
  bot_picks, backtest_results)
- `ipc_server.c` — unrecognized commands now fall through to
  `ipc_research_dispatch()`. Legacy portfolio/alert handlers untouched.
- `main.c` — db_init + ipc_research_init/cleanup + db_close on shutdown
- `market_data.h` — `MAX_SYMBOLS` 64 → 600 for the S&P 500 universe
- `Makefile` — new sources added to SRCS, `-lm` linker flag

## ⚠️ YOUR TURN — three files to write yourself

These are deliberately stubs. Each `.c` file has detailed comment-pointers
explaining the design — read them top to bottom before you start coding.

### 1. `backend/src/aggregation.c` (~200 lines)

The hash map at the heart of the bot ensemble. FNV-1a hash + open
addressing + linear probing. The pointer comments walk you through every
decision.

**Test plan**: write a `agg_test.c` scratch file that adds 50 picks for
"AAPL", 30 for "MSFT", 10 for "GOOG", calls `agg_top_k(_, _, 3)`, and
asserts the order. ~30 lines, run in 5 minutes.

### 2. `backend/src/backtest.c` (~250 lines)

Equal-weighted backtest using `db_cache_load()` for price lookups. Computes
total return, Sharpe (annualized), max drawdown, hit rate, alpha vs SPY.
Pointer comments cover edge cases.

**Test plan**: backfill 5 well-known tickers + SPY for ~Aug-Dec 2025 via
the existing `polygon_rest` path, then call `backtest_run` and sanity-check
against Yahoo Finance. **Do this BEFORE wiring up bots** — you need a
known-good ground truth.

### 3. `backend/src/convergence.c` (~30 lines real code)

Just two functions, one is a one-liner. Don't overthink it.

**Test plan**: Two AggResult arrays, eyeball the Jaccard ratio.

---

## What you'll need to set up before this builds

```bash
# Postgres (if not already)
brew install postgresql@17
brew services start postgresql@17
createdb stockapp

# Build
cd backend
make
./stock-backend YOUR_POLYGON_API_KEY
```

The build will succeed even with empty stub bodies — they return safe
defaults. The IPC commands will work end-to-end except they'll return
empty/zero results until you fill in the algos.

## How to test the IPC layer manually

```bash
# In one terminal:
./backend/stock-backend YOUR_POLYGON_KEY

# In another:
nc localhost 8765
{"cmd":"sp500_list"}
# → {"type":"sp500_list","tickers":["A","AAL",...]}

{"cmd":"bot_run_create","label":"manual-test","n_bots_target":2,"hold_days":30}
# → {"type":"bot_run_created","run_id":1}

{"cmd":"bot_picks_ingest","run_id":1,"bot_index":0,"persona":"value","picks":["AAPL","MSFT","NVDA"]}
# → {"type":"bot_picks_ack","run_id":1,"bot_index":0,"n":3,"n_dropped":0}

{"cmd":"bot_picks_ingest","run_id":1,"bot_index":1,"persona":"momentum","picks":["NVDA","TSLA"]}
# → {"type":"bot_picks_ack",...}

{"cmd":"aggregate_run","run_id":1,"k":5}
# → empty `top` until aggregation.c is implemented; full results once it is
```

## Coming in Pass 2 — TypeScript bot system

After you've written the three `.c` files, ping me to start Pass 2:

- `bots/` directory at repo root
- Anthropic Haiku 4.5 client wrapper with prompt caching
- **Persistent monthly budget tracker — hard $50/month cap, refuses to run**
- 20 distinct personas (value/momentum/contrarian/sector specialists/...)
- Concurrency-controlled orchestrator (`p-limit`)
- IPC client streaming picks to the C backend
- CLI: `npm run bots -- --count 500 --window 30`

## Coming in Pass 3 — Crawler

Pre-summarized news/sector context fed to bots, using prompt caching so
500 bots reuse the same expensive base context for ~10x cost savings.

## Coming in Pass 4 — Frontend wiring

Sliders for `hold_days`, "run backtest" button, top-20 consensus display,
run history.

## Honest limitations to acknowledge in your project writeup

- **Survivorship bias**: the S&P 500 list is the *current* snapshot, not
  point-in-time. Backtests crossing constituent rebalances are slightly
  optimistic.
- **LLM training cutoff leakage**: Claude knows what happened to Tesla in
  2023 even if you tell it the date is 2020. Backtest only on data *after*
  the model's training cutoff for honest results. (For Haiku 4.5 that
  means mid-2025 onward — about 10 months of clean test data as of 2026-04.)
- **No real news context yet** — Pass 1 bots will only see price history +
  sector. Adding crawled news is Pass 3.
- **Transaction cost is a flat 0.2%** — doesn't model bid-ask spread,
  market impact, or taxes. Real returns would be lower.
- **Risk-free rate assumed 0** in the Sharpe calc.

These are *features* in your interview pitch, not bugs. They show you
understand what could be wrong with naive backtesting — most students
don't.

## Decisions log (start filling this in!)

- **2026-04-07** — Chose C for the analytics core (aggregation, backtest,
  Postgres path) and TypeScript for the bot orchestration + crawler.
  Rationale: C gives a strong systems story for resume, TS has the best
  ecosystem for HTTP/LLM/concurrency work and shares types with the React
  frontend.
- **2026-04-07** — Universe = S&P 500 (current snapshot). Smaller
  universes are too narrow; broader universes (Russell 1000, all US
  equities) require liquidity filters and more data infrastructure than
  is justified for v1.
- **2026-04-07** — Aggregator re-runs on every `aggregate_run` IPC call
  rather than maintaining live in-memory state. Trades a bit of CPU for
  statelessness — multiple clients can query the same run safely, and a
  backend restart doesn't lose progress (it's all in Postgres).
- *(your next decision here)*
