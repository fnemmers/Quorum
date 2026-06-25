#include "db.h"
#include "market_data.h"
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static PGconn *g_conn = NULL;

/* Accessor for other modules (bot_picks.c) so they can use parameterised
 * queries without each holding their own connection. Returns NULL if db_init
 * has not been called. */
PGconn *db_get_conn(void) { return g_conn; }

/* ── Helpers ─────────────────────────────────────────────────── */

static int64_t now_ms(void)
{
    return (int64_t)time(NULL) * 1000LL;
}

/* Run a statement that returns no rows. Returns 0 on success. */
static int exec_sql(const char *sql)
{
    PGresult *res = PQexec(g_conn, sql);
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "[DB] Error: %s\n  SQL: %s\n",
                PQerrorMessage(g_conn), sql);
        PQclear(res);
        return -1;
    }
    PQclear(res);
    return 0;
}

/* Run a parameterised query; caller must PQclear the result. */
static PGresult *exec_params(const char *sql,
                int nparams,
                             const char *const *vals)
{
    PGresult *res = PQexecParams(g_conn, sql, nparams,
                    NULL, vals, NULL, NULL, 0);
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "[DB] Query error: %s\n  SQL: %s\n",
                PQerrorMessage(g_conn), sql);
    }
    return res;
}

/* ── Init / schema ───────────────────────────────────────────── */

