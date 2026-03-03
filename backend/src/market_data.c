#include market_data.h
#include <string.h>
#include <stdio.h>

MarketState g_state;

void market_data_init(void) {
    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.lock, NULL);
    g_state.next_alert_id = 1;
    /* pre-fill client fds with -1 so we can detect empty slots */
    for (int i = 0; i < MAX_CLIENTS; i++) g_state.client_fds[i] = -1;
}

int market_find_symbol(const char *symbol) {
    for (int i = 0; i < g_state.symbol_count; i++)
        if (strcmp(g_state.symbols[i], symbol) == 0) return i;
    return -1;
}

int market_get_or_add_symbol(const char *symbol) {
    int idx = market_find_symbol(symbol);
    if (idx >= 0) return idx;
    if (g_state.symbol_count >= MAX_SYMBOLS) return -1;
    idx = g_state.symbol_count++;
    strncpy(g_state.symbols[idx], symbol, MAX_SYMBOL_LEN - 1);
    g_state.quotes[idx].valid = 0;
    g_state.price_head[idx]   = 0;
    g_state.price_count[idx]  = 0;
    return idx;
}

void market_update_quote(const char *symbol, double price,
    double bid, double ask,
    int64_t volume, int64_t ts) {
    pthread_mutex_lock(&g_state.lock);

    int idx = market_get_or_add_symbol(symbol);
    if (idx < 0) { pthread_mutex_unlock(&g_state.lock); return; }

    Quote *q   = &g_state.quotes[idx];
    strncpy(q->symbol, symbol, MAX_SYMBOL_LEN - 1);
    q->price     = price;
    q->bid       = bid;
    q->ask       = ask;
    q->volume    = volume;
    q->timestamp = ts;
    q->valid     = 1;

    /* append to ring buffer */
    int h = g_state.price_head[idx];
    g_state.price_history[idx][h] = price;
    g_state.price_head[idx]       = (h + 1) % PRICE_HISTORY;
    if (g_state.price_count[idx] < PRICE_HISTORY)
        g_state.price_count[idx]++;

    pthread_mutex_unlock(&g_state.lock);
}

void market_portfolio_add(const char *symbol, double shares, double price) {
    pthread_mutex_lock(&g_state.lock);

    /* check if holding already exists → update avg */
    for (int i = 0; i < g_state.holding_count; i++) {
        if (strcmp(g_state.holdings[i].symbol, symbol) == 0) {
            Holding *h   = &g_state.holdings[i];
            double total = h->shares * h->avg_price + shares * price;
            h->shares   += shares;
            h->avg_price = (h->shares > 0) ? total / h->shares : 0.0;
            pthread_mutex_unlock(&g_state.lock);
            return;
        }
    }

    if (g_state.holding_count < MAX_HOLDINGS) {
        Holding *h = &g_state.holdings[g_state.holding_count++];
        strncpy(h->symbol, symbol, MAX_SYMBOL_LEN - 1);
        h->shares    = shares;
        h->avg_price = price;
    }

    pthread_mutex_unlock(&g_state.lock);
}

void market_portfolio_remove(const char *symbol) {
    pthread_mutex_lock(&g_state.lock);
    for (int i = 0; i < g_state.holding_count; i++) {
        if (strcmp(g_state.holdings[i].symbol, symbol) == 0) {
            g_state.holdings[i] =
                g_state.holdings[--g_state.holding_count];
            break;
        }
    }
    pthread_mutex_unlock(&g_state.lock);
}

int market_alert_add(const char *symbol, AlertType type, double trigger) {
    pthread_mutex_lock(&g_state.lock);
    if (g_state.alert_count >= MAX_ALERTS) {
        pthread_mutex_unlock(&g_state.lock);
        return -1;
    }
    AlertRecord *a = &g_state.alerts[g_state.alert_count++];
    a->id            = g_state.next_alert_id++;
    strncpy(a->symbol, symbol, MAX_SYMBOL_LEN - 1);
    a->type          = type;
    a->trigger_price = trigger;
    a->active        = 1;
    int id = a->id;
    pthread_mutex_unlock(&g_state.lock);
    return id;
}

void market_alert_remove(int id) {
    pthread_mutex_lock(&g_state.lock);
    for (int i = 0; i < g_state.alert_count; i++) {
        if (g_state.alerts[i].id == id) {
            g_state.alerts[i] =
                g_state.alerts[--g_state.alert_count];
            break;
        }
    }
    pthread_mutex_unlock(&g_state.lock);
}
