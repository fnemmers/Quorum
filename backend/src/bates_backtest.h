#ifndef BATES_BACKTEST_H
#define BATES_BACKTEST_H

#include "market_data.h"   /* MAX_SYMBOL_LEN */
#include <stdint.h>

/*
 * bates_backtest  --  Mathematical, market-maker-style backtest.
 *
 * For every (symbol, strike, expiry, right) in a grid:
 *   1. Calibrate a Bates model to the symbol's ~180 daily bars,
 *      apply the news-jump overlay, and price the option via the
 *      Lewis (2001) Fourier integral against the Bates CF.
 *   2. Invert to a Black-Scholes model IV.
 *   3. Generate a synthetic market IV:
 *        market_iv = model_iv + N(0, noise_sigma_vol) + skew(K/S, T)
 *      Deterministic per (run_id, symbol, K, T) via seeded RNG.
 *   4. Simulate `n_paths` forward Bates paths under the calibrated
 *      dynamics; along each path, rebalance a Black-Scholes delta hedge
 *      daily at the frozen `market_iv`. Record hedged P&L.
 *   5. Report per-option: model_iv, market_iv, edge_vol_pts,
 *      expected & std of hedged P&L, daily Sharpe.
 *
 * The point isn't a live trading number — the option data is fake.
 * It's a math sanity check: for a fair edge, delta-hedged P&L should be
 * ≈ 0.5 * gamma * S^2 * (implied_var - realized_var) * dt, and the sign
 * of edge_vol_pts should predict the sign of the P&L.
 */

typedef struct {
    char    symbol[MAX_SYMBOL_LEN];
    double  strike;
    int     dte_days;
    char    right;                  /* 'C' (call) or 'P' (put) */
    double  spot;

    double  model_iv;
    double  market_iv;
    double  edge_vol_pts;           /* market_iv - model_iv */
    double  premium;                /* mid at market_iv, t=0 */

    double  delta0;                 /* BS delta at (spot, market_iv) */
    double  gamma0;
    double  vega0;

    double  expected_hedged_pnl;
    double  std_hedged_pnl;
    double  sharpe_daily;
    int     n_paths;
    int     signed_edge;            /* +1 sell rich vol, -1 buy cheap */
    double  dollar_edge;            /* |delta0| * premium * edge_vol_pts
                                     * MM position-sizing view: signed
                                     * $ P&L per $1 of edge, per contract. */
} BatesBacktestRow;

typedef struct {
    int64_t run_id;
    int     n_rows;
    int     n_symbols;
    int     n_strikes;
    int     n_expiries;
    int     n_paths;
    int     horizon_days;
    double  noise_sigma_vol;
    double  r;

    double  mean_edge_vol_pts;      /* over all rows */
    double  std_edge_vol_pts;
    double  hit_rate_edge_gt_1vol;  /* frac(|edge| > 0.01) */
    double  avg_hedged_pnl;
    double  avg_sharpe_daily;
} BatesBacktestSummary;

/*
 * Run the backtest across a user-provided symbol universe.
 *
 *   symbols         array of ticker C-strings (each ≤ MAX_SYMBOL_LEN)
 *   n_symbols       length of symbols
 *   session_id      for logging & seed derivation (any int64, use time
 *                   or 0 if you don't care)
 *   horizon_days    hedge horizon; also determines MC step count
 *   moneyness_lo/hi log-spaced K/S bounds
 *   n_strikes       strikes per symbol
 *   expiries_days   pointer to array of DTE values, e.g. {7, 30, 90}
 *   n_expiries      length of expiries_days
 *   n_paths         MC bundle per option, e.g. 256
 *   noise_sigma_vol Gaussian sigma for synth market IV, in vol pts
 *   seed            RNG seed
 *   r               risk-free rate (annualized, decimal)
 *
 * Allocates *out_rows with malloc; caller must free().
 * Fills *out_n and *out_summary.
 * Returns 0 on success, -1 on error.
 */
int bates_backtest_run(const char *const *symbols, int n_symbols,
                       int64_t  session_id,
                       int      horizon_days,
                       double   moneyness_lo,
                       double   moneyness_hi,
                       int      n_strikes,
                       const int *expiries_days,
                       int      n_expiries,
                       int      n_paths,
                       double   noise_sigma_vol,
                       uint64_t seed,
                       double   r,
                       BatesBacktestRow **out_rows,
                       int      *out_n,
                       BatesBacktestSummary *out_summary);

#endif /* BATES_BACKTEST_H */
