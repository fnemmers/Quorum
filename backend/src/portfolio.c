/*
 * portfolio.c  --  Paper trading engine + live holdings ledger.
 *
 * Persistence: PostgreSQL via db.c.  Pricing: latest quote from
 * market_data if the symbol is subscribed, else polygon_rest_snapshot,
 * else the most recent daily close from price_cache.
 */

#include "portfolio.h"
#include "db.h"
#include "market_data.h"
#include "polygon_rest.h"

#include <libpq-fe.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#define PAPER_ACCOUNT_ID  1

/* ── Helpers ────────────────────────────────────────────────── */

static int64_t now_ms(void) { return (int64_t)time(NULL) * 1000LL; }

static void upper(char *s) {
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/*
 * Get the most-recent price for `symbol`.  Returns 0 on success and
 * writes the price to *out_px.  Falls back through:
 *   1. live quote (market_data)
 *   2. Polygon REST snapshot
 *   3. last daily close in price_cache
 */
static int fetch_price(const char *symbol, double *out_px) {
    /* 1) live quote */
    pthread_mutex_lock(&g_state.lock);
    int idx = market_find_symbol(symbol);
    if (idx >= 0 && g_state.quotes[idx].valid) {
        *out_px = g_state.quotes[idx].price;
        pthread_mutex_unlock(&g_state.lock);
        return 0;
    }
    pthread_mutex_unlock(&g_state.lock);

    /* 2) REST snapshot */
    Quote q = {0};
    if (polygon_rest_snapshot(symbol, &q) == 0 && q.price > 0) {
        *out_px = q.price;
        return 0;
    }

    /* 3) last cached daily close */
    int64_t now = now_ms();
    int64_t back = now - 30LL * 86400LL * 1000LL;
    OHLCBar bars[64];
    int n = db_cache_load(symbol, "day", back, now, bars, 64);
    if (n > 0) {
        *out_px = bars[n - 1].close;
        return 0;
    }
    return -1;
}

/* ── Init / schema ──────────────────────────────────────────── */

int portfolio_init(double initial_cash) {
    PGconn *c = db_get_conn();
    if (!c) return -1;

    const char *ddl[] = {
        "CREATE TABLE IF NOT EXISTS paper_account ("
        "  id            INT PRIMARY KEY,"
        "  cash          DOUBLE PRECISION NOT NULL,"
        "  initial_cash  DOUBLE PRECISION NOT NULL,"
        "  updated_at    BIGINT           NOT NULL"
        ");",

        "CREATE TABLE IF NOT EXISTS paper_positions ("
        "  symbol      TEXT PRIMARY KEY,"
        "  qty         DOUBLE PRECISION NOT NULL,"
        "  avg_cost    DOUBLE PRECISION NOT NULL,"
        "  updated_at  BIGINT           NOT NULL"
        ");",

        "CREATE TABLE IF NOT EXISTS paper_orders ("
        "  id          BIGSERIAL PRIMARY KEY,"
        "  symbol      TEXT             NOT NULL,"
        "  side        TEXT             NOT NULL,"
        "  qty         DOUBLE PRECISION NOT NULL,"
        "  fill_price  DOUBLE PRECISION NOT NULL,"
        "  cash_delta  DOUBLE PRECISION NOT NULL,"
        "  created_at  BIGINT           NOT NULL"
        ");",

        "CREATE INDEX IF NOT EXISTS idx_paper_orders_created "
        "ON paper_orders (created_at DESC);",

        "CREATE TABLE IF NOT EXISTS live_holdings ("
        "  symbol      TEXT PRIMARY KEY,"
        "  qty         DOUBLE PRECISION NOT NULL,"
        "  avg_cost    DOUBLE PRECISION NOT NULL,"
        "  notes       TEXT             NOT NULL DEFAULT '',"
        "  updated_at  BIGINT           NOT NULL"
        ");",
    };
    for (size_t i = 0; i < sizeof(ddl) / sizeof(ddl[0]); i++) {
        PGresult *r = PQexec(c, ddl[i]);
        if (PQresultStatus(r) != PGRES_COMMAND_OK) {
            fprintf(stderr, "[PORTFOLIO] ddl error: %s\n", PQerrorMessage(c));
            PQclear(r);
            return -1;
        }
        PQclear(r);
    }

    /* Seed paper account row (only if missing) */
    char cash_s[64], init_s[64], ts_s[32], id_s[8];
    snprintf(id_s,   sizeof(id_s),   "%d",   PAPER_ACCOUNT_ID);
    snprintf(cash_s, sizeof(cash_s), "%.6f", initial_cash);
    snprintf(init_s, sizeof(init_s), "%.6f", initial_cash);
    snprintf(ts_s,   sizeof(ts_s),   "%lld", (long long)now_ms());
    const char *vals[4] = { id_s, cash_s, init_s, ts_s };

    PGresult *r = PQexecParams(c,
        "INSERT INTO paper_account (id, cash, initial_cash, updated_at) "
        "VALUES ($1, $2, $3, $4) "
        "ON CONFLICT (id) DO NOTHING;",
        4, NULL, vals, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        fprintf(stderr, "[PORTFOLIO] seed error: %s\n", PQerrorMessage(c));
        PQclear(r);
        return -1;
    }
    PQclear(r);

    printf("[PORTFOLIO] schema ready (initial paper cash = $%.2f)\n",
           initial_cash);
    return 0;
}

/* ── Paper: order ───────────────────────────────────────────── */

static int load_position(const char *symbol, double *qty_out,
                         double *avg_cost_out) {
    PGconn *c = db_get_conn();
    if (!c) return -1;
    const char *vals[1] = { symbol };
    PGresult *r = PQexecParams(c,
        "SELECT qty, avg_cost FROM paper_positions WHERE symbol = $1;",
        1, NULL, vals, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        PQclear(r);
        return -1;
    }
    if (PQntuples(r) == 0) {
        *qty_out = 0.0; *avg_cost_out = 0.0;
        PQclear(r);
        return 0;
    }
    *qty_out      = atof(PQgetvalue(r, 0, 0));
    *avg_cost_out = atof(PQgetvalue(r, 0, 1));
    PQclear(r);
    return 0;
}

static int load_cash(double *cash_out) {
    PGconn *c = db_get_conn();
    if (!c) return -1;
    char id_s[8]; snprintf(id_s, sizeof(id_s), "%d", PAPER_ACCOUNT_ID);
    const char *vals[1] = { id_s };
    PGresult *r = PQexecParams(c,
        "SELECT cash FROM paper_account WHERE id = $1;",
        1, NULL, vals, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r);
        return -1;
    }
    *cash_out = atof(PQgetvalue(r, 0, 0));
    PQclear(r);
    return 0;
}

