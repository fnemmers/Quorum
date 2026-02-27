#ifndef DB_H
#define DB_H

#include "market_data.h"

/*
 * db  –  PostgreSQL persistence layer (libpq).
 *
 * Tables created automatically on first run:
 *   portfolio     – saved holdings      (survives restarts)
 *   alerts        – saved price alerts  (survives restarts)
 *   price_cache   – OHLCV bars          (avoids re-fetching Polygon)
 *   order_history – trade log           (ready for broker integration)
 *
 * Connection string format:
 *   "host=localhost dbname=stockapp user=postgres password=YOUR_PW"
 */

/* Open connection and create tables if missing. Returns 0 on success. */
int  db_init(const char *connstr);

/* Close the connection. Call at shutdown. */
void db_close(void);

/* ── Portfolio ───────────────────────────────────────────────── */
int  db_portfolio_save(const char *symbol, double shares, double avg_price);
int  db_portfolio_delete(const char *symbol);
int  db_portfolio_load(void);   /* loads into g_state.holdings */

/* ── Alerts ──────────────────────────────────────────────────── */
int  db_alert_save(int id, const char *symbol,
                   AlertType type, double trigger_price);
int  db_alert_delete(int id);
int  db_alert_set_fired(int id);
int  db_alerts_load(void);      /* loads active alerts into g_state.alerts */

/* ── Price cache ─────────────────────────────────────────────── */
int  db_cache_store(const char *symbol, const char *timespan,
                    const OHLCBar *bars, int count);
int  db_cache_load(const char *symbol, const char *timespan,
                   int64_t from_ms, int64_t to_ms,
                   OHLCBar *bars_out, int max_bars);

/* ── Order history ───────────────────────────────────────────── */
typedef enum { ORDER_BUY, ORDER_SELL }               OrderSide;
typedef enum { ORDER_MARKET, ORDER_LIMIT }           OrderType;
typedef enum { ORDER_PENDING, ORDER_FILLED,
               ORDER_CANCELLED }                     OrderStatus;

typedef struct {
    int         id;
    char        symbol[MAX_SYMBOL_LEN];
    OrderSide   side;
    OrderType   order_type;
    double      quantity;
    double      price;
    double      filled_price;
    OrderStatus status;
    int64_t     created_at;
    int64_t     filled_at;
} OrderRecord;

/* Insert a new order. Returns new order ID or -1 on error. */
int  db_order_insert(const char *symbol, OrderSide side,
                     OrderType type, double qty, double price);

/* Update status of an existing order. */
int  db_order_update_status(int id, OrderStatus status,
                             double filled_price, int64_t filled_at);

/* Load recent orders into out[]. Returns count or -1. */
int  db_orders_load(OrderRecord *out, int max_orders);

#endif /* DB_H */
