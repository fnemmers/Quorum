#ifndef RISK_SCORE_H
#define RISK_SCORE_H

#include "aggregation.h"   /* AggResult */
#include "heston.h"        /* HestonScore */
#include "market_data.h"   /* MAX_SYMBOL_LEN */

/*
 * risk_score  -  Blends bot consensus + Heston risk-adjusted score into
 *                one sortable float per ticker, then qsorts the array.
 *
 * The blended score is the single number the rebalance layer consumes.
 * Convergence over the LLM bot population is handled upstream (multiple
 * seeded runs averaged before being passed in here). Convergence over
 * the Heston MC is handled inside heston_run. By the time we hit this
 * module, both inputs are stable - we just normalize and combine.
 *
 * Pipeline:
 *     bot_consensus[N]   -> z-scores
 *     heston_scalars[N]  -> z-scores
 *     blended_score[i]   = w_bot * z_bot[i] + w_heston * z_heston[i]
 *     qsort by blended_score desc
 *
 * Default weights: w_bot = 0.6, w_heston = 0.4.
 * Heston scalar uses lambda1 = 1.0, lambda2 = 0.5 by default.
 */

typedef struct {
    char   symbol[MAX_SYMBOL_LEN];
    int    bot_count;            /* raw aggregation count                  */
    double bot_disagreement;     /* variance across K-seed bot runs (0..1) */
    double heston_expected_ret;
    double heston_forward_vol;
    double heston_es_95;
    double heston_prob_loss;
    double z_bot;
    double z_heston;
    double blended_score;
} ScoredResult;

typedef struct {
    double w_bot;             /* weight on z-scored bot count    */
    double w_heston;          /* weight on z-scored Heston score */
    double lambda_es;         /* Heston: penalty on |ES_95|      */
    double lambda_ploss;      /* Heston: penalty on P(loss>5%)   */
} RiskWeights;

/*
 * Sensible defaults for swing-horizon ranking.
 */
RiskWeights risk_weights_default(void);

/*
 * Build ScoredResult[] from parallel arrays of AggResult and HestonScore.
 * The two arrays must be aligned by symbol (same length n, same order).
 * `bot_disagreement` is optional - pass NULL to leave the field at 0.
 *
 * Returns 0 on success, -1 on bad inputs.
 */
int risk_score_build(const AggResult     *bots,
                     const HestonScore   *heston,
                     const double        *bot_disagreement,  /* may be NULL */
                     int                  n,
                     RiskWeights          w,
                     ScoredResult        *out);

/*
 * qsort the ScoredResult[] by blended_score descending. Ties broken
 * alphabetically so the order is deterministic.
 */
void risk_score_sort(ScoredResult *arr, int n);

#endif /* RISK_SCORE_H */
