#ifndef DB_H
#define DB_H

#include "market_data.h"

/*
 * db  --  Minimal PostgreSQL persistence layer for the Bates + AI-jump
 *         project. Three schemas:
 *
 *   price_cache        --  daily OHLCV bars (avoids re-fetching Polygon
 *                          on every Heston calibration)
 *   news_cache         --  raw Polygon news articles
 *   news_jump_signal   --  per-ticker news-derived jump parameter overlay
 *
 * All other state (options grid, Bates backtest results, option
 * rankings) is stateless — commands compute-and-return, the frontend
 * caches.
 */

int db_init(const char *connstr);
void db_close(void);

/* Forward decl so callers don't need libpq-fe.h. */
typedef struct pg_conn PGconn;

/* Returns the underlying connection (or NULL if db_init not called).
 * news_jump.c uses this for its parameterised queries. */
PGconn *db_get_conn(void);

/* ── Price cache ─────────────────────────────────────────────── */
/*
 * Store a batch of OHLCV bars for a symbol+timespan combo.
 * Existing rows with the same (symbol, timespan, bar_time) are replaced.
 */
int db_cache_store(const char *symbol, const char *timespan,
                   const OHLCBar *bars, int count);

/*
 * Load cached bars for a symbol+timespan into bars_out.
 * Returns number of bars loaded, or -1 on error.
 * Only returns bars whose bar_time is between from_ms and to_ms (Unix ms).
 */
int db_cache_load(const char *symbol, const char *timespan,
                  int64_t from_ms, int64_t to_ms,
                  OHLCBar *bars_out, int max_bars);

#endif /* DB_H */