int db_init(const char *connstr)
{
    g_conn = PQconnectdb(connstr);
    if (PQstatus(g_conn) != CONNECTION_OK)
    {
        fprintf(stderr, "[DB] Connection failed: %s\n",
                PQerrorMessage(g_conn));
        PQfinish(g_conn);
        g_conn = NULL;
        return -1;
    }
    printf("[DB] Connected to PostgreSQL: %s\n",
           PQdb(g_conn));

    /* Create tables */
    exec_sql(
        "CREATE TABLE IF NOT EXISTS portfolio ("
        "  symbol     TEXT PRIMARY KEY,"
        "  shares     DOUBLE PRECISION NOT NULL,"
        "  avg_price  DOUBLE PRECISION NOT NULL,"
        "  updated_at BIGINT NOT NULL"
        ");");

    exec_sql(
        "CREATE TABLE IF NOT EXISTS alerts ("
        "  id             SERIAL PRIMARY KEY,"
        "  symbol         TEXT   NOT NULL,"
        "  condition      TEXT   NOT NULL,"
        "  trigger_price  DOUBLE PRECISION NOT NULL,"
        "  active         BOOLEAN NOT NULL DEFAULT TRUE,"
        "  created_at     BIGINT  NOT NULL,"
        "  fired_at       BIGINT"
        ");");

    exec_sql(
        "CREATE TABLE IF NOT EXISTS price_cache ("
        "  symbol    TEXT             NOT NULL,"
        "  timespan  TEXT             NOT NULL,"
        "  bar_time  BIGINT           NOT NULL,"
        "  open      DOUBLE PRECISION NOT NULL,"
        "  high      DOUBLE PRECISION NOT NULL,"
        "  low       DOUBLE PRECISION NOT NULL,"
        "  close     DOUBLE PRECISION NOT NULL,"
        "  volume    BIGINT           NOT NULL,"
        "  PRIMARY KEY (symbol, timespan, bar_time)"
        ");");

    exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_price_cache "
        "ON price_cache (symbol, timespan, bar_time);");

    exec_sql(
        "CREATE TABLE IF NOT EXISTS order_history ("
        "  id            SERIAL PRIMARY KEY,"
        "  symbol        TEXT             NOT NULL,"
        "  side          TEXT             NOT NULL,"
        "  order_type    TEXT             NOT NULL,"
        "  quantity      DOUBLE PRECISION NOT NULL,"
        "  price         DOUBLE PRECISION NOT NULL,"
        "  filled_price  DOUBLE PRECISION,"
        "  status        TEXT             NOT NULL DEFAULT 'pending',"
        "  created_at    BIGINT           NOT NULL,"
        "  filled_at     BIGINT"
        ");");

    /* ── Bot ensemble research tables ─────────────────────────── */

    exec_sql(
        "CREATE TABLE IF NOT EXISTS bot_runs ("
        "  id              BIGSERIAL PRIMARY KEY,"
        "  label           TEXT    NOT NULL,"
        "  n_bots_target   INT     NOT NULL,"
        "  n_bots_actual   INT,"
        "  hold_days       INT     NOT NULL,"
        "  started_at      BIGINT  NOT NULL,"
        "  finished_at     BIGINT"
        ");");

    exec_sql(
        "CREATE TABLE IF NOT EXISTS bot_picks ("
        "  id          BIGSERIAL PRIMARY KEY,"
        "  run_id      BIGINT  NOT NULL REFERENCES bot_runs(id) ON DELETE CASCADE,"
        "  bot_index   INT     NOT NULL,"
        "  persona     TEXT    NOT NULL,"
        "  symbol      TEXT    NOT NULL,"
        "  created_at  BIGINT  NOT NULL"
        ");");

    exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_bot_picks_run "
        "ON bot_picks (run_id);");

    exec_sql(
        "CREATE TABLE IF NOT EXISTS backtest_results ("
        "  id            BIGSERIAL PRIMARY KEY,"
        "  run_id        BIGINT  NOT NULL REFERENCES bot_runs(id) ON DELETE CASCADE,"
        "  start_date    TEXT    NOT NULL,"
        "  end_date      TEXT    NOT NULL,"
        "  hold_days     INT     NOT NULL,"
        "  n_used        INT     NOT NULL,"
        "  n_skipped     INT     NOT NULL,"
        "  port_return   DOUBLE PRECISION NOT NULL,"
        "  bench_return  DOUBLE PRECISION NOT NULL,"
        "  alpha         DOUBLE PRECISION NOT NULL,"
        "  sharpe        DOUBLE PRECISION NOT NULL,"
        "  max_dd        DOUBLE PRECISION NOT NULL,"
        "  hit_rate      DOUBLE PRECISION NOT NULL,"
        "  txn_cost      DOUBLE PRECISION NOT NULL,"
        "  created_at    BIGINT  NOT NULL"
        ");");

    /* ── News crawler cache ──────────────────────────────────────── */

    exec_sql(
        "CREATE TABLE IF NOT EXISTS news_cache ("
        "  article_id   TEXT PRIMARY KEY,"
        "  title        TEXT    NOT NULL,"
        "  description  TEXT    NOT NULL,"
        "  publisher    TEXT    NOT NULL,"
        "  url          TEXT    NOT NULL,"
        "  tickers      TEXT    NOT NULL,"
        "  published_at BIGINT  NOT NULL"
        ");");

    exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_news_cache_published "
        "ON news_cache (published_at DESC);");

    /* ── Heston MC per-stock risk scores ────────────────────────── */

    exec_sql(
        "CREATE TABLE IF NOT EXISTS heston_scores ("
        "  run_id          BIGINT NOT NULL REFERENCES bot_runs(id) ON DELETE CASCADE,"
        "  symbol          TEXT   NOT NULL,"
        "  expected_return DOUBLE PRECISION NOT NULL,"
        "  forward_vol     DOUBLE PRECISION NOT NULL,"
        "  es_95           DOUBLE PRECISION NOT NULL,"
        "  prob_loss_5     DOUBLE PRECISION NOT NULL,"
        "  n_paths_used    INT    NOT NULL,"
        "  converged       BOOLEAN NOT NULL,"
        "  created_at      BIGINT NOT NULL,"
        "  PRIMARY KEY (run_id, symbol)"
        ");");

    /* ── Active-management rebalance audit log ──────────────────── */
    /* override is NULL until the user acts on it. action_taken is what
     * we end up doing - same as suggested_action for AUTO, possibly
     * different (or NONE) for NOTIFY/ESCALATE after user input. */

    exec_sql(
        "CREATE TABLE IF NOT EXISTS rebalance_events ("
        "  id                  BIGSERIAL PRIMARY KEY,"
        "  symbol              TEXT NOT NULL,"
        "  decision            TEXT NOT NULL,"
        "  suggested_action    TEXT NOT NULL,"
        "  action_taken        TEXT,"
        "  old_blend           DOUBLE PRECISION NOT NULL,"
        "  new_blend           DOUBLE PRECISION NOT NULL,"
        "  exit_threshold      DOUBLE PRECISION NOT NULL,"
        "  obscurity           DOUBLE PRECISION NOT NULL,"
        "  clarity             DOUBLE PRECISION NOT NULL,"
        "  primary_driver      TEXT NOT NULL,"
        "  score_gap_clarity   DOUBLE PRECISION NOT NULL,"
        "  llm_agreement       DOUBLE PRECISION NOT NULL,"
        "  heston_breach       DOUBLE PRECISION NOT NULL,"
        "  horizon_maturity    DOUBLE PRECISION NOT NULL,"
        "  days_held           INT NOT NULL,"
        "  intended_hold_days  INT NOT NULL,"
        "  debrief             TEXT NOT NULL,"
        "  user_override       TEXT,"
        "  created_at          BIGINT NOT NULL,"
        "  resolved_at         BIGINT"
        ");");

    exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_rebalance_symbol "
        "ON rebalance_events (symbol, created_at DESC);");

    printf("[DB] Schema ready\n");
    return 0;
}

