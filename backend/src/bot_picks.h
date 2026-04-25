#ifndef BOT_PICKS_H
#define BOT_PICKS_H

#include "market_data.h"
#include <stdint.h>

/*
 * bot_picks  –  Postgres persistence for bot ensemble runs.
 *
 * Three tables:
 *
 *   bot_runs       – one row per "run" (e.g. "500 bots, 30-day window, 2026-04-07")
 *   bot_picks      – one row per (run, bot, ticker) triple
 *   backtest_results – one row per backtest invocation (linked to a run)
 *
 * Schema is created in db.c db_init() — see the new CREATE TABLE block.
 *
 * Why store every individual pick (not just the aggregated top-20)?
 *   - Lets you re-aggregate later with different rules
 *   - Lets you analyze WHICH personas drove consensus picks
 *     ("the value bots all picked X, the momentum bots all picked Y")
 *   - Enables reproducibility studies
 *   - It's cheap: 500 bots * 20 picks = 10k rows per run
 */

/* ── Run lifecycle ─────────────────────────────────────────────── */

/*
 * Create a new run row, return its primary key (run_id).
 * `label` is a human-readable name ("nightly-2026-04-07").
 * `n_bots_target` is how many bots the orchestrator plans to spawn.
 * `hold_days` is the slider value used for any backtest of this run.
 */
int64_t bot_run_create(const char *label,
                       int n_bots_target,
                       int hold_days);

/*
 * Mark a run as completed. `n_bots_actual` is how many bots actually
 * finished (may be < target if convergence stopped it early or budget
 * was hit).
 */
int bot_run_finish(int64_t run_id, int n_bots_actual);

/* ── Run listing ───────────────────────────────────────────────── */

#define BOT_RUN_LABEL_LEN 128

typedef struct BotRunInfo {
    int64_t id;
    char    label[BOT_RUN_LABEL_LEN];
    int     n_bots_target;
    int     n_bots_actual;   /* 0 if never finished */
    int     hold_days;
    int64_t started_at;      /* unix ms */
    int64_t finished_at;     /* unix ms, 0 if not finished */
} BotRunInfo;

/*
 * Fetch up to `max_rows` most recent bot_runs (ORDER BY id DESC), newest
 * first. Returns the number of rows filled, or -1 on error.
 */
int bot_runs_list(BotRunInfo *out, int max_rows);

/* ── Pick ingestion ────────────────────────────────────────────── */

/*
 * Insert one bot's full set of picks for a given run.
 * `picks` is an array of n_picks NUL-terminated tickers.
 * `bot_index` is the orchestrator's local bot id (0 .. n_bots-1).
 * `persona` is the strategy name ("value", "momentum", etc.).
 *
 * Uses a single transaction for the batch.
 * Returns 0 on success.
 */
int bot_picks_insert_batch(int64_t run_id,
                           int bot_index,
                           const char *persona,
                           const char *const *picks,
                           int n_picks);

/*
 * Load every pick from a run into the caller-supplied buffer of tickers.
 * The buffer is filled with raw symbols only (one entry per pick row),
 * suitable for feeding into agg_add_pick() in a loop.
 *
 * `out` should be a flat char[][MAX_SYMBOL_LEN] sized for max_picks.
 * Returns the number of picks loaded, or -1 on error.
 */
int bot_picks_load_run(int64_t run_id,
                       char (*out)[MAX_SYMBOL_LEN],
                       int max_picks);

/* ── Backtest results ──────────────────────────────────────────── */

/*
 * Persist a BacktestResult row tied to a run.
 * Returns the result row id, or -1 on error.
 *
 * Defined as a forward decl to avoid pulling backtest.h into this header.
 */
struct BacktestResult;
int64_t bot_backtest_save(int64_t run_id,
                          const struct BacktestResult *r);

#endif /* BOT_PICKS_H */