int portfolio_paper_order(const char *symbol, const char *side, double qty,
                          char *err_out, int err_len) {
    if (!symbol || !side) {
        if (err_out) snprintf(err_out, err_len, "missing arg");
        return -1;
    }
    if (!(qty > 0.0) || !isfinite(qty)) {
        if (err_out) snprintf(err_out, err_len, "qty must be > 0");
        return -1;
    }
    int is_buy;
    if      (!strcasecmp(side, "buy"))  is_buy = 1;
    else if (!strcasecmp(side, "sell")) is_buy = 0;
    else {
        if (err_out) snprintf(err_out, err_len, "side must be buy|sell");
        return -1;
    }

    char sym[MAX_SYMBOL_LEN];
    strncpy(sym, symbol, sizeof(sym) - 1);
    sym[sizeof(sym) - 1] = '\0';
    upper(sym);

    double px = 0.0;
    if (fetch_price(sym, &px) != 0 || !(px > 0.0)) {
        if (err_out) snprintf(err_out, err_len, "no price for %s", sym);
        return -1;
    }

    double cash = 0.0;
    if (load_cash(&cash) != 0) {
        if (err_out) snprintf(err_out, err_len, "cash lookup failed");
        return -1;
    }
    double pos_qty = 0.0, pos_avg = 0.0;
    if (load_position(sym, &pos_qty, &pos_avg) != 0) {
        if (err_out) snprintf(err_out, err_len, "position lookup failed");
        return -1;
    }

    double notional   = qty * px;
    double cash_delta = is_buy ? -notional : +notional;

    if (is_buy) {
        if (notional > cash + 1e-6) {
            if (err_out) snprintf(err_out, err_len,
                "insufficient cash: need $%.2f, have $%.2f", notional, cash);
            return -1;
        }
    } else {
        if (qty > pos_qty + 1e-9) {
            if (err_out) snprintf(err_out, err_len,
                "insufficient qty: sell %.4f, hold %.4f", qty, pos_qty);
            return -1;
        }
    }

    /* Compute new position */
    double new_qty, new_avg;
    if (is_buy) {
        new_qty = pos_qty + qty;
        new_avg = (new_qty > 0.0)
            ? (pos_qty * pos_avg + qty * px) / new_qty
            : 0.0;
    } else {
        new_qty = pos_qty - qty;
        new_avg = (new_qty > 1e-9) ? pos_avg : 0.0;
    }

    PGconn *c = db_get_conn();
    if (!c) {
        if (err_out) snprintf(err_out, err_len, "db offline");
        return -1;
    }

    /* Transaction: update cash, upsert/delete position, insert order. */
    PGresult *r = PQexec(c, "BEGIN;");
    PQclear(r);

    char cash_new_s[64], id_s[8], ts_s[32];
    snprintf(id_s,       sizeof(id_s),       "%d",   PAPER_ACCOUNT_ID);
    snprintf(cash_new_s, sizeof(cash_new_s), "%.6f", cash + cash_delta);
    snprintf(ts_s,       sizeof(ts_s),       "%lld", (long long)now_ms());
    const char *upd_cash[3] = { cash_new_s, ts_s, id_s };
    r = PQexecParams(c,
        "UPDATE paper_account SET cash = $1, updated_at = $2 WHERE id = $3;",
        3, NULL, upd_cash, NULL, NULL, 0);
    PQclear(r);

    if (new_qty > 1e-9) {
        char qty_s[64], avg_s[64];
        snprintf(qty_s, sizeof(qty_s), "%.9f", new_qty);
        snprintf(avg_s, sizeof(avg_s), "%.9f", new_avg);
        const char *ups[4] = { sym, qty_s, avg_s, ts_s };
        r = PQexecParams(c,
            "INSERT INTO paper_positions (symbol, qty, avg_cost, updated_at) "
            "VALUES ($1, $2, $3, $4) "
            "ON CONFLICT (symbol) DO UPDATE "
            "  SET qty = EXCLUDED.qty, "
            "      avg_cost = EXCLUDED.avg_cost, "
            "      updated_at = EXCLUDED.updated_at;",
            4, NULL, ups, NULL, NULL, 0);
        PQclear(r);
    } else {
        const char *del[1] = { sym };
        r = PQexecParams(c,
            "DELETE FROM paper_positions WHERE symbol = $1;",
            1, NULL, del, NULL, NULL, 0);
        PQclear(r);
    }

    char qty_s2[64], px_s[64], delta_s[64];
    snprintf(qty_s2,  sizeof(qty_s2),  "%.9f", qty);
    snprintf(px_s,    sizeof(px_s),    "%.6f", px);
    snprintf(delta_s, sizeof(delta_s), "%.6f", cash_delta);
    const char *ord[6] = { sym, is_buy ? "buy" : "sell",
                           qty_s2, px_s, delta_s, ts_s };
    r = PQexecParams(c,
        "INSERT INTO paper_orders "
        "  (symbol, side, qty, fill_price, cash_delta, created_at) "
        "VALUES ($1, $2, $3, $4, $5, $6);",
        6, NULL, ord, NULL, NULL, 0);
    PQclear(r);

    r = PQexec(c, "COMMIT;");
    PQclear(r);

    return 0;
}

