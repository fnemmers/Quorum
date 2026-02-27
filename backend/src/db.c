#include "db.h"
#include "market_data.h"
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static PGconn *g_conn = NULL;

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