void db_close(void)
{
    if (g_conn)
    {
        PQfinish(g_conn);
        g_conn = NULL;
        printf("[DB] Disconnected\n");
    }
}

/* ── Portfolio ───────────────────────────────────────────────── */

int db_portfolio_save(const char *symbol, double shares, double avg_price)
{
    if (!g_conn)
        return -1;

    char s_shares[64], s_avg[64], s_ts[32];
    snprintf(s_shares, sizeof(s_shares), "%.8f", shares);
    snprintf(s_avg, sizeof(s_avg), "%.8f", avg_price);
    snprintf(s_ts, sizeof(s_ts), "%lld", (long long)now_ms());

    const char *vals[] = {symbol, s_shares, s_avg, s_ts};
    PGresult *res = exec_params(
        "INSERT INTO portfolio (symbol, shares, avg_price, updated_at) "
        "VALUES ($1, $2, $3, $4) "
        "ON CONFLICT (symbol) DO UPDATE SET "
        "  shares=EXCLUDED.shares, avg_price=EXCLUDED.avg_price, "
        "  updated_at=EXCLUDED.updated_at;",
        4, vals);

    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK) ? 0 : -1;
    PQclear(res);
    return ok;
}

int db_portfolio_delete(const char *symbol)
{
    if (!g_conn)
        return -1;
    const char *vals[] = {symbol};
    PGresult *res = exec_params(
        "DELETE FROM portfolio WHERE symbol=$1;", 1, vals);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK) ? 0 : -1;
    PQclear(res);
    return ok;
}

int db_portfolio_load(void)
{
    if (!g_conn)
        return -1;

    PGresult *res = PQexec(g_conn,
                           "SELECT symbol, shares, avg_price FROM portfolio;");
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        PQclear(res);
        return -1;
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.holding_count = 0;
    int rows = PQntuples(res);
    for (int i = 0; i < rows && i < MAX_HOLDINGS; i++)
    {
        Holding *h = &g_state.holdings[g_state.holding_count++];
        strncpy(h->symbol, PQgetvalue(res, i, 0), MAX_SYMBOL_LEN - 1);
        h->shares = atof(PQgetvalue(res, i, 1));
        h->avg_price = atof(PQgetvalue(res, i, 2));
    }
    int count = g_state.holding_count;
    pthread_mutex_unlock(&g_state.lock);

    PQclear(res);
    printf("[DB] Loaded %d portfolio holdings\n", count);
    return count;
}