int portfolio_paper_reset(void) {
    PGconn *c = db_get_conn();
    if (!c) return -1;

    PGresult *r;
    r = PQexec(c, "BEGIN;"); PQclear(r);
    r = PQexec(c, "DELETE FROM paper_orders;");    PQclear(r);
    r = PQexec(c, "DELETE FROM paper_positions;"); PQclear(r);

    char ts_s[32]; snprintf(ts_s, sizeof(ts_s), "%lld", (long long)now_ms());
    char id_s[8];  snprintf(id_s, sizeof(id_s),  "%d",   PAPER_ACCOUNT_ID);
    const char *vals[2] = { ts_s, id_s };
    r = PQexecParams(c,
        "UPDATE paper_account SET cash = initial_cash, updated_at = $1 "
        "WHERE id = $2;",
        2, NULL, vals, NULL, NULL, 0);
    PQclear(r);

    r = PQexec(c, "COMMIT;"); PQclear(r);
    return 0;
}

/* ── Paper: state ───────────────────────────────────────────── */

int portfolio_paper_state(cJSON *out) {
    if (!out) return -1;
    PGconn *c = db_get_conn();
    if (!c) return -1;

    /* account row */
    char id_s[8]; snprintf(id_s, sizeof(id_s), "%d", PAPER_ACCOUNT_ID);
    const char *acc_v[1] = { id_s };
    PGresult *r = PQexecParams(c,
        "SELECT cash, initial_cash FROM paper_account WHERE id = $1;",
        1, NULL, acc_v, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r);
        return -1;
    }
    double cash         = atof(PQgetvalue(r, 0, 0));
    double initial_cash = atof(PQgetvalue(r, 0, 1));
    PQclear(r);

    cJSON_AddStringToObject(out, "type",         "paper_state");
    cJSON_AddNumberToObject(out, "cash",         cash);
    cJSON_AddNumberToObject(out, "initial_cash", initial_cash);

    /* positions */
    r = PQexec(c,
        "SELECT symbol, qty, avg_cost, updated_at FROM paper_positions "
        "ORDER BY symbol;");
    cJSON *jpos = cJSON_AddArrayToObject(out, "positions");
    double positions_value  = 0.0;
    double unrealized_pnl   = 0.0;
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        int n = PQntuples(r);
        for (int i = 0; i < n; i++) {
            const char *sym = PQgetvalue(r, i, 0);
            double qty      = atof(PQgetvalue(r, i, 1));
            double avg_cost = atof(PQgetvalue(r, i, 2));
            int64_t upd     = strtoll(PQgetvalue(r, i, 3), NULL, 10);

            double px = 0.0;
            int have_px = (fetch_price(sym, &px) == 0);
            double mv    = have_px ? qty * px : qty * avg_cost;
            double upnl  = have_px ? qty * (px - avg_cost) : 0.0;
            double upct  = (avg_cost > 0.0 && have_px)
                ? (px - avg_cost) / avg_cost : 0.0;

            positions_value += mv;
            unrealized_pnl  += upnl;

            cJSON *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "symbol",         sym);
            cJSON_AddNumberToObject(row, "qty",            qty);
            cJSON_AddNumberToObject(row, "avg_cost",       avg_cost);
            cJSON_AddNumberToObject(row, "last_price",     have_px ? px : 0.0);
            cJSON_AddBoolToObject  (row, "has_price",      have_px);
            cJSON_AddNumberToObject(row, "market_value",   mv);
            cJSON_AddNumberToObject(row, "unrealized_pnl", upnl);
            cJSON_AddNumberToObject(row, "unrealized_pct", upct);
            cJSON_AddNumberToObject(row, "updated_at",     (double)upd);
            cJSON_AddItemToArray(jpos, row);
        }
    }
    PQclear(r);

    double equity   = cash + positions_value;
    double total    = equity - initial_cash;
    double ret_pct  = (initial_cash > 0.0) ? total / initial_cash : 0.0;
    cJSON_AddNumberToObject(out, "positions_value",  positions_value);
    cJSON_AddNumberToObject(out, "equity",           equity);
    cJSON_AddNumberToObject(out, "unrealized_pnl",   unrealized_pnl);
    cJSON_AddNumberToObject(out, "total_pnl",        total);
    cJSON_AddNumberToObject(out, "total_return_pct", ret_pct);

    /* recent orders (last 100) */
    r = PQexec(c,
        "SELECT id, symbol, side, qty, fill_price, cash_delta, created_at "
        "FROM paper_orders ORDER BY id DESC LIMIT 100;");
    cJSON *jord = cJSON_AddArrayToObject(out, "orders");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        int n = PQntuples(r);
        for (int i = 0; i < n; i++) {
            cJSON *row = cJSON_CreateObject();
            cJSON_AddNumberToObject(row, "id",
                strtod(PQgetvalue(r, i, 0), NULL));
            cJSON_AddStringToObject(row, "symbol",     PQgetvalue(r, i, 1));
            cJSON_AddStringToObject(row, "side",       PQgetvalue(r, i, 2));
            cJSON_AddNumberToObject(row, "qty",        atof(PQgetvalue(r, i, 3)));
            cJSON_AddNumberToObject(row, "fill_price", atof(PQgetvalue(r, i, 4)));
            cJSON_AddNumberToObject(row, "cash_delta", atof(PQgetvalue(r, i, 5)));
            cJSON_AddNumberToObject(row, "created_at",
                (double)strtoll(PQgetvalue(r, i, 6), NULL, 10));
            cJSON_AddItemToArray(jord, row);
        }
    }
    PQclear(r);

    return 0;
}

