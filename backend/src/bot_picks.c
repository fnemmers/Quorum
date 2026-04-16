/*
 * bot_picks.c — Postgres persistence for bot ensemble runs.
 *
 * Mirrors the style of db.c (PQexecParams, char-buffer formatting, etc.).
 * The schema is created in db.c db_init() — see the new CREATE TABLE block.
 *
 * Tables:
 *   bot_runs        (id, label, n_bots_target, n_bots_actual, hold_days,
 *                    started_at, finished_at)
 *   bot_picks       (id, run_id, bot_index, persona, symbol, created_at)
 *   backtest_results (id, run_id, start_date, end_date, hold_days,
 *                     n_used, n_skipped, port_return, bench_return, alpha,
 *                     sharpe, max_dd, hit_rate, txn_cost, created_at)
 */

#include "bot_picks.h"
#include "backtest.h"   /* BacktestResult */
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Reach into db.c's connection. We expose it via an extern declaration to
 * avoid threading a pointer through every function — same pattern db.c uses
 * internally with its file-static g_conn. If you'd rather, refactor db.c to
 * publish a `db_conn()` accessor and call it here. */
extern PGconn *db_get_conn(void);   /* implemented in db.c (we add it) */

static int64_t now_ms(void) {
    return (int64_t)time(NULL) * 1000LL;
}

/* ── Run lifecycle ─────────────────────────────────────────────── */

int64_t bot_run_create(const char *label,
                       int n_bots_target,
                       int hold_days) {
    PGconn *conn = db_get_conn();
    if (!conn) return -1;

    char s_target[16], s_hold[16], s_started[32];
    snprintf(s_target,  sizeof(s_target),  "%d",   n_bots_target);
    snprintf(s_hold,    sizeof(s_hold),    "%d",   hold_days);
    snprintf(s_started, sizeof(s_started), "%lld", (long long)now_ms());

    const char *vals[] = { label, s_target, s_hold, s_started };
    PGresult *res = PQexecParams(conn,
        "INSERT INTO bot_runs (label, n_bots_target, hold_days, started_at) "
        "VALUES ($1, $2, $3, $4) RETURNING id;",
        4, NULL, vals, NULL, NULL, 0);

    int64_t id = -1;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
        id = atoll(PQgetvalue(res, 0, 0));
    else
        fprintf(stderr, "[BOTPICKS] run_create failed: %s\n",
                PQerrorMessage(conn));
    PQclear(res);
    return id;
}

int bot_run_finish(int64_t run_id, int n_bots_actual) {
    PGconn *conn = db_get_conn();
    if (!conn) return -1;

    char s_id[32], s_actual[16], s_finished[32];
    snprintf(s_id,       sizeof(s_id),       "%lld", (long long)run_id);
    snprintf(s_actual,   sizeof(s_actual),   "%d",   n_bots_actual);
    snprintf(s_finished, sizeof(s_finished), "%lld", (long long)now_ms());

    const char *vals[] = { s_actual, s_finished, s_id };
    PGresult *res = PQexecParams(conn,
        "UPDATE bot_runs SET n_bots_actual=$1, finished_at=$2 WHERE id=$3;",
        3, NULL, vals, NULL, NULL, 0);

    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK) ? 0 : -1;
    PQclear(res);
    return ok;
}

/* ── Pick ingestion ────────────────────────────────────────────── */

int bot_picks_insert_batch(int64_t run_id,
                           int bot_index,
                           const char *persona,
                           const char *const *picks,
                           int n_picks) {
    PGconn *conn = db_get_conn();
    if (!conn || n_picks <= 0) return -1;

    /* Wrap the batch in a transaction so partial failures roll back. */
    PGresult *bgn = PQexec(conn, "BEGIN;");
    PQclear(bgn);

    char s_run[32], s_bot[16], s_ts[32];
    snprintf(s_run, sizeof(s_run), "%lld", (long long)run_id);
    snprintf(s_bot, sizeof(s_bot), "%d",   bot_index);
    snprintf(s_ts,  sizeof(s_ts),  "%lld", (long long)now_ms());

    int ok = 0;
    for (int i = 0; i < n_picks; i++) {
        const char *vals[] = { s_run, s_bot, persona, picks[i], s_ts };
        PGresult *res = PQexecParams(conn,
            "INSERT INTO bot_picks "
            "  (run_id, bot_index, persona, symbol, created_at) "
            "VALUES ($1, $2, $3, $4, $5);",
            5, NULL, vals, NULL, NULL, 0);
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            fprintf(stderr, "[BOTPICKS] insert failed: %s\n",
                    PQerrorMessage(conn));
            ok = -1;
            PQclear(res);
            break;
        }
        PQclear(res);
    }

    PGresult *end = PQexec(conn, ok == 0 ? "COMMIT;" : "ROLLBACK;");
    PQclear(end);
    return ok;
}

