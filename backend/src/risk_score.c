#include "risk_score.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * risk_score.c  -  z-score blending of bot + Heston, then a single sort.
 *
 * z-scoring is done across the candidate universe in the call - so the
 * blended ordering only makes sense relative to the rest of the array
 * passed in. That is intentional: ranking is a relative operation.
 */

RiskWeights risk_weights_default(void) {
    RiskWeights w;
    w.w_bot         = 0.6;
    w.w_heston      = 0.4;
    w.lambda_es     = 1.0;
    w.lambda_ploss  = 0.5;
    return w;
}

/* Standard sample mean / stdev. Returns stdev = 1 when n < 2 or var ~ 0
 * to avoid divide-by-zero when z-scoring a degenerate array. */
static void mean_stdev(const double *x, int n, double *mean_out, double *sd_out) {
    if (n <= 0) { *mean_out = 0.0; *sd_out = 1.0; return; }

    double m = 0.0;
    for (int i = 0; i < n; i++) m += x[i];
    m /= (double)n;

    double v = 0.0;
    for (int i = 0; i < n; i++) {
        double d = x[i] - m;
        v += d * d;
    }
    v /= (double)(n > 1 ? n - 1 : 1);

    double sd = sqrt(v);
    if (sd < 1e-12) sd = 1.0;     /* degenerate - all values equal */

    *mean_out = m;
    *sd_out   = sd;
}

int risk_score_build(const AggResult     *bots,
                     const HestonScore   *heston,
                     const double        *bot_disagreement,
                     int                  n,
                     RiskWeights          w,
                     ScoredResult        *out) {
    if (!bots || !heston || !out || n <= 0) return -1;

    double *bot_raw    = malloc((size_t)n * sizeof(double));
    double *heston_raw = malloc((size_t)n * sizeof(double));
    if (!bot_raw || !heston_raw) { free(bot_raw); free(heston_raw); return -1; }

    for (int i = 0; i < n; i++) {
        bot_raw[i]    = (double)bots[i].count;
        heston_raw[i] = heston_risk_adjusted_score(&heston[i],
                                                   w.lambda_es,
                                                   w.lambda_ploss);
    }

    double bm, bs, hm, hs;
    mean_stdev(bot_raw,    n, &bm, &bs);
    mean_stdev(heston_raw, n, &hm, &hs);

    for (int i = 0; i < n; i++) {
        strncpy(out[i].symbol, bots[i].symbol, MAX_SYMBOL_LEN - 1);
        out[i].symbol[MAX_SYMBOL_LEN - 1] = '\0';

        out[i].bot_count           = bots[i].count;
        out[i].bot_disagreement    = bot_disagreement ? bot_disagreement[i] : 0.0;
        out[i].heston_expected_ret = heston[i].expected_return;
        out[i].heston_forward_vol  = heston[i].forward_vol;
        out[i].heston_es_95        = heston[i].es_95;
        out[i].heston_prob_loss    = heston[i].prob_loss_5;

        out[i].z_bot    = (bot_raw[i]    - bm) / bs;
        out[i].z_heston = (heston_raw[i] - hm) / hs;
        out[i].blended_score = w.w_bot * out[i].z_bot
                             + w.w_heston * out[i].z_heston;
    }

    free(bot_raw);
    free(heston_raw);
    return 0;
}

/* qsort comparator: blended_score desc, then symbol asc for ties. */
static int cmp_scored(const void *lhs, const void *rhs) {
    const ScoredResult *a = (const ScoredResult *)lhs;
    const ScoredResult *b = (const ScoredResult *)rhs;
    if (a->blended_score < b->blended_score) return  1;
    if (a->blended_score > b->blended_score) return -1;
    return strcmp(a->symbol, b->symbol);
}

void risk_score_sort(ScoredResult *arr, int n) {
    if (!arr || n <= 1) return;
    qsort(arr, (size_t)n, sizeof(ScoredResult), cmp_scored);
}