/* ── Live holdings ──────────────────────────────────────────── */

int portfolio_live_upsert(const char *symbol, double qty, double avg_cost,
                          const char *notes) {
    if (!symbol || !*symbol) return -1;
    PGconn *c = db_get_conn();
    if (!c) return -1;

    char sym[MAX_SYMBOL_LEN];
    strncpy(sym, symbol, sizeof(sym) - 1);
    sym[sizeof(sym) - 1] = '\0';
    upper(sym);

    if (!(qty > 0.0) || !isfinite(qty)) {
        const char *del[1] = { sym };
        PGresult *r = PQexecParams(c,
            "DELETE FROM live_holdings WHERE symbol = $1;",
            1, NULL, del, NULL, NULL, 0);
        PQclear(r);
        return 0;
    }

    char qty_s[64], avg_s[64], ts_s[32];
    snprintf(qty_s, sizeof(qty_s), "%.9f", qty);
    snprintf(avg_s, sizeof(avg_s), "%.9f", avg_cost);
    snprintf(ts_s,  sizeof(ts_s),  "%lld", (long long)now_ms());
    const char *ups[5] = { sym, qty_s, avg_s, notes ? notes : "", ts_s };

    PGresult *r = PQexecParams(c,
        "INSERT INTO live_holdings (symbol, qty, avg_cost, notes, updated_at) "
        "VALUES ($1, $2, $3, $4, $5) "
        "ON CONFLICT (symbol) DO UPDATE "
        "  SET qty = EXCLUDED.qty, "
        "      avg_cost = EXCLUDED.avg_cost, "
        "      notes = EXCLUDED.notes, "
        "      updated_at = EXCLUDED.updated_at;",
        5, NULL, ups, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        fprintf(stderr, "[PORTFOLIO] live upsert error: %s\n",
                PQerrorMessage(c));
        PQclear(r);
        return -1;
    }
    PQclear(r);
    return 0;
}