/* ── Alerts ──────────────────────────────────────────────────── */

int db_alert_save(int id, const char *symbol,
                  AlertType type, double trigger_price)
{
    if (!g_conn)
        return -1;

    char s_id[32], s_trigger[64], s_ts[32];
    snprintf(s_id, sizeof(s_id), "%d", id);
    snprintf(s_trigger, sizeof(s_trigger), "%.8f", trigger_price);
    snprintf(s_ts, sizeof(s_ts), "%lld", (long long)now_ms());
    const char *cond = (type == ALERT_ABOVE) ? "above" : "below";

    const char *vals[] = {s_id, symbol, cond, s_trigger, s_ts};
    PGresult *res = exec_params(
        "INSERT INTO alerts (id, symbol, condition, trigger_price, created_at) "
        "VALUES ($1, $2, $3, $4, $5) "
        "ON CONFLICT (id) DO UPDATE SET "
        "  trigger_price=EXCLUDED.trigger_price, active=TRUE;",
        5, vals);

    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK) ? 0 : -1;
    PQclear(res);
    return ok;
}

int db_alert_delete(int id)
{
    if (!g_conn)
        return -1;
    char s_id[32];
    snprintf(s_id, sizeof(s_id), "%d", id);
    const char *vals[] = {s_id};
    PGresult *res = exec_params(
        "DELETE FROM alerts WHERE id=$1;", 1, vals);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK) ? 0 : -1;
    PQclear(res);
    return ok;
}

int db_alert_set_fired(int id)
{
    if (!g_conn)
        return -1;
    char s_id[32], s_ts[32];
    snprintf(s_id, sizeof(s_id), "%d", id);
    snprintf(s_ts, sizeof(s_ts), "%lld", (long long)now_ms());
    const char *vals[] = {s_ts, s_id};
    PGresult *res = exec_params(
        "UPDATE alerts SET active=FALSE, fired_at=$1 WHERE id=$2;",
        2, vals);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK) ? 0 : -1;
    PQclear(res);
    return ok;
}

int db_alerts_load(void)
{
    if (!g_conn)
        return -1;

    PGresult *res = PQexec(g_conn,
                           "SELECT id, symbol, condition, trigger_price "
                           "FROM alerts WHERE active=TRUE;");
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        PQclear(res);
        return -1;
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.alert_count = 0;
    g_state.next_alert_id = 1;
    int rows = PQntuples(res);

    for (int i = 0; i < rows && i < MAX_ALERTS; i++)
    {
        AlertRecord *a = &g_state.alerts[g_state.alert_count++];
        a->id = atoi(PQgetvalue(res, i, 0));
        strncpy(a->symbol, PQgetvalue(res, i, 1), MAX_SYMBOL_LEN - 1);
        a->type = strcmp(PQgetvalue(res, i, 2), "above") == 0
                      ? ALERT_ABOVE
                      : ALERT_BELOW;
        a->trigger_price = atof(PQgetvalue(res, i, 3));
        a->active = 1;
        if (a->id >= g_state.next_alert_id)
            g_state.next_alert_id = a->id + 1;
    }
    int count = g_state.alert_count;
    pthread_mutex_unlock(&g_state.lock);

    PQclear(res);
    printf("[DB] Loaded %d active alerts\n", count);
    return count;
}

/* ── Price cache ─────────────────────────────────────────────── */

