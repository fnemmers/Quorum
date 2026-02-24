#ifndef MARKET_DATA_H
#define MARKET_DATA_H

#include <stdint.h>
#include <pthread.h>

#define MAX_SYMBOLS     64
#define MAX_SYMBOL_LEN  16
#define PRICE_HISTORY   1000   /* ring-buffer size per symbol */
#define MAX_CLIENTS     16

/* ── A single OHLCV bar (candlestick) ──────────────────────────────── */
typedef struct {
    int64_t  timestamp;   /* Unix ms */
    double   open;
    double   high;
    double   low;
    double   close;
    int64_t  volume;
} OHLCBar;

/* ── Latest quote for one symbol ─────────────────────────────────────  */
typedef struct {
    char     symbol[MAX_SYMBOL_LEN];
    double   price;
    double   bid;
    double   ask;
    int64_t  volume;
    int64_t  timestamp;   /* Unix ms */
    int      valid;       /* 1 = has data */
} Quote;

/* ── Alert record ────────────────────────────────────────────────────  */
typedef enum { ALERT_ABOVE, ALERT_BELOW } AlertType;

typedef struct {
    int        id;
    char       symbol[MAX_SYMBOL_LEN];
    AlertType  type;
    double     trigger_price;
    int        active;
} AlertRecord;

#define MAX_ALERTS 256

/* ── Portfolio holding ───────────────────────────────────────────────  */
typedef struct {
    char   symbol[MAX_SYMBOL_LEN];
    double shares;
    double avg_price;
} Holding;

#define MAX_HOLDINGS 64

/* ── Shared state (protected by mutex) ───────────────────────────────  */
typedef struct {
    pthread_mutex_t lock;

    /* subscribed symbols */
    char     symbols[MAX_SYMBOLS][MAX_SYMBOL_LEN];
    int      symbol_count;

    /* latest quotes */
    Quote    quotes[MAX_SYMBOLS];

    /* price ring-buffers (for mini charts) */
    double   price_history[MAX_SYMBOLS][PRICE_HISTORY];
    int      price_head[MAX_SYMBOLS];
    int      price_count[MAX_SYMBOLS];

    /* alerts */
    AlertRecord alerts[MAX_ALERTS];
    int         alert_count;
    int         next_alert_id;

    /* portfolio */
    Holding  holdings[MAX_HOLDINGS];
    int      holding_count;

    /* IPC client sockets (so we can broadcast updates) */
    int      client_fds[MAX_CLIENTS];
    int      client_count;
} MarketState;

/* Global singleton – defined in market_data.c */
extern MarketState g_state;

void market_data_init(void);

/* Returns index of symbol, or -1 if not found */
int  market_find_symbol(const char *symbol);

/* Returns index (creates slot if missing), -1 on overflow */
int  market_get_or_add_symbol(const char *symbol);

/* Update quote; also appends to ring buffer */
void market_update_quote(const char *symbol, double price,
                         double bid, double ask,
                         int64_t volume, int64_t ts);

/* Portfolio helpers */
void market_portfolio_add(const char *symbol, double shares, double price);
void market_portfolio_remove(const char *symbol);

/* Alert helpers */
int  market_alert_add(const char *symbol, AlertType type, double trigger);
void market_alert_remove(int id);

#endif /* MARKET_DATA_H */
