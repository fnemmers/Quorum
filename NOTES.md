# quorum: design notes

Living document. Update this with rationale as the project evolves. Future-you (and any interviewer asking "why did you build it this way") will thank you.

---

## Current identity (one-liner)

A market-maker-style option research platform. A C11 backend calibrates a Bates stochastic-vol-with-jumps model per underlying, applies a news-driven overlay to the jump parameters, prices an option grid via the Lewis (2001) Fourier integral, and ranks the grid by the gap between the Bates-implied model IV and a synthesized market IV. React frontend surfaces the ranking, MC path bundles, IV surfaces, and Bates diagnostics in real time. Custom newline-JSON TCP IPC on port 8765.

## Current architecture

```
┌──────────────────┐    WS 3001     ┌─────────────────────┐
│  React frontend  │◄──────────────►│  bridge (Node)      │
│  (panels + UI)   │                │                     │
└──────────────────┘                └─────────┬───────────┘
                                              │ TCP 8765
                                              │ newline JSON
                                              ▼
                                  ┌─────────────────────┐
                                  │  C backend           │
                                  │  - ipc_server.c      │
                                  │  - ipc_research.c    │
                                  │  - heston.c (Bates)  │
                                  │  - heston_surface.c  │
                                  │  - bates_backtest.c  │
                                  │  - news_jump.c       │
                                  │  - option_score.c    │
                                  │  - crawler.c         │
                                  │  - sp500_universe.c  │
                                  │  - polygon_ws/rest.c │
                                  │  - db.c (Postgres)   │
                                  └──────────┬───────────┘
                                             │ libpq
                                             ▼
                                  ┌─────────────────────┐
                                  │  Postgres            │
                                  │  - price_cache       │
                                  │  - news_cache        │
                                  │  - news_jump_signal  │
                                  │  - bates_backtest_*  │
                                  │  - option_score_*    │
                                  │  - portfolio (legacy)│
                                  │  - alerts (legacy)   │
                                  └─────────────────────┘
```

---

## Evolution of the project

Three eras. The through-line is that the C backend, Polygon integration, Postgres persistence, IPC protocol, and React shell have been stable since day one. The *thing being computed* has changed twice.

### Era 1 (2026-04, "LLM ensemble")

**Thesis.** If 500 persona-diverse LLM analyst bots each independently pick a small basket from the S&P 500 universe, hash-count the picks, and take the top-K by vote share, the consensus should beat naive index selection. The pitch was "wisdom of the LLM crowd, priced against a real backtester."

**What was built.**
- C: `aggregation.c` (FNV-1a hash + open-addressed top-K), `backtest.c` (equal-weight, Sharpe, DD, hit rate, alpha vs SPY), `convergence.c` (Jaccard stability to decide when adding more bots stops changing consensus), `bot_picks.c` (Postgres CRUD for runs/picks), `sp500_universe.c`.
- Python `bots/`: `bot_runner.py` (persona bots against Anthropic Haiku, later swapped to a local vLLM server serving Qwen 2.5 14B AWQ once the API cost became untenable), `kseed_runner.py` (K parallel ensembles with cross-ensemble disagreement variance), `bot_runner_backtest.py` (historical-window variant with cached OHLCV), `backfill.py`.
- Frontend: `CompilationPanel`, `PortfolioPanel`, `AlertPanel`, `TradeBlotter`, `QuotePanel`, `ResearchPanel`, `RiskPanel`, `PaperTrailPanel`.

**Why it fell short.**
1. *LLM training-cutoff leakage.* The model "knows" what happened to a ticker even when you tell it the date is earlier. Honest backtests were confined to the sliver of time after the model's training cutoff, which shrank the usable evaluation window.
2. *No hard quantitative content.* For a quant internship resume artifact the story was "I called an LLM in a loop." The math surface area was FNV hashing and Sharpe ratios. Fine engineering, weak signal to a quant recruiter.
3. *Consensus was noisier than expected.* Even with `kseed_runner` and cross-ensemble disagreement scoring, top-K rankings were dominated by mega-caps the model saw every day. The convergence detector fired quickly but the converged set was uninteresting.

### Era 2 (2026-06 through 2026-08-17, "Heston risk overlay")

**Thesis.** Keep the LLM ensemble, but downweight its picks by a stochastic-vol risk score so overcrowded / high-vol names don't dominate. This is where the quant substance entered.