int db_cache_store(const char *symbol, const char *timespan,
                   const OHLCBar *bars, int count)
{
    if (!g_conn || count <= 0)
        return 0;

    exec_sql("BEGIN;");
    int stored = 0;

    for (int i = 0; i < count; i++)
    {
        char s_bt[32], s_o[32], s_h[32], s_l[32], s_c[32], s_v[32];
        snprintf(s_bt, sizeof(s_bt), "%lld", (long long)bars[i].timestamp);
        snprintf(s_o, sizeof(s_o), "%.8f", bars[i].open);
        snprintf(s_h, sizeof(s_h), "%.8f", bars[i].high);
        snprintf(s_l, sizeof(s_l), "%.8f", bars[i].low);
        snprintf(s_c, sizeof(s_c), "%.8f", bars[i].close);
        snprintf(s_v, sizeof(s_v), "%lld", (long long)bars[i].volume);

        const char *vals[] = {symbol, timespan, s_bt, s_o, s_h, s_l, s_c, s_v};
        PGresult *res = exec_params(
            "INSERT INTO price_cache "
            "  (symbol,timespan,bar_time,open,high,low,close,volume) "
            "VALUES ($1,$2,$3,$4,$5,$6,$7,$8) "
            "ON CONFLICT (symbol,timespan,bar_time) DO UPDATE SET "
            "  open=$4, high=$5, low=$6, close=$7, volume=$8;",
            8, vals);
        if (PQresultStatus(res) == PGRES_COMMAND_OK)
            stored++;
        PQclear(res);
    }

    exec_sql("COMMIT;");
    return stored;
}

int db_cache_load(const char *symbol, const char *timespan,
                  int64_t from_ms, int64_t to_ms,
                  OHLCBar *bars_out, int max_bars)
{
    if (!g_conn)
        return -1;

    char s_from[32], s_to[32], s_lim[16];
    snprintf(s_from, sizeof(s_from), "%lld", (long long)from_ms);
    snprintf(s_to, sizeof(s_to), "%lld", (long long)to_ms);
    snprintf(s_lim, sizeof(s_lim), "%d", max_bars);

    const char *vals[] = {symbol, timespan, s_from, s_to, s_lim};
    PGresult *res = exec_params(
        "SELECT bar_time,open,high,low,close,volume "
        "FROM price_cache "
        "WHERE symbol=$1 AND timespan=$2 "
        "  AND bar_time>=$3 AND bar_time<=$4 "
        "ORDER BY bar_time ASC LIMIT $5;",
        5, vals);

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        PQclear(res);
        return -1;
    }

    int count = PQntuples(res);
    if (count > max_bars)
        count = max_bars;
    for (int i = 0; i < count; i++)
    {
        bars_out[i].timestamp = atoll(PQgetvalue(res, i, 0));
        bars_out[i].open = atof(PQgetvalue(res, i, 1));
        bars_out[i].high = atof(PQgetvalue(res, i, 2));
        bars_out[i].low = atof(PQgetvalue(res, i, 3));
        bars_out[i].close = atof(PQgetvalue(res, i, 4));
        bars_out[i].volume = atoll(PQgetvalue(res, i, 5));
    }
    PQclear(res);
    return count;
}

/* ── Order history ───────────────────────────────────────────── */

int db_order_insert(const char *symbol, OrderSide side,
                    OrderType type, double qty, double price)
{
    if (!g_conn)
        return -1;

    const char *s_side = (side == ORDER_BUY) ? "buy" : "sell";
    const char *s_type = (type == ORDER_MARKET) ? "market" : "limit";
    char s_qty[32], s_price[32], s_ts[32];
    snprintf(s_qty, sizeof(s_qty), "%.8f", qty);
    snprintf(s_price, sizeof(s_price), "%.8f", price);
    snprintf(s_ts, sizeof(s_ts), "%lld", (long long)now_ms());

    const char *vals[] = {symbol, s_side, s_type, s_qty, s_price, s_ts};
    PGresult *res = exec_params(
        "INSERT INTO order_history "
        "  (symbol,side,order_type,quantity,price,created_at) "
        "VALUES ($1,$2,$3,$4,$5,$6) RETURNING id;",
        6, vals);

    int id = -1;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
        id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);
    return id;
}

