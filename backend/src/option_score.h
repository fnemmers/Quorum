#ifndef OPTION_SCORE_H
#define OPTION_SCORE_H

#include "bates_backtest.h"
#include <stdint.h>

/*
 * option_score  --  Cross-normalized z-score fusion over the Bates
 *                   backtest grid, producing a single sortable option
 *                   ranking.
 *
 * For every row produced by bates_backtest_run() we compute three
 * z-scores, then blend them with configurable weights:
 *
 *   blended_option_score
 *      = w_edge   · z_bates_edge     (per-expiry-bucket normalization)
 *      + w_news   · z_news_jump      (per-universe normalization)
 *      + w_convex · z_convexity      (per-expiry-bucket: gamma/vega)
 *
 * z_bates_edge captures the Q vs P gap from the whiteboard: market IV
 * minus Bates-implied model IV, standardized within a maturity bucket
 * so 7-DTE options don't dominate the ranking on raw vol scale.
 *
 * z_news_jump captures news-driven tail updates on the underlying,
 * z_convexity is the gamma-per-vega premium efficiency.
 *
 * Sign convention preserved: blended_option_score is signed
 * (long-cheap-vol vs sell-rich-vol via row->signed_edge). The
 * downstream consumer interprets |blended_option_score| as conviction.
 */

typedef struct {
    /* Copy of the underlying Bates row (denormalized for the JSON
     * response — the frontend gets everything it needs in one table). */
    BatesBacktestRow bates;

    /* Component z-scores. */
    double z_bates_edge;
    double z_news_jump;
    double z_convexity;

    /* Fused signal. */
    double blended_option_score;

    /* Rank position (1-indexed) after sorting DESC by
     * blended_option_score. Populated by option_score_run(). */
    int    rank;
} OptionScoredResult;

typedef struct {
    double w_edge;
    double w_news;
    double w_convex;
} OptionScoreWeights;

/*
 * Per-symbol scalar inputs the fusion needs but that live outside the
 * Bates backtest itself. Populate one row per underlying that appears
 * in the backtest rows.
 */
typedef struct {
    char   symbol[MAX_SYMBOL_LEN];
    /* News-jump aggregate scalar: lam_bump + 5 * |mu_j_bias|.
     * Zero if no signal row exists. */
    double news_scalar;
} PerSymbolFusionInput;

/*
 * Compute z-scores and blended_option_score for every row in the
 * Bates backtest output.
 *
 *   in_rows          array of Bates rows, in_n elements
 *   per_symbol       array of PerSymbolFusionInput, ps_n elements
 *   weights          fusion weights (pass NULL for defaults
 *                    0.60 / 0.27 / 0.13 — renormalized after the
 *                    bot component was retired)
 *   min_universe     if the run has fewer than this many symbols,
 *                    z_news_jump degrades gracefully (falls back to 0)
 *                    — per-universe normalization is unstable in
 *                    small universes.
 *   out_rows         malloc'd array, in_n elements, ranked DESC by
 *                    blended_option_score with `rank` field filled.
 *
 * Returns 0 on success, -1 on bad inputs / OOM.
 */
int option_score_run(const BatesBacktestRow *in_rows, int in_n,
                     const PerSymbolFusionInput *per_symbol, int ps_n,
                     const OptionScoreWeights *weights,
                     int min_universe,
                     OptionScoredResult **out_rows);

/* Default weights: w_edge=0.60, w_news=0.27, w_convex=0.13
 * (renormalized after the bot component was retired). */
void option_score_default_weights(OptionScoreWeights *w);

#endif /* OPTION_SCORE_H */
