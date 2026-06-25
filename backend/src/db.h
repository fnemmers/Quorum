#ifndef DB_H
#define DB_H

#include "market_data.h"

/*
 * db  –  SQLite persistence layer.
 *
 * Database file: stockapp.db (created next to the executable)
 *
 * Tables:
 *   portfolio     – saved holdings (survives restarts)
 *   alerts        – saved price alerts (survives restarts)
 *   price_cache   – OHLCV bars (avoids re-fetching Polygon)
 *   order_history – trade log (ready for broker integration)
 */

/* Open/create the database and run migrations. Call once at startup. */
int db_init(const char *path);

/* Close the database. Call at shutdown. */
void db_close(void);

/* Forward decl so callers don't need libpq-fe.h. */
typedef struct pg_conn PGconn;

/* Returns the underlying connection (or NULL if db_init not called).
 * Used by other persistence modules (bot_picks.c) so they can issue
 * parameterised queries against the same connection. */
PGconn *db_get_conn(void);

/* ── Portfolio ───────────────────────────────────────────────── */
int db_portfolio_save(const char *symbol, double shares, double avg_price);
int db_portfolio_delete(const char *symbol);
int db_portfolio_load(void); /* loads into g_state.holdings */

/* ── Alerts ──────────────────────────────────────────────────── */
int db_alert_save(int id, const char *symbol,
                  AlertType type, double trigger_price);
int db_alert_delete(int id);
int db_alert_set_fired(int id);
int db_alerts_load(void); /* loads into g_state.alerts */

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

/* ── Order history ───────────────────────────────────────────── */
typedef enum
{
    ORDER_BUY,
    ORDER_SELL
} OrderSide;
typedef enum
{
    ORDER_MARKET,
    ORDER_LIMIT
} OrderType;
typedef enum
{
    ORDER_PENDING,
    ORDER_FILLED,
    ORDER_CANCELLED
} OrderStatus;

typedef struct
{
    int id;
    char symbol[MAX_SYMBOL_LEN];
    OrderSide side;
    OrderType order_type;
    double quantity;
    double price;
    OrderStatus status;
    int64_t created_at;
    int64_t filled_at;
    double filled_price;
} OrderRecord;

int db_order_insert(const char *symbol, OrderSide side,
                    OrderType type, double qty, double price);
int db_order_update_status(int id, OrderStatus status,
                           double filled_price, int64_t filled_at);
int db_orders_load(OrderRecord *out, int max_orders); /* returns count */

/* ── Heston scores ───────────────────────────────────────────── */
/*
 * Upsert one Heston score for (run_id, symbol). Returns 0 on success.
 */
int db_heston_save(long long run_id,
                   const char *symbol,
                   double expected_return,
                   double forward_vol,
                   double es_95,
                   double prob_loss_5,
                   int    n_paths_used,
                   int    converged);

/* ── Rebalance events ─────────────────────────────────────────── */
/*
 * Insert a new rebalance event. Returns the new row id, or -1 on failure.
 * The user_override and resolved_at columns are NULL until the user
 * accepts/overrides via db_rebalance_resolve().
 */
long long db_rebalance_save(const char *symbol,
                            const char *decision,           /* "auto"/"notify"/"escalate"/"hold" */
                            const char *suggested_action,   /* "sell"/"trim"/"flip"/"none" */
                            const char *action_taken_or_null,
                            double      old_blend,
                            double      new_blend,
                            double      exit_threshold,
                            double      obscurity,
                            double      clarity,
                            const char *primary_driver,
                            double      score_gap_clarity,
                            double      llm_agreement,
                            double      heston_breach,
                            double      horizon_maturity,
                            int         days_held,
                            int         intended_hold_days,
                            const char *debrief);

/*
 * Record the user's resolution of an outstanding NOTIFY/ESCALATE event.
 * action_taken is the action the user accepted (or "none" if they
 * rejected the suggestion). override is NULL if accepted as suggested,
 * or a short note if they overrode.
 */
int db_rebalance_resolve(long long  event_id,
                         const char *action_taken,
                         const char *user_override_or_null);

#endif /* DB_H */