**What was added.**
- `heston.c`: Bates/Heston SV Monte Carlo path generator with the standard SV dynamics (`dS = μ S dt + √v S dW^S`, `dv = κ(θ - v) dt + σ √v dW^v`, `⟨dW^S, dW^v⟩ = ρ dt`).
- `heston_surface.c`: implied-vol surface fit / lookup.
- `risk_score.c`: per-symbol scalar collapsed from the MC path bundle.
- `rebalance.c`: combined the blended ranking (LLM vote share × risk score) with current holdings, GICS sector diversity, and the risk scalar to emit a concrete trade list.
- Frontend: `MCPathBundlePanel`, `HestonSurfacePanel`, `HestonDiagnosticsPanel`, `TickEvaluationPanel` as the per-symbol host.

**Why it still fell short.**
The Heston layer was doing real math, but structurally the LLM was still the primary signal and the SV model was a filter on top. The pipeline was two loosely-coupled halves. When explaining it, the LLM half undercut the quant half ("so the model that picks stocks is Qwen 14B, and then...").

### Era 3 (2026-08-18 onward, "Pure Bates + news-driven jumps")

**Thesis.** Drop the LLM stock-picker entirely. The signal is the *option itself*: calibrate Bates, price the whole grid, compare to a synthesized market IV, and rank by Q-vs-P edge. News becomes an input to the jump parameters, not a text prompt to an LLM.

**What changed on 2026-08-18.**
- **Deleted.** `aggregation.c/h`, `backtest.c/h`, `bot_picks.c/h`, `convergence.c/h`, `rebalance.c/h`, `risk_score.c/h`, entire `bots/` directory (bot_runner, kseed_runner, backfill, requirements), frontend panels tied to the ensemble (CompilationPanel, PortfolioPanel, AlertPanel, TradeBlotter, QuotePanel, ResearchPanel, RiskPanel, PaperTrailPanel).
- **Added.** `bates_backtest.c/h` (grid pricer via Lewis (2001) Fourier integral against the Bates CF + daily-rebalanced delta hedge simulator), `news_jump.c/h` (crawler rollup into per-symbol `(lam_bump, mu_j_bias, sigma_j_bump)` and an `apply()` that mutates a calibrated `HestonParams` in place), `option_score.c/h` (z-score fusion producing the final ranking), `BatesBacktestPanel.tsx`, `NewsJumpPanel.tsx`, `OptionRankingPanel.tsx`.
- **Kept.** All Polygon integration, Postgres, IPC, the Heston MC and surface code, and the Tick Evaluation host panel with its MC / surface / diagnostics children.
- **Kept but demoted.** Optional local-vLLM sentiment layer, gated behind `QUORUM_NEWS_LLM=1`. It contributes `+0.03 * sentiment` to `mu_j_bias` on top of the keyword result; the keyword-derived `event_class` stays authoritative. The LLM went from "the picker" to "a small adjustment to one jump parameter, off by default."

**Why this is the right story.** The pipeline now has a coherent single thesis: "market-maker-style Q-vs-P option ranking, with news as a jump-parameter overlay." Every component contributes math. The interview pitch is "I built a Bates calibrator, a Lewis Fourier pricer, a delta-hedge P&L simulator, and a cross-sectional z-score fusion, wired to a live news feed."

---

## Decisions log

- **2026-04-07.** Chose C for the analytics core and TypeScript/Python for orchestration. Rationale: C gives a strong systems story for a resume; TS/Python own the HTTP/LLM/concurrency work. *Still valid in Era 3 (the Python layer is gone but the C/JS/TS split remains).*
- **2026-04-07.** Universe = S&P 500 (current snapshot). Smaller universes too narrow; broader universes need liquidity filters and more data infrastructure than v1 justifies. *Still valid.*
- **2026-04-07.** Aggregator re-runs on every `aggregate_run` IPC call rather than maintaining live in-memory state. *Superseded 2026-08-18: aggregator deleted.*
- **2026-06.** Added Heston MC + IV surface + per-symbol risk score. Rationale: raise the quant surface area of the project before internship applications open.
- **2026-08-15.** Realized the LLM half was undercutting the quant half in every explanation. Started scoping the pivot.
- **2026-08-18.** Pivot committed. Removed LLM ensemble, aggregation, convergence, rebalance, risk_score, and the Python `bots/` tree. Added Bates backtest grid, news-jump overlay, and option-score fusion. The project is now pure Bates + news-driven jumps + option ranking.
- **2026-08-19.** SEC/EDGAR treated as permanently off-limits after an IP ban two months prior. All news must come through Polygon or a licensed vendor; no scraping.
- **2026-08-20.** README rewritten around the Q-vs-P framing (`bates_backtest` + `option_score`). Kept the E\*TRADE key slots in `.env` for future live execution but the code path is not wired.
- *(next decision here)*

