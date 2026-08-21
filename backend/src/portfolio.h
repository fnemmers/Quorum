#ifndef PORTFOLIO_H
#define PORTFOLIO_H

/*
 * portfolio  --  Paper trading engine + live holdings ledger.
 *
 *   paper_account       singleton with cash + initial_cash
 *   paper_positions     per-symbol qty + avg_cost (weighted)
 *   paper_orders        immutable fill log
 *   live_holdings       user-entered real-money book (read-only trades)
 *
 * Fills for paper orders come from Polygon: latest quote if the symbol
 * is already subscribed (market_data.quotes), else a REST snapshot,
 * else the most recent cached daily close. No slippage or commissions.
 *
 * Callers here are the ipc_research dispatch layer — every function
 * returns 0/1 or a status code and writes JSON directly into a caller-
 * provided cJSON object.
 */

#include "cJSON.h"

/* ── init / schema ───────────────────────────────────────────── */

/*
 * Ensures paper/live tables exist and seeds the paper account with
 * `initial_cash` if the account row is missing. Safe to call on every
 * boot.
 */
int portfolio_init(double initial_cash);

/* ── Paper trading ───────────────────────────────────────────── */

/*
 * Fill a market order at the best available current price for `symbol`.
 * `side` is "buy" or "sell". `qty` is fractional-share allowed.
 *
 * Returns 0 on success, negative on failure. On failure `err_out`
 * (nullable) is populated with a short human-readable reason.
 */
int portfolio_paper_order(const char *symbol, const char *side, double qty,
                          char *err_out, int err_len);

/* Reset cash to initial_cash and clear positions + orders. */
int portfolio_paper_reset(void);

/*
 * Populate `out` with:
 *   { type:"paper_state", cash, initial_cash, equity, unrealized_pnl,
 *     total_pnl, total_return_pct,
 *     positions:[{symbol, qty, avg_cost, last_price, market_value,
 *                 unrealized_pnl, unrealized_pct}],
 *     orders:[{id, symbol, side, qty, fill_price, cash_delta,
 *              created_at}] }
 *
 * `out` must already be a cJSON object.
 */
int portfolio_paper_state(cJSON *out);

/* ── Live holdings (read-only trades, editable book) ─────────── */

/*
 * Upsert a live-holdings row.  If `qty <= 0`, the row is deleted.
 * `notes` may be NULL/empty.
 */
int portfolio_live_upsert(const char *symbol, double qty, double avg_cost,
                          const char *notes);

/* Populate `out` with { type:"live_holdings", holdings:[...] }. */
int portfolio_live_state(cJSON *out);

#endif /* PORTFOLIO_H */