int db_order_update_status(int id, OrderStatus status,
                           double filled_price, int64_t filled_at)
{
    if (!g_conn)
        return -1;

    const char *s_status =
        status == ORDER_FILLED ? "filled" : status == ORDER_CANCELLED ? "cancelled"
                                                                      : "pending";

    char s_id[32], s_fp[32], s_fat[32];
    snprintf(s_id, sizeof(s_id), "%d", id);
    snprintf(s_fp, sizeof(s_fp), "%.8f", filled_price);
    snprintf(s_fat, sizeof(s_fat), "%lld", (long long)filled_at);

    const char *vals[] = {s_status, s_fp, s_fat, s_id};
    PGresult *res = exec_params(
        "UPDATE order_history "
        "SET status=$1, filled_price=$2, filled_at=$3 "
        "WHERE id=$4;",
        4, vals);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK) ? 0 : -1;
    PQclear(res);
    return ok;
}

int db_orders_load(OrderRecord *out, int max_orders)
{
    if (!g_conn)
        return -1;

    char s_lim[16];
    snprintf(s_lim, sizeof(s_lim), "%d", max_orders);
    const char *vals[] = {s_lim};
    PGresult *res = exec_params(
        "SELECT id,symbol,side,order_type,quantity,price,"
        "       status,created_at,filled_at,filled_price "
        "FROM order_history ORDER BY created_at DESC LIMIT $1;",
        1, vals);

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        PQclear(res);
        return -1;
    }

    int count = PQntuples(res);
    if (count > max_orders)
        count = max_orders;
    for (int i = 0; i < count; i++)
    {
        OrderRecord *o = &out[i];
        o->id = atoi(PQgetvalue(res, i, 0));
        strncpy(o->symbol, PQgetvalue(res, i, 1), MAX_SYMBOL_LEN - 1);
        o->side = strcmp(PQgetvalue(res, i, 2), "buy") == 0
                      ? ORDER_BUY
                      : ORDER_SELL;
        o->order_type = strcmp(PQgetvalue(res, i, 3), "market") == 0
                            ? ORDER_MARKET
                            : ORDER_LIMIT;
        o->quantity = atof(PQgetvalue(res, i, 4));
        o->price = atof(PQgetvalue(res, i, 5));
        const char *st = PQgetvalue(res, i, 6);
        o->status = strcmp(st, "filled") == 0 ? ORDER_FILLED : strcmp(st, "cancelled") == 0 ? ORDER_CANCELLED
                                                                                            : ORDER_PENDING;
        o->created_at = atoll(PQgetvalue(res, i, 7));
        o->filled_at = PQgetisnull(res, i, 8) ? 0
                                              : atoll(PQgetvalue(res, i, 8));
        o->filled_price = PQgetisnull(res, i, 9) ? 0.0
                                                 : atof(PQgetvalue(res, i, 9));
    }
    PQclear(res);
    return count;
}

/* ── Heston scores ───────────────────────────────────────────── */

int db_heston_save(long long run_id,
                   const char *symbol,
                   double expected_return,
                   double forward_vol,
                   double es_95,
                   double prob_loss_5,
                   int    n_paths_used,
                   int    converged)
{
    if (!g_conn || !symbol) return -1;

    char s_run[32], s_er[32], s_fv[32], s_es[32], s_pl[32];
    char s_n[16],  s_conv[8], s_ts[32];
    snprintf(s_run,  sizeof(s_run),  "%lld", run_id);
    snprintf(s_er,   sizeof(s_er),   "%.10f", expected_return);
    snprintf(s_fv,   sizeof(s_fv),   "%.10f", forward_vol);
    snprintf(s_es,   sizeof(s_es),   "%.10f", es_95);
    snprintf(s_pl,   sizeof(s_pl),   "%.10f", prob_loss_5);
    snprintf(s_n,    sizeof(s_n),    "%d",    n_paths_used);
    snprintf(s_conv, sizeof(s_conv), "%s",    converged ? "true" : "false");
    snprintf(s_ts,   sizeof(s_ts),   "%lld",  (long long)now_ms());

    const char *vals[] = { s_run, symbol, s_er, s_fv, s_es,
                           s_pl,  s_n,    s_conv, s_ts };
    PGresult *res = exec_params(
        "INSERT INTO heston_scores "
        "  (run_id, symbol, expected_return, forward_vol, es_95, "
        "   prob_loss_5, n_paths_used, converged, created_at) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9) "
        "ON CONFLICT (run_id, symbol) DO UPDATE SET "
        "  expected_return = EXCLUDED.expected_return,"
        "  forward_vol     = EXCLUDED.forward_vol,"
        "  es_95           = EXCLUDED.es_95,"
        "  prob_loss_5     = EXCLUDED.prob_loss_5,"
        "  n_paths_used    = EXCLUDED.n_paths_used,"
        "  converged       = EXCLUDED.converged,"
        "  created_at      = EXCLUDED.created_at;",
        9, vals);

    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK) ? 0 : -1;
    PQclear(res);
    return ok;
}

