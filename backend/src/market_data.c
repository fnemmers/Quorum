#include "market_data.h"
#include <string.h>
#include <stdio.h>

MarketState g_state;

void market_data_init(void) {
    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.lock, NULL);
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
