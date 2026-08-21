#ifndef NEWS_JUMP_H
#define NEWS_JUMP_H

#include "heston.h"
#include "market_data.h"
#include <stdint.h>

/*
 * news_jump  --  Convert cached Polygon news into per-ticker Bates jump
 *                parameter overlays (lam_bump, mu_j_bias, sigma_j_bump).
 *
 * Flow:
 *   1. crawler.c stores raw articles in news_cache (title, description,
 *      tickers CSV, published_at).
 *   2. news_jump_recompute() rolls up the window into a per-symbol row
 *      in news_jump_signal.
 *   3. news_jump_apply() reads the freshest row for a symbol and mutates
 *      a HestonParams before it goes through the pricer / MC.
 *
 * Mapping (baseline; tune by editing news_jump.c):
 *   lam_bump      = min(4.0, 0.25 * n_articles_in_window)
 *   mu_j_bias     = sum of keyword hits (negative: -0.02, positive: +0.015)
 *   sigma_j_bump  = 0.02 per negative-event hit (capped at 0.10)
 *   event_class   = highest-priority category matched (empty if none)
 *   sentiment_avg = mean of per-article sentiment scalar in [-1, 1]
 *
 * Optional local vLLM sentiment layer is gated behind QUORUM_NEWS_LLM=1.
 * When enabled the LLM contributes an extra `+0.03 * sentiment` to
 * mu_j_bias on top of the keyword result; the keyword event_class stays
 * authoritative.
 */

typedef struct {
    char    symbol[MAX_SYMBOL_LEN];
    int64_t as_of;                    /* window-end timestamp (ms)       */
    int     window_hours;             /* lookback window in hours        */
    int     n_articles;
    double  sentiment_avg;            /* [-1, 1]; 0 if none              */
    char    event_class[16];          /* "earn"|"guide"|"down"|"up"|
                                       * "ma"|"lit"|"macro"|"beat"|
                                       * "buyback"|"" */
    double  lam_bump;
    double  mu_j_bias;
    double  sigma_j_bump;
} NewsJumpSignal;

/*
 * Recompute per-symbol news-jump signals for every ticker mentioned in
 * news_cache within (as_of_ms - window_hours*3600*1000, as_of_ms].
 * Upserts one row into news_jump_signal per symbol. If window_hours <= 0
 * defaults to 48. Returns number of symbols written, or -1 on error.
 */
int news_jump_recompute(int64_t as_of_ms, int window_hours);

/*
 * Look up the freshest news_jump_signal row for `symbol` at-or-before
 * `as_of_ms` and mutate p accordingly:
 *
 *   p->lam     += lam_bump
 *   p->mu_j    += mu_j_bias
 *   p->sigma_j += sigma_j_bump
 *
 * No-op (returns 0) if no signal row exists. Returns 1 if a signal was
 * applied, -1 on DB error. Zero-clip on final sigma_j so a stray
 * negative bump never turns the vol negative.
 */
int news_jump_apply(HestonParams *p, const char *symbol, int64_t as_of_ms);

/* Read one symbol's freshest signal into *out. Returns 1 if found, 0 if
 * absent, -1 on error. */
int news_jump_get(const char *symbol, int64_t as_of_ms, NewsJumpSignal *out);

#endif /* NEWS_JUMP_H */