int portfolio_live_state(cJSON *out) {
    if (!out) return -1;
    PGconn *c = db_get_conn();
    if (!c) return -1;

    cJSON_AddStringToObject(out, "type", "live_holdings");
    cJSON *arr = cJSON_AddArrayToObject(out, "holdings");

    PGresult *r = PQexec(c,
        "SELECT symbol, qty, avg_cost, notes, updated_at "
        "FROM live_holdings ORDER BY symbol;");

    double total_cost = 0.0, total_mv = 0.0;
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        int n = PQntuples(r);
        for (int i = 0; i < n; i++) {
            const char *sym = PQgetvalue(r, i, 0);
            double qty      = atof(PQgetvalue(r, i, 1));
            double avg_cost = atof(PQgetvalue(r, i, 2));
            const char *nt  = PQgetvalue(r, i, 3);
            int64_t upd     = strtoll(PQgetvalue(r, i, 4), NULL, 10);

            double px = 0.0;
            int have_px = (fetch_price(sym, &px) == 0);
            double cost  = qty * avg_cost;
            double mv    = have_px ? qty * px : cost;
            double upnl  = have_px ? qty * (px - avg_cost) : 0.0;
            double upct  = (avg_cost > 0.0 && have_px)
                ? (px - avg_cost) / avg_cost : 0.0;
            total_cost += cost;
            total_mv   += mv;

            cJSON *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "symbol",         sym);
            cJSON_AddNumberToObject(row, "qty",            qty);
            cJSON_AddNumberToObject(row, "avg_cost",       avg_cost);
            cJSON_AddStringToObject(row, "notes",          nt);
            cJSON_AddNumberToObject(row, "last_price",     have_px ? px : 0.0);
            cJSON_AddBoolToObject  (row, "has_price",      have_px);
            cJSON_AddNumberToObject(row, "cost_basis",     cost);
            cJSON_AddNumberToObject(row, "market_value",   mv);
            cJSON_AddNumberToObject(row, "unrealized_pnl", upnl);
            cJSON_AddNumberToObject(row, "unrealized_pct", upct);
            cJSON_AddNumberToObject(row, "updated_at",     (double)upd);
            cJSON_AddItemToArray(arr, row);
        }
    }
    PQclear(r);

    cJSON_AddNumberToObject(out, "total_cost",     total_cost);
    cJSON_AddNumberToObject(out, "total_value",    total_mv);
    cJSON_AddNumberToObject(out, "unrealized_pnl", total_mv - total_cost);
    cJSON_AddNumberToObject(out, "unrealized_pct",
        total_cost > 0.0 ? (total_mv - total_cost) / total_cost : 0.0);

    return 0;
}