int bot_picks_load_run(int64_t run_id,
                       char (*out)[MAX_SYMBOL_LEN],
                       int max_picks) {
    PGconn *conn = db_get_conn();
    if (!conn) return -1;

    char s_id[32], s_lim[16];
    snprintf(s_id,  sizeof(s_id),  "%lld", (long long)run_id);
    snprintf(s_lim, sizeof(s_lim), "%d",   max_picks);

    const char *vals[] = { s_id, s_lim };
    PGresult *res = PQexecParams(conn,
        "SELECT symbol FROM bot_picks WHERE run_id=$1 LIMIT $2;",
        2, NULL, vals, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "[BOTPICKS] load failed: %s\n",
                PQerrorMessage(conn));
        PQclear(res);
        return -1;
    }

    int n = PQntuples(res);
    if (n > max_picks) n = max_picks;
    for (int i = 0; i < n; i++) {
        const char *sym = PQgetvalue(res, i, 0);
        strncpy(out[i], sym, MAX_SYMBOL_LEN - 1);
        out[i][MAX_SYMBOL_LEN - 1] = '\0';
    }
    PQclear(res);
    return n;
}

/* ── Backtest results ──────────────────────────────────────────── */

int64_t bot_backtest_save(int64_t run_id,
                          const struct BacktestResult *r) {
    PGconn *conn = db_get_conn();
    if (!conn || !r) return -1;

    /* `struct BacktestResult` (from bot_picks.h forward decl) and
     * `BacktestResult` (typedef in backtest.h) are now the same tagged
     * struct, so this is a no-op alias. */
    const BacktestResult *br = r;

    char s_run[32], s_hold[16], s_used[16], s_skip[16];
    char s_pret[32], s_bret[32], s_alpha[32];
    char s_sharpe[32], s_dd[32], s_hit[32], s_cost[32], s_ts[32];

    snprintf(s_run,    sizeof(s_run),    "%lld", (long long)run_id);
    snprintf(s_hold,   sizeof(s_hold),   "%d",   br->hold_days);
    snprintf(s_used,   sizeof(s_used),   "%d",   br->n_tickers_used);
    snprintf(s_skip,   sizeof(s_skip),   "%d",   br->n_skipped);
    snprintf(s_pret,   sizeof(s_pret),   "%.6f", br->portfolio_return_pct);
    snprintf(s_bret,   sizeof(s_bret),   "%.6f", br->benchmark_return_pct);
    snprintf(s_alpha,  sizeof(s_alpha),  "%.6f", br->alpha_pct);
    snprintf(s_sharpe, sizeof(s_sharpe), "%.6f", br->sharpe_ratio);
    snprintf(s_dd,     sizeof(s_dd),     "%.6f", br->max_drawdown_pct);
    snprintf(s_hit,    sizeof(s_hit),    "%.6f", br->hit_rate_pct);
    snprintf(s_cost,   sizeof(s_cost),   "%.6f", br->transaction_cost_pct);
    snprintf(s_ts,     sizeof(s_ts),     "%lld", (long long)now_ms());

    const char *vals[] = {
        s_run, br->start_date, br->end_date, s_hold,
        s_used, s_skip,
        s_pret, s_bret, s_alpha,
        s_sharpe, s_dd, s_hit, s_cost, s_ts
    };

    PGresult *res = PQexecParams(conn,
        "INSERT INTO backtest_results "
        "  (run_id, start_date, end_date, hold_days, "
        "   n_used, n_skipped, "
        "   port_return, bench_return, alpha, "
        "   sharpe, max_dd, hit_rate, txn_cost, created_at) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14) "
        "RETURNING id;",
        14, NULL, vals, NULL, NULL, 0);

    int64_t id = -1;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
        id = atoll(PQgetvalue(res, 0, 0));
    else
        fprintf(stderr, "[BOTPICKS] backtest_save failed: %s\n",
                PQerrorMessage(conn));
    PQclear(res);
    return id;
}
