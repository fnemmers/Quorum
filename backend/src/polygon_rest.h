#ifndef POLYGON_REST_H
#define POLYGON_REST_H

#include "market_data.h"

/*
 * polygon_rest  –  Polygon.io REST API client.
 *
 * Uses libcurl for HTTPS requests.  All functions are synchronous
 * (block until complete) and should be called from a worker thread.
 *
 * Polygon REST base: https://api.polygon.io
 */

/* Call once at startup (initialises libcurl) */
void polygon_rest_init(const char *api_key);
void polygon_rest_cleanup(void);

/*
 * Fetch aggregate (OHLCV) bars for a symbol.
 *   timespan : "minute" | "hour" | "day" | "week" | "month"
 *   from_date: "YYYY-MM-DD"
 *   to_date  : "YYYY-MM-DD"
 *   bars_out : caller-allocated array
 *   max_bars : capacity of bars_out
 * Returns number of bars written, or -1 on error.
 */
int polygon_rest_aggregates(const char *symbol,
                            int         multiplier,
                            const char *timespan,
                            const char *from_date,
                            const char *to_date,
                            OHLCBar    *bars_out,
                            int         max_bars);

/*
 * Fetch the latest snapshot quote for a symbol.
 * Returns 0 on success, -1 on error.
 */
int polygon_rest_snapshot(const char *symbol, Quote *out);

#endif /* POLYGON_REST_H */
