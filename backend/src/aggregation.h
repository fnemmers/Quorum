#ifndef AGGREGATION_H
#define AGGREGATION_H

#include "market_data.h"  /* MAX_SYMBOL_LEN */
#include <stddef.h>

/*
 * aggregation  –  Counts how often each ticker appears across N bot picks
 *                 and returns the top-K most-picked tickers.
 *
 * This is a hand-written hash map of (ticker -> count). The map is owned
 * by an Aggregator instance so multiple runs can coexist (e.g. one in
 * progress while another is being queried).
 *
 * Typical lifecycle:
 *
 *     Aggregator *a = agg_create(2048);
 *     for each bot pick:
 *         agg_add_pick(a, "AAPL");
 *     AggResult top20[20];
 *     int n = agg_top_k(a, top20, 20);
 *     agg_free(a);
 *
 * The hot path is agg_add_pick(): if 500 bots each pick 20 tickers,
 * that's 10,000 inserts per run. Make it O(1) average — that's the whole
 * point of using a hash map instead of a linear scan.
 */

typedef struct Aggregator Aggregator;   /* opaque – defined in aggregation.c */

typedef struct {
    char   symbol[MAX_SYMBOL_LEN];
    int    count;        /* how many bots picked this ticker */
} AggResult;

/*
 * Create an aggregator with an initial bucket capacity.
 * Pick a power of two if you use bitmask indexing (e.g. 2048).
 * Returns NULL on allocation failure.
 */
Aggregator *agg_create(size_t initial_capacity);

/* Free everything owned by the aggregator. */
void agg_free(Aggregator *a);

/*
 * Record one bot's pick of one ticker.
 * Increments the count for `symbol` (creating the entry if new).
 * Returns 0 on success, -1 on allocation failure.
 *
 * NOTE: bots will sometimes return tickers not in the S&P 500 universe
 * (typos, hallucinated symbols). The validation/filtering is done at the
 * IPC layer before calling this — agg_add_pick assumes the symbol is OK.
 */
int agg_add_pick(Aggregator *a, const char *symbol);

/*
 * Fill `out` with the top-K most-picked tickers, sorted by count desc.
 * Returns the number of entries actually written (<= k).
 *
 * Tiebreak rule: alphabetical by ticker (deterministic, easy to test).
 *
 * Does NOT modify the aggregator — safe to call multiple times.
 */
int agg_top_k(Aggregator *a, AggResult *out, int k);

/* Total number of distinct tickers currently tracked. */
size_t agg_distinct_count(const Aggregator *a);

/* Total number of picks ingested (sum of all counts). */
size_t agg_total_picks(const Aggregator *a);

#endif /* AGGREGATION_H */
