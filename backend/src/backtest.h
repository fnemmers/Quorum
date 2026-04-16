#ifndef BACKTEST_H
#define BACKTEST_H

#include "market_data.h"
#include <stdint.h>

/*
 * backtest  –  Simulates an equal-weighted portfolio of N tickers held for
 *              `hold_days` calendar days starting from `start_date`, and
 *              reports performance metrics vs. a benchmark (SPY).
 *
 * All price lookups go through db_cache_load(), so price_cache must be
 * populated for the relevant tickers before calling this. The caller is
 * responsible for backfilling missing data first.
 *
 * Date format: ISO "YYYY-MM-DD". Internally converted to Unix ms.
 *
 * Methodology (the version you're going to implement):
 *   1. For each ticker, look up the adjusted close on `start_date` (entry)
 *      and on `start_date + hold_days` (exit). Skip tickers missing data.
 *   2. Per-ticker return = (exit - entry) / entry.
 *   3. Portfolio return  = mean of per-ticker returns (equal weight).
 *   4. Compare to SPY return over the same window.
 *   5. Compute Sharpe over the *daily returns* of the equal-weighted
 *      portfolio across the hold window (you'll need daily bars, not
 *      just entry/exit).
 *   6. Max drawdown = largest peak-to-trough decline of the daily
 *      cumulative portfolio value.
 *
 * Honest accounting:
 *   - Subtract a flat transaction cost (default 0.2% round-trip) from
 *     the total return so the result isn't fictional.
 *   - Skip tickers that didn't exist on start_date (no entry price).
 *     Track them in `n_skipped` so the caller can warn the user.
 */

typedef struct BacktestResult {
    /* Inputs echoed back for clarity */
    int      n_tickers_requested;
    int      n_tickers_used;       /* after skipping missing data */
    int      n_skipped;
    int      hold_days;
    char     start_date[16];       /* "YYYY-MM-DD" */
    char     end_date[16];

    /* Performance vs benchmark */
    double   portfolio_return_pct; /* total return over the window, % */
    double   benchmark_return_pct; /* SPY return over same window, % */
    double   alpha_pct;            /* portfolio - benchmark */

    /* Risk-adjusted metrics */
    double   sharpe_ratio;         /* annualized, rf = 0 for now */
    double   max_drawdown_pct;     /* worst peak-to-trough, % (negative) */
    double   hit_rate_pct;         /* % of tickers that finished positive */

    /* Bookkeeping */
    double   transaction_cost_pct; /* applied (default 0.2) */
} BacktestResult;

/*
 * Run the backtest. Returns 0 on success, -1 on fatal error
 * (e.g. price_cache empty for SPY, invalid date).
 *
 * `tickers` is a flat array of NUL-terminated strings, length n_tickers.
 *
 * If your aggregator gives you `AggResult[]` instead, just pass
 * &agg_result[i].symbol via a small adapter loop.
 */
int backtest_run(const char *const *tickers,
                 int n_tickers,
                 const char *start_date,    /* "YYYY-MM-DD" */
                 int hold_days,
                 BacktestResult *out);

/*
 * Helper: parse "YYYY-MM-DD" into Unix milliseconds (UTC midnight).
 * Returns -1 on invalid input.
 *
 * (You can implement this in backtest.c or share it via a util module.)
 */
int64_t backtest_parse_date_ms(const char *yyyy_mm_dd);

/*
 * Helper: format a Unix-ms timestamp back into "YYYY-MM-DD" in `out`
 * (>= 11 bytes). Returns 0 on success.
 */
int backtest_format_date(int64_t ms, char *out);

#endif /* BACKTEST_H */
