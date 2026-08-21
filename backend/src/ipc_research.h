#ifndef IPC_RESEARCH_H
#define IPC_RESEARCH_H

#include "cJSON.h"

/*
 * ipc_research  --  Backend commands for the Bates + AI-jump project.
 *
 * All computation is stateless — every command takes the parameters it
 * needs (including a `symbols` array), computes, and returns. Nothing
 * is persisted except the raw feed layers (price_cache, news_cache,
 * news_jump_signal).
 *
 * Protocol (newline-delimited JSON over the ipc_server.c :8765 TCP
 * server). ipc_server.c calls ipc_research_dispatch() for any command
 * it doesn't recognize.
 *
 * Client -> Backend
 *
 *   {"cmd":"sp500_list"}
 *     -> {"type":"sp500_list","tickers":["A","AAL",...]}
 *
 *   {"cmd":"crawl_news","limit":50}
 *     -> {"type":"crawl_done","n_fetched":50,"n_stored":45}
 *
 *   {"cmd":"get_news_digest","max_chars":32000,"days":7}
 *     -> {"type":"news_digest","digest":"..."}
 *
 *   {"cmd":"news_jump_status","symbols":["AAPL","MSFT"],
 *                              "window_hours":48}
 *     -> {"type":"news_jump","as_of":...,"window_hours":48,
 *         "rows":[{"symbol":"AAPL","n_articles":5,
 *                  "event_class":"guide","sentiment_avg":-0.12,
 *                  "lam_bump":1.25,"mu_j_bias":-0.02,
 *                  "sigma_j_bump":0.02}, ...]}
 *
 *   {"cmd":"heston_path_bundle","symbol":"NVDA","horizon_days":21,
 *                                "n_paths":5000,"n_buckets":100}
 *     -> {"type":"heston_path_bundle","symbol":"NVDA", ...}
 *
 *   {"cmd":"heston_surface","symbol":"NVDA","n_strikes":21,
 *                            "n_maturities":8,"max_mat_days":180,
 *                            "moneyness_lo":0.7,"moneyness_hi":1.3,
 *                            "r":0.045}
 *     -> {"type":"heston_surface","symbol":"NVDA", ...}
 *
 *   {"cmd":"heston_diagnostics","symbol":"NVDA","n_paths":4000}
 *     -> {"type":"heston_diagnostics","symbol":"NVDA", ...}
 *
 *   {"cmd":"bates_backtest_run",
 *          "symbols":["AAPL","MSFT","NVDA",...],
 *          "horizon_days":30, "moneyness_lo":0.85, "moneyness_hi":1.15,
 *          "n_strikes":7, "expiries_days":[7,30,90],
 *          "n_paths":256, "noise_sigma_vol":0.015, "r":0.045,
 *          "seed":12345}
 *     -> {"type":"bates_backtest","summary":{...},"rows":[...]}
 *
 *   {"cmd":"option_ranking_blend",
 *          "symbols":["AAPL","MSFT",...],
 *          "horizon_days":30, "moneyness_lo":0.85, "moneyness_hi":1.15,
 *          "n_strikes":7, "expiries_days":[7,30,90],
 *          "n_paths":256, "noise_sigma_vol":0.015, "r":0.045,
 *          "seed":12345,
 *          "w_edge":0.60, "w_news":0.27, "w_convex":0.13,
 *          "min_universe":8}
 *     -> {"type":"option_ranking","n_rows":...,"weights":{...},
 *         "ranked":[{rank, symbol, strike, dte_days, right, ...,
 *                    z_bates_edge, z_news_jump, z_convexity,
 *                    blended_option_score, signed_edge}, ...]}
 */

/* Returns 1 if the command was recognized and handled, 0 if not. */
int ipc_research_dispatch(int client_fd, const char *cmd, cJSON *root);

/* Setup / teardown for any in-memory state held here (currently none). */
void ipc_research_init(void);
void ipc_research_cleanup(void);

#endif /* IPC_RESEARCH_H */
