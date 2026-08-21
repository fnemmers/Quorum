#include "option_score.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── Small stats helpers ─────────────────────────────────────────── */

typedef struct {
    double sum;
    double sumsq;
    int    n;
} Accum;

static void accum_add(Accum *a, double x) {
    a->sum   += x;
    a->sumsq += x * x;
    a->n     += 1;
}

static void accum_mean_std(const Accum *a, double *mean, double *std) {
    if (a->n <= 0) { *mean = 0.0; *std = 0.0; return; }
    *mean = a->sum / (double)a->n;
    double var = a->sumsq / (double)a->n - (*mean) * (*mean);
    if (var < 0) var = 0;
    *std = sqrt(var);
}

/* Compute a z-score given (x, mean, std). Returns 0 when std ≈ 0. */
static double zscore(double x, double mean, double std) {
    if (std < 1e-9) return 0.0;
    return (x - mean) / std;
}

/* ── Public API ──────────────────────────────────────────────────── */

void option_score_default_weights(OptionScoreWeights *w) {
    if (!w) return;
    /* Renormalized after the bot component was retired. Original
     * split was (edge 0.45, bot 0.25, news 0.20, convex 0.10);
     * dropping bot and dividing the remainder by 0.75 gives:      */
    w->w_edge   = 0.60;
    w->w_news   = 0.27;
    w->w_convex = 0.13;
}

/* Bucketed accumulator keyed by DTE. Small bucket count (typical
 * expiries: 3–6) so an O(n) linear scan per lookup is fine. */
typedef struct {
    int    dte_days;
    Accum  edge;
    Accum  convex;   /* gamma / vega */
} DteBucket;

static DteBucket *find_or_add_bucket(DteBucket *buckets, int *n,
                                     int cap, int dte) {
    for (int i = 0; i < *n; i++) {
        if (buckets[i].dte_days == dte) return &buckets[i];
    }
    if (*n >= cap) return NULL;
    DteBucket *b = &buckets[(*n)++];
    memset(b, 0, sizeof(*b));
    b->dte_days = dte;
    return b;
}

static int cmp_blended_desc(const void *a, const void *b) {
    const OptionScoredResult *ra = (const OptionScoredResult *)a;
    const OptionScoredResult *rb = (const OptionScoredResult *)b;
    if (ra->blended_option_score < rb->blended_option_score) return  1;
    if (ra->blended_option_score > rb->blended_option_score) return -1;
    return 0;
}

int option_score_run(const BatesBacktestRow *in_rows, int in_n,
                     const PerSymbolFusionInput *per_symbol, int ps_n,
                     const OptionScoreWeights *weights,
                     int min_universe,
                     OptionScoredResult **out_rows) {
    if (!in_rows || !out_rows || in_n <= 0) return -1;

    OptionScoreWeights w_default;
    option_score_default_weights(&w_default);
    const OptionScoreWeights *w = weights ? weights : &w_default;

    /* ── Pass 1: bucket per-DTE stats for z_edge & z_convex ───── */

    enum { MAX_BUCKETS = 32 };
    DteBucket buckets[MAX_BUCKETS];
    int       n_buckets = 0;

    for (int i = 0; i < in_n; i++) {
        DteBucket *b = find_or_add_bucket(buckets, &n_buckets,
                                          MAX_BUCKETS, in_rows[i].dte_days);
        if (!b) continue;
        accum_add(&b->edge, in_rows[i].edge_vol_pts);
        double gamma = in_rows[i].gamma0;
        double vega  = in_rows[i].vega0;
        double convex = vega > 1e-9 ? gamma / vega : 0.0;
        accum_add(&b->convex, convex);
    }

    /* ── Pass 2: per-universe stats for z_news ───────────────── */

    Accum news_acc = {0, 0, 0};
    for (int i = 0; i < ps_n; i++)
        accum_add(&news_acc, per_symbol[i].news_scalar);
    double news_mean, news_std;
    accum_mean_std(&news_acc, &news_mean, &news_std);

    /* Small-universe fallback: if fewer distinct symbols than the
     * threshold, z_news_jump is unreliable and zeroed. */
    int small_universe = ps_n < min_universe;

    /* ── Pass 3: emit fused rows ─────────────────────────────── */

    OptionScoredResult *out =
        (OptionScoredResult *)calloc((size_t)in_n, sizeof(OptionScoredResult));
    if (!out) return -1;

    for (int i = 0; i < in_n; i++) {
        OptionScoredResult *o = &out[i];
        o->bates = in_rows[i];

        /* Bucket lookup. */
        double edge_mean = 0, edge_std = 0, cvx_mean = 0, cvx_std = 0;
        for (int b = 0; b < n_buckets; b++) {
            if (buckets[b].dte_days == in_rows[i].dte_days) {
                accum_mean_std(&buckets[b].edge,   &edge_mean, &edge_std);
                accum_mean_std(&buckets[b].convex, &cvx_mean,  &cvx_std);
                break;
            }
        }

        double convex_raw = in_rows[i].vega0 > 1e-9
            ? in_rows[i].gamma0 / in_rows[i].vega0 : 0.0;

        o->z_bates_edge = zscore(in_rows[i].edge_vol_pts, edge_mean, edge_std);
        o->z_convexity  = zscore(convex_raw,              cvx_mean,  cvx_std);

        /* Per-symbol lookup for news scalar. */
        double news_val = 0.0;
        for (int s = 0; s < ps_n; s++) {
            if (strncmp(per_symbol[s].symbol, in_rows[i].symbol,
                        MAX_SYMBOL_LEN) == 0) {
                news_val = per_symbol[s].news_scalar;
                break;
            }
        }

        o->z_news_jump = small_universe
            ? 0.0
            : zscore(news_val, news_mean, news_std);

        o->blended_option_score =
            w->w_edge   * o->z_bates_edge
          + w->w_news   * o->z_news_jump
          + w->w_convex * o->z_convexity;
    }

    /* ── Rank DESC by blended_option_score ────────────────────── */
    qsort(out, (size_t)in_n, sizeof(OptionScoredResult), cmp_blended_desc);
    for (int i = 0; i < in_n; i++) out[i].rank = i + 1;

    *out_rows = out;
    return 0;
}
