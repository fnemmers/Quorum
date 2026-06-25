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
 *
 *   {"cmd":"heston_score_run","run_id":42,"k":20,"horizon_days":21}
 *     → {"type":"heston_scored","run_id":42,
 *        "scores":[{"symbol":"NVDA","expected_return":...,
 *                   "forward_vol":...,"es_95":...,"prob_loss_5":...,
 *                   "n_paths_used":...,"converged":true}, ...]}
 *
 *   {"cmd":"ranking_blend","run_id":42,"k":20,"horizon_days":21,
 *                          "disagreement":{"AAPL":0.12,...},
 *                          "w_bot":0.6,"w_heston":0.4}
 *     → {"type":"ranking_blended","run_id":42,"sigma_blend":...,
 *        "ranked":[{"rank":1,"symbol":"NVDA","blended_score":...,
 *                   "z_bot":...,"z_heston":...,
 *                   "bot_count":...,"bot_disagreement":...,
 *                   "expected_return":...,"forward_vol":...,
 *                   "es_95":...,"prob_loss":...}, ...]}
 *
 *   {"cmd":"rebalance_check","run_id":42,"k":20,
 *                            "disagreement":{...},
 *                            "holdings":[{"symbol":"AAPL",
 *                                         "old_blend":1.2,
 *                                         "days_held":4,
 *                                         "intended_hold_days":21}, ...],
 *                            "exit_rank_band":40,
 *                            "es_risk_limit":-0.10}
 *     → {"type":"rebalance_check","run_id":42,
 *        "events":[{"event_id":17,"symbol":"AAPL",
 *                   "decision":"auto|notify|escalate|hold",
 *                   "suggested_action":"sell|trim|flip|none",
 *                   "obscurity":...,"clarity":...,
 *                   "primary_driver":"score_gap|llm_agreement|...",
 *                   "debrief":"..."}, ...]}
 *
 *   {"cmd":"rebalance_resolve","event_id":17,"action":"sell",
 *                              "override":"keep - earnings beat coming"}
 *     → {"type":"rebalance_resolved","event_id":17}
 *
 *   {"cmd":"convergence_check","run_id":42,"k":20,
 *                              "prev":["NVDA","AAPL",...],
 *                              "threshold":0.9}
 *     → {"type":"convergence_check","run_id":42,
 *        "jaccard":0.87,"threshold":0.9,"stable":false,
 *        "top":[{"symbol":"NVDA","count":...}, ...]}
 *
 *   {"cmd":"heston_path_bundle","symbol":"NVDA",
 *                                "horizon_days":21,
 *                                "n_paths":5000,
 *                                "n_buckets":100}
 *     → {"type":"heston_path_bundle","symbol":"NVDA",
 *        "horizon_days":21,"n_paths":...,"n_steps":21,
 *        "n_buckets":100,"spot":800.5,
 *        "price_min":...,"price_max":...,
 *        "expected_return":...,"es_95":...,
 *        "time_days":[0,1,...,21],
 *        "density":[[bucket0_step0, ..., bucket0_stepN], ...],
 *        "p05":[...],"p50":[...],"p95":[...]}
 */

/* Returns 1 if the command was recognized and handled, 0 if not. */
int ipc_research_dispatch(int client_fd, const char *cmd, cJSON *root);

/* Setup / teardown for any in-memory state held here (currently none). */
void ipc_research_init(void);
void ipc_research_cleanup(void);

#endif /* IPC_RESEARCH_H */