/* ── Rebalance events ─────────────────────────────────────────── */

long long db_rebalance_save(const char *symbol,
                            const char *decision,
                            const char *suggested_action,
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
                            const char *debrief)
{
    if (!g_conn || !symbol || !decision || !suggested_action) return -1;

    char s_old[32], s_new[32], s_exit[32];
    char s_obs[32], s_clr[32];
    char s_gap[32], s_llm[32], s_brc[32], s_hor[32];
    char s_dh[16], s_ih[16], s_ts[32];

    snprintf(s_old, sizeof(s_old), "%.10f", old_blend);
    snprintf(s_new, sizeof(s_new), "%.10f", new_blend);
    snprintf(s_exit,sizeof(s_exit),"%.10f", exit_threshold);
    snprintf(s_obs, sizeof(s_obs), "%.10f", obscurity);
    snprintf(s_clr, sizeof(s_clr), "%.10f", clarity);
    snprintf(s_gap, sizeof(s_gap), "%.10f", score_gap_clarity);
    snprintf(s_llm, sizeof(s_llm), "%.10f", llm_agreement);
    snprintf(s_brc, sizeof(s_brc), "%.10f", heston_breach);
    snprintf(s_hor, sizeof(s_hor), "%.10f", horizon_maturity);
    snprintf(s_dh,  sizeof(s_dh),  "%d",    days_held);
    snprintf(s_ih,  sizeof(s_ih),  "%d",    intended_hold_days);
    snprintf(s_ts,  sizeof(s_ts),  "%lld",  (long long)now_ms());

    const char *vals[] = {
        symbol, decision, suggested_action, action_taken_or_null,
        s_old, s_new, s_exit, s_obs, s_clr, primary_driver,
        s_gap, s_llm, s_brc, s_hor, s_dh, s_ih,
        debrief, s_ts
    };
    PGresult *res = exec_params(
        "INSERT INTO rebalance_events "
        "  (symbol, decision, suggested_action, action_taken, "
        "   old_blend, new_blend, exit_threshold, obscurity, clarity, "
        "   primary_driver, score_gap_clarity, llm_agreement, "
        "   heston_breach, horizon_maturity, days_held, intended_hold_days, "
        "   debrief, created_at) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,"
        "        $11,$12,$13,$14,$15,$16,$17,$18) "
        "RETURNING id;",
        18, vals);

    long long id = -1;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
        id = atoll(PQgetvalue(res, 0, 0));
    PQclear(res);
    return id;
}

int db_rebalance_resolve(long long  event_id,
                         const char *action_taken,
                         const char *user_override_or_null)
{
    if (!g_conn || event_id < 0 || !action_taken) return -1;

    char s_id[32], s_ts[32];
    snprintf(s_id, sizeof(s_id), "%lld", event_id);
    snprintf(s_ts, sizeof(s_ts), "%lld", (long long)now_ms());

    const char *vals[] = { action_taken, user_override_or_null, s_ts, s_id };
    PGresult *res = exec_params(
        "UPDATE rebalance_events "
        "SET action_taken = $1, user_override = $2, resolved_at = $3 "
        "WHERE id = $4;",
        4, vals);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK) ? 0 : -1;
    PQclear(res);
    return ok;
}