---

## Honest limitations (current)

- **Option data is synthesized.** `market_iv = model_iv + N(0, noise_sigma_vol) + skew(K/S, T)`, deterministic per `(run_id, symbol, K, T)`. This is a math sanity check for the pipeline, not a live trading number. For a fair edge, `E[hedged P&L] ≈ 0.5 * Γ * S² * (σ_impl² - σ_realized²) * Δt` and the sign of `edge_vol_pts` should predict the sign of the P&L. Wiring a real option chain (E\*TRADE, Polygon options) is the next real-money step.
- **Survivorship bias.** The S&P 500 list is the *current* snapshot, not point-in-time.
- **Heston/Bates calibration** uses a bounded parameter search, not a full-optimizer surface fit. Fine for ranking, not production-grade for exotics.
- **News-jump keyword mapping is hand-tuned.** `news_jump.c` uses a small keyword table with priorities. Optional local-vLLM sentiment gated behind `QUORUM_NEWS_LLM=1`.
- **Transaction cost / bid-ask / market impact** are not modeled in the hedged-P&L path.
- **Risk-free rate** is a single scalar passed at run start.

These are *features* in the interview pitch, not bugs. They show awareness of what naive backtesting gets wrong.

---

## Prior plans (archived)

Kept for context on how the project used to be scoped. Everything in this section describes the Era 1 and Era 2 build and is no longer live.

### Era 1 architecture diagram (as it was in April 2026)

```
┌──────────────────┐    HTTP/WS    ┌─────────────────────┐
│  React frontend  │◄──────────────►  bridge (Node)      │
│  (sliders, UI)   │                │  (existing)         │
└──────────────────┘                └─────────┬───────────┘
                                              │ TCP 8765
┌──────────────────┐                          ▼
│  TS bot system   │  TCP 8765    ┌─────────────────────┐
│  (Pass 2)        │◄────────────►│  C backend           │
│  - crawler       │              │  - aggregation.c    │
│  - personas      │              │  - backtest.c       │
│  - orchestrator  │              │  - convergence.c    │
│  - budget cap    │              │  - bot_picks.c (DB) │
└──────────────────┘              └─────────────────────┘
```

### Era 1 pass plan

- **Pass 1.** C stubs (`aggregation`, `backtest`, `convergence`, `bot_picks`, `sp500_universe`, `ipc_research`). Done April 2026.
- **Pass 2.** TypeScript bot system: Anthropic Haiku client with prompt caching, persistent monthly $50 budget cap, 20 personas, `p-limit` concurrency, IPC streaming. *Later replaced with Python + local vLLM after cost pressure. Removed entirely 2026-08-18.*
- **Pass 3.** Crawler pre-summarizing news/sector context, prompt-cached so 500 bots reuse the same base context for ~10x savings. *Crawler survived; the prompt-caching path is gone. News now feeds `news_jump.c`, not an LLM prompt.*
- **Pass 4.** Frontend wiring: sliders for `hold_days`, "run backtest" button, top-20 consensus display, run history. *Panels deleted 2026-08-18 and replaced with `OptionRankingPanel`, `BatesBacktestPanel`, `NewsJumpPanel`.*

### Era 2 additions (Heston risk overlay)

- `heston.c/h`, `heston_surface.c/h`, `risk_score.c/h`, `rebalance.c/h`.
- IPC commands `heston_score_run`, `ranking_blend`, `rebalance_check`, `rebalance_resolve`. *All deleted 2026-08-18 except the Heston MC / surface / diagnostics commands, which survive under `bates_backtest_run` / `option_ranking_blend` / `heston_*` today.*

### Era 1 limitations (for reference; several are now moot)

- **LLM training-cutoff leakage.** The root cause of the pivot. No longer relevant in Era 3 because there is no LLM stock picker.
- **No real news context** in Pass 1 bots. Resolved in Pass 3, then generalized in Era 3.
- **Transaction cost flat 0.2%.** Still an honest limitation, but the current pipeline models delta-hedged P&L rather than a strategy return, so this specific number no longer applies.
