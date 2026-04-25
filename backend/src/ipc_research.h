#ifndef IPC_RESEARCH_H
#define IPC_RESEARCH_H

#include "cJSON.h"

/*
 * ipc_research  –  Research-side IPC handlers (bot ensemble + backtest).
 *
 * Lives separately from ipc_server.c so the legacy portfolio/alert/quote
 * code stays untouched. ipc_server.c calls ipc_research_dispatch() for any
 * command it doesn't recognize.
 *
 * Protocol additions (newline-delimited JSON, all run over the existing
 * port 8765 TCP server):
 *
 * Client → C
 *   {"cmd":"sp500_list"}
 *     → {"type":"sp500_list","tickers":["A","AAL",...]}
 *
 *   {"cmd":"bot_run_create","label":"nightly-2026-04-07",
 *                            "n_bots_target":500,"hold_days":30}
 *     → {"type":"bot_run_created","run_id":42}
 *
 *   {"cmd":"bot_picks_ingest","run_id":42,"bot_index":17,
 *                              "persona":"value",
 *                              "picks":["AAPL","MSFT",...]}
 *     → {"type":"bot_picks_ack","run_id":42,"bot_index":17,"n":20}
 *
 *   {"cmd":"bot_run_finish","run_id":42,"n_bots_actual":487}
 *     → {"type":"bot_run_finished","run_id":42}
 *
 *   {"cmd":"aggregate_run","run_id":42,"k":20}
 *     → {"type":"aggregate_result","run_id":42,
 *        "top":[{"symbol":"NVDA","count":312},...]}
 *
 *   {"cmd":"backtest_run","run_id":42,"k":20,
 *                          "start_date":"2025-08-15","hold_days":30}
 *     → {"type":"backtest_result","run_id":42,
 *        "port_return":..., "bench_return":..., "alpha":...,
 *        "sharpe":..., "max_dd":..., "hit_rate":...,
 *        "n_used":..., "n_skipped":...}
 *
 *   {"cmd":"crawl_news","limit":50}
 *     → {"type":"crawl_done","n_fetched":50,"n_stored":45}
 *
 *   {"cmd":"get_news_digest","max_chars":32000,"days":7}
 *     → {"type":"news_digest","digest":"=== MARKET BRIEFING ..."}
 */

/* Returns 1 if the command was recognized and handled, 0 if not. */
int ipc_research_dispatch(int client_fd, const char *cmd, cJSON *root);

/* Setup / teardown for any in-memory state held here (currently none). */
void ipc_research_init(void);
void ipc_research_cleanup(void);

#endif /* IPC_RESEARCH_H */
