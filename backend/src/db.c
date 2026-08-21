#include "db.h"
#include "market_data.h"
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static PGconn *g_conn = NULL;

PGconn *db_get_conn(void) { return g_conn; }

/* ── Helpers ─────────────────────────────────────────────────── */

static int exec_sql(const char *sql) {
    PGresult *res = PQexec(g_conn, sql);
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        fprintf(stderr, "[DB] Error: %s\n  SQL: %s\n",
                PQerrorMessage(g_conn), sql);
        PQclear(res);
        return -1;
    }
    PQclear(res);
    return 0;
}

static PGresult *exec_params(const char *sql, int nparams,
                             const char *const *vals) {
    PGresult *res = PQexecParams(g_conn, sql, nparams, NULL, vals,
                                 NULL, NULL, 0);
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        fprintf(stderr, "[DB] Query error: %s\n  SQL: %s\n",
                PQerrorMessage(g_conn), sql);
    }
    return res;
}

/* ── Init / schema ───────────────────────────────────────────── */

int db_init(const char *connstr) {
    g_conn = PQconnectdb(connstr);
    if (PQstatus(g_conn) != CONNECTION_OK) {
        fprintf(stderr, "[DB] Connection failed: %s\n",
                PQerrorMessage(g_conn));
        PQfinish(g_conn);
        g_conn = NULL;
        return -1;
    }
    printf("[DB] Connected to PostgreSQL: %s\n", PQdb(g_conn));

    /* ── Price cache (daily bars) ─────────────────────────────── */
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

    /* ── News crawler cache ──────────────────────────────────── */
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

    /* ── News-derived jump-parameter signal (Bates overlay) ──── */
    exec_sql(
        "CREATE TABLE IF NOT EXISTS news_jump_signal ("
        "  symbol         TEXT   NOT NULL,"
        "  as_of          BIGINT NOT NULL,"
        "  window_hours   INT    NOT NULL,"
        "  n_articles     INT    NOT NULL,"
        "  sentiment_avg  DOUBLE PRECISION NOT NULL DEFAULT 0,"
        "  event_class    TEXT   NOT NULL DEFAULT '',"
        "  lam_bump       DOUBLE PRECISION NOT NULL DEFAULT 0,"
        "  mu_j_bias      DOUBLE PRECISION NOT NULL DEFAULT 0,"
        "  sigma_j_bump   DOUBLE PRECISION NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (symbol, as_of)"
        ");");

    exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_news_jump_symbol_asof "
        "ON news_jump_signal (symbol, as_of DESC);");

    printf("[DB] Schema ready\n");
    return 0;
}

void db_close(void) {
    if (g_conn) {
        PQfinish(g_conn);
        g_conn = NULL;
        printf("[DB] Disconnected\n");
    }
}

/* ── Price cache ─────────────────────────────────────────────── */

int db_cache_store(const char *symbol, const char *timespan,
                   const OHLCBar *bars, int count) {
    if (!g_conn || count <= 0) return 0;

    exec_sql("BEGIN;");
    int stored = 0;

    for (int i = 0; i < count; i++) {
        char s_bt[32], s_o[32], s_h[32], s_l[32], s_c[32], s_v[32];
        snprintf(s_bt, sizeof(s_bt), "%lld", (long long)bars[i].timestamp);
        snprintf(s_o,  sizeof(s_o),  "%.8f", bars[i].open);
        snprintf(s_h,  sizeof(s_h),  "%.8f", bars[i].high);
        snprintf(s_l,  sizeof(s_l),  "%.8f", bars[i].low);
        snprintf(s_c,  sizeof(s_c),  "%.8f", bars[i].close);
        snprintf(s_v,  sizeof(s_v),  "%lld", (long long)bars[i].volume);

        const char *vals[] = { symbol, timespan, s_bt, s_o, s_h, s_l, s_c, s_v };
        PGresult *res = exec_params(
            "INSERT INTO price_cache "
            "  (symbol,timespan,bar_time,open,high,low,close,volume) "
            "VALUES ($1,$2,$3,$4,$5,$6,$7,$8) "
            "ON CONFLICT (symbol,timespan,bar_time) DO UPDATE SET "
            "  open=$4, high=$5, low=$6, close=$7, volume=$8;",
            8, vals);
        if (PQresultStatus(res) == PGRES_COMMAND_OK) stored++;
        PQclear(res);
    }

    exec_sql("COMMIT;");
    return stored;
}

int db_cache_load(const char *symbol, const char *timespan,
                  int64_t from_ms, int64_t to_ms,
                  OHLCBar *bars_out, int max_bars) {
    if (!g_conn) return -1;

    char s_from[32], s_to[32], s_lim[16];
    snprintf(s_from, sizeof(s_from), "%lld", (long long)from_ms);
    snprintf(s_to,   sizeof(s_to),   "%lld", (long long)to_ms);
    snprintf(s_lim,  sizeof(s_lim),  "%d",   max_bars);

    const char *vals[] = { symbol, timespan, s_from, s_to, s_lim };
    PGresult *res = exec_params(
        "SELECT bar_time,open,high,low,close,volume "
        "FROM price_cache "
        "WHERE symbol=$1 AND timespan=$2 "
        "  AND bar_time>=$3 AND bar_time<=$4 "
        "ORDER BY bar_time ASC LIMIT $5;",
        5, vals);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        return -1;
    }

    int count = PQntuples(res);
    if (count > max_bars) count = max_bars;
    for (int i = 0; i < count; i++) {
        bars_out[i].timestamp = atoll(PQgetvalue(res, i, 0));
        bars_out[i].open      = atof (PQgetvalue(res, i, 1));
        bars_out[i].high      = atof (PQgetvalue(res, i, 2));
        bars_out[i].low       = atof (PQgetvalue(res, i, 3));
        bars_out[i].close     = atof (PQgetvalue(res, i, 4));
        bars_out[i].volume    = atoll(PQgetvalue(res, i, 5));
    }
    PQclear(res);
    return count;
}
