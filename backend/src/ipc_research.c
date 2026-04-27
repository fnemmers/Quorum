/*
 * ipc_research.c  –  IPC handlers for the bot ensemble + backtest path.
 *
 * Owned commands (see ipc_research.h for the wire format):
 *   sp500_list
 *   bot_run_create
 *   bot_picks_ingest
 *   bot_run_finish
 *   aggregate_run
 *   backtest_run
 *
 * Each handler reads its params from the cJSON root, calls into the
 * appropriate module (bot_picks, aggregation, backtest, sp500_universe),
 * and writes a newline-terminated JSON line back to client_fd.
 */

#include "ipc_research.h"
#include "bot_picks.h"
#include "aggregation.h"
#include "backtest.h"
#include "sp500_universe.h"
#include "crawler.h"
#include "market_data.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
#else
  #include <sys/socket.h>
  #include <unistd.h>
#endif

/* ── Small helpers ──────────────────────────────────────────────── */

static void send_line(int fd, const char *json) {
    size_t len = strlen(json);
    char *line = (char *)malloc(len + 2);
    if (!line) return;
    memcpy(line, json, len);
    line[len]   = '\n';
    line[len+1] = '\0';
    send(fd, line, (int)(len + 1), 0);
    free(line);
}

static void send_obj(int fd, cJSON *obj) {
    char *str = cJSON_PrintUnformatted(obj);
    if (!str) return;
    send_line(fd, str);
    free(str);
}

static void send_error(int fd, const char *msg) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type",    "error");
    cJSON_AddStringToObject(o, "message", msg);
    send_obj(fd, o);
    cJSON_Delete(o);
}

/* ── sp500_list ─────────────────────────────────────────────────── */

static void cmd_sp500_list(int fd) {
    cJSON *o   = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "sp500_list");
    cJSON *arr = cJSON_AddArrayToObject(o, "tickers");

    const char *const *list = sp500_tickers();
    size_t n = sp500_count();
    for (size_t i = 0; i < n; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(list[i]));

    send_obj(fd, o);
    cJSON_Delete(o);
}

/* ── bot_runs_list ──────────────────────────────────────────────── */

static void cmd_bot_runs_list(int fd, cJSON *root) {
    cJSON *jl = cJSON_GetObjectItemCaseSensitive(root, "limit");
    int limit = (jl && cJSON_IsNumber(jl)) ? (int)jl->valuedouble : 50;
    if (limit <= 0 || limit > 500) limit = 50;

    BotRunInfo *rows = (BotRunInfo *)calloc((size_t)limit, sizeof(BotRunInfo));
    if (!rows) { send_error(fd, "oom"); return; }

    int n = bot_runs_list(rows, limit);
    if (n < 0) { free(rows); send_error(fd, "bot_runs_list failed"); return; }

    cJSON *o   = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "bot_runs_list");
    cJSON *arr = cJSON_AddArrayToObject(o, "runs");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "id",            (double)rows[i].id);
        cJSON_AddStringToObject(e, "label",         rows[i].label);
        cJSON_AddNumberToObject(e, "n_bots_target", rows[i].n_bots_target);
        cJSON_AddNumberToObject(e, "n_bots_actual", rows[i].n_bots_actual);
        cJSON_AddNumberToObject(e, "hold_days",     rows[i].hold_days);
        cJSON_AddNumberToObject(e, "started_at",    (double)rows[i].started_at);
        cJSON_AddNumberToObject(e, "finished_at",   (double)rows[i].finished_at);
        cJSON_AddItemToArray(arr, e);
    }
    send_obj(fd, o);
    cJSON_Delete(o);
    free(rows);
}

/* ── bot_run_create ─────────────────────────────────────────────── */

static void cmd_bot_run_create(int fd, cJSON *root) {
    cJSON *jl = cJSON_GetObjectItemCaseSensitive(root, "label");
    cJSON *jn = cJSON_GetObjectItemCaseSensitive(root, "n_bots_target");
    cJSON *jh = cJSON_GetObjectItemCaseSensitive(root, "hold_days");

    const char *label = (jl && cJSON_IsString(jl)) ? jl->valuestring : "unnamed";
    int n_bots = (jn && cJSON_IsNumber(jn)) ? (int)jn->valuedouble : 0;
    int hold   = (jh && cJSON_IsNumber(jh)) ? (int)jh->valuedouble : 30;

    int64_t run_id = bot_run_create(label, n_bots, hold);
    if (run_id < 0) {
        send_error(fd, "bot_run_create failed");
        return;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "bot_run_created");
    cJSON_AddNumberToObject(o, "run_id", (double)run_id);
    send_obj(fd, o);
    cJSON_Delete(o);
}

/* ── bot_picks_ingest ───────────────────────────────────────────── */

static void cmd_bot_picks_ingest(int fd, cJSON *root) {
    cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "run_id");
    cJSON *jb = cJSON_GetObjectItemCaseSensitive(root, "bot_index");
    cJSON *jp = cJSON_GetObjectItemCaseSensitive(root, "persona");
    cJSON *jx = cJSON_GetObjectItemCaseSensitive(root, "picks");

    if (!jr || !cJSON_IsNumber(jr) ||
        !jb || !cJSON_IsNumber(jb) ||
        !jx || !cJSON_IsArray(jx)) {
        send_error(fd, "bot_picks_ingest: missing run_id/bot_index/picks");
        return;
    }

    int64_t run_id    = (int64_t)jr->valuedouble;
    int     bot_index = (int)jb->valuedouble;
    const char *persona =
        (jp && cJSON_IsString(jp)) ? jp->valuestring : "default";

    int n = cJSON_GetArraySize(jx);
    if (n <= 0) { send_error(fd, "empty picks array"); return; }

    /* Build a flat const char* array, validating each entry against
     * the S&P 500 universe. Bots will sometimes hallucinate tickers —
     * we silently drop those rather than fail the whole batch. */
    const char **buf = (const char **)calloc((size_t)n, sizeof(char *));
    if (!buf) { send_error(fd, "oom"); return; }

    int kept = 0;
    for (int i = 0; i < n; i++) {
        cJSON *t = cJSON_GetArrayItem(jx, i);
        if (!t || !cJSON_IsString(t)) continue;
        if (!sp500_contains(t->valuestring)) continue;
        buf[kept++] = t->valuestring;
    }

    int rc = (kept > 0)
        ? bot_picks_insert_batch(run_id, bot_index, persona, buf, kept)
        : 0;
    free(buf);

    if (rc != 0) {
        send_error(fd, "bot_picks_insert_batch failed");
        return;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "bot_picks_ack");
    cJSON_AddNumberToObject(o, "run_id",    (double)run_id);
    cJSON_AddNumberToObject(o, "bot_index", bot_index);
    cJSON_AddNumberToObject(o, "n",         kept);
    cJSON_AddNumberToObject(o, "n_dropped", n - kept);
    send_obj(fd, o);
    cJSON_Delete(o);
}

/* ── bot_run_finish ─────────────────────────────────────────────── */

static void cmd_bot_run_finish(int fd, cJSON *root) {
    cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "run_id");
    cJSON *jn = cJSON_GetObjectItemCaseSensitive(root, "n_bots_actual");
    if (!jr || !cJSON_IsNumber(jr)) {
        send_error(fd, "bot_run_finish: missing run_id");
        return;
    }
    int64_t run_id = (int64_t)jr->valuedouble;
    int n_actual = (jn && cJSON_IsNumber(jn)) ? (int)jn->valuedouble : 0;

    if (bot_run_finish(run_id, n_actual) != 0) {
        send_error(fd, "bot_run_finish failed");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "bot_run_finished");
    cJSON_AddNumberToObject(o, "run_id", (double)run_id);
    send_obj(fd, o);
    cJSON_Delete(o);
}

/* ── aggregate_run ──────────────────────────────────────────────── */

static void cmd_aggregate_run(int fd, cJSON *root) {
    cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "run_id");
    cJSON *jk = cJSON_GetObjectItemCaseSensitive(root, "k");
    if (!jr || !cJSON_IsNumber(jr)) {
        send_error(fd, "aggregate_run: missing run_id");
        return;
    }
    int64_t run_id = (int64_t)jr->valuedouble;
    int k = (jk && cJSON_IsNumber(jk)) ? (int)jk->valuedouble : 20;
    if (k <= 0 || k > 500) k = 20;

    /* Pull every pick row for this run from Postgres, feed each one into
     * a fresh aggregator, then ask for top-K. The aggregator lives only
     * for the duration of this call — re-aggregating on demand keeps the
     * server stateless and lets multiple clients query the same run. */
    enum { MAX_PICKS = 50000 };
    char (*picks)[MAX_SYMBOL_LEN] =
        calloc(MAX_PICKS, sizeof(*picks));
    if (!picks) { send_error(fd, "oom"); return; }

    int n = bot_picks_load_run(run_id, picks, MAX_PICKS);
    if (n < 0) { free(picks); send_error(fd, "load_run failed"); return; }

    Aggregator *agg = agg_create(2048);
    if (!agg) { free(picks); send_error(fd, "agg_create failed"); return; }

    for (int i = 0; i < n; i++)
        agg_add_pick(agg, picks[i]);

    AggResult top[500];
    int got = agg_top_k(agg, top, k);

    cJSON *o   = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type",   "aggregate_result");
    cJSON_AddNumberToObject(o, "run_id", (double)run_id);
    cJSON_AddNumberToObject(o, "n_picks_total", (double)n);
    cJSON *arr = cJSON_AddArrayToObject(o, "top");
    for (int i = 0; i < got; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "symbol", top[i].symbol);
        cJSON_AddNumberToObject(e, "count",  top[i].count);
        cJSON_AddItemToArray(arr, e);
    }
    send_obj(fd, o);
    cJSON_Delete(o);

    agg_free(agg);
    free(picks);
}

/* ── backtest_run ───────────────────────────────────────────────── */

static void cmd_backtest_run(int fd, cJSON *root) {
    cJSON *jr  = cJSON_GetObjectItemCaseSensitive(root, "run_id");
    cJSON *jk  = cJSON_GetObjectItemCaseSensitive(root, "k");
    cJSON *jsd = cJSON_GetObjectItemCaseSensitive(root, "start_date");
    cJSON *jhd = cJSON_GetObjectItemCaseSensitive(root, "hold_days");

    if (!jr  || !cJSON_IsNumber(jr) ||
        !jsd || !cJSON_IsString(jsd) ||
        !jhd || !cJSON_IsNumber(jhd)) {
        send_error(fd, "backtest_run: need run_id, start_date, hold_days");
        return;
    }
    int64_t run_id     = (int64_t)jr->valuedouble;
    int     k          = (jk && cJSON_IsNumber(jk)) ? (int)jk->valuedouble : 20;
    const char *start  = jsd->valuestring;
    int     hold       = (int)jhd->valuedouble;

    /* Step 1: re-aggregate the run to get the top-K consensus tickers. */
    enum { MAX_PICKS = 50000 };
    char (*picks)[MAX_SYMBOL_LEN] =
        calloc(MAX_PICKS, sizeof(*picks));
    if (!picks) { send_error(fd, "oom"); return; }

    int n = bot_picks_load_run(run_id, picks, MAX_PICKS);
    if (n < 0) { free(picks); send_error(fd, "load_run failed"); return; }

    Aggregator *agg = agg_create(2048);
    if (!agg) { free(picks); send_error(fd, "agg_create failed"); return; }
    for (int i = 0; i < n; i++) agg_add_pick(agg, picks[i]);

    AggResult top[500];
    int got = agg_top_k(agg, top, k);
    free(picks);

    if (got <= 0) {
        agg_free(agg);
        send_error(fd, "no consensus picks");
        return;
    }

    /* Step 2: build a const char *[] of ticker pointers for backtest_run. */
    const char **tickers =
        (const char **)calloc((size_t)got, sizeof(char *));
    if (!tickers) { agg_free(agg); send_error(fd, "oom"); return; }
    for (int i = 0; i < got; i++) tickers[i] = top[i].symbol;

    BacktestResult res = {0};
    int rc = backtest_run(tickers, got, start, hold, &res);
    free(tickers);
    agg_free(agg);

    if (rc != 0) {
        send_error(fd, "backtest_run failed (check price_cache backfill)");
        return;
    }

    /* Step 3: persist + reply. */
    bot_backtest_save(run_id, (const struct BacktestResult *)&res);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type",         "backtest_result");
    cJSON_AddNumberToObject(o, "run_id",       (double)run_id);
    cJSON_AddStringToObject(o, "start_date",   res.start_date);
    cJSON_AddStringToObject(o, "end_date",     res.end_date);
    cJSON_AddNumberToObject(o, "hold_days",    res.hold_days);
    cJSON_AddNumberToObject(o, "n_used",       res.n_tickers_used);
    cJSON_AddNumberToObject(o, "n_skipped",    res.n_skipped);
    cJSON_AddNumberToObject(o, "port_return",  res.portfolio_return_pct);
    cJSON_AddNumberToObject(o, "bench_return", res.benchmark_return_pct);
    cJSON_AddNumberToObject(o, "alpha",        res.alpha_pct);
    cJSON_AddNumberToObject(o, "sharpe",       res.sharpe_ratio);
    cJSON_AddNumberToObject(o, "max_dd",       res.max_drawdown_pct);
    cJSON_AddNumberToObject(o, "hit_rate",     res.hit_rate_pct);
    send_obj(fd, o);
    cJSON_Delete(o);
}

/* ── crawl_news ────────────────────────────────────────────────── */

static void cmd_crawl_news(int fd, cJSON *root) {
    cJSON *jl = cJSON_GetObjectItemCaseSensitive(root, "limit");
    cJSON *jc = cJSON_GetObjectItemCaseSensitive(root, "cutoff_date");
    int limit = (jl && cJSON_IsNumber(jl)) ? (int)jl->valuedouble : 50;
    const char *cutoff = (jc && cJSON_IsString(jc) && jc->valuestring[0])
        ? jc->valuestring : NULL;

    int n = crawler_fetch_news(limit, cutoff);
    if (n < 0) {
        send_error(fd, "crawl_news failed");
        return;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "crawl_done");
    cJSON_AddNumberToObject(o, "n_fetched", limit);
    cJSON_AddNumberToObject(o, "n_stored", n);
    send_obj(fd, o);
    cJSON_Delete(o);
}

/* ── get_news_digest ───────────────────────────────────────────── */

static void cmd_get_news_digest(int fd, cJSON *root) {
    cJSON *jc = cJSON_GetObjectItemCaseSensitive(root, "max_chars");
    cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "days");
    cJSON *ja = cJSON_GetObjectItemCaseSensitive(root, "as_of");
    int max_chars = (jc && cJSON_IsNumber(jc)) ? (int)jc->valuedouble : 32000;
    int days      = (jd && cJSON_IsNumber(jd)) ? (int)jd->valuedouble : 7;
    const char *as_of = (ja && cJSON_IsString(ja) && ja->valuestring[0])
        ? ja->valuestring : NULL;

    char *digest = crawler_build_digest(max_chars, days, as_of);
    if (!digest) {
        send_error(fd, "build_digest failed");
        return;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "news_digest");
    cJSON_AddStringToObject(o, "digest", digest);
    send_obj(fd, o);
    cJSON_Delete(o);
    free(digest);
}

/* ── Public dispatch ────────────────────────────────────────────── */

void ipc_research_init(void)    { /* no-op for now */ }
void ipc_research_cleanup(void) { /* no-op for now */ }

int ipc_research_dispatch(int client_fd, const char *cmd, cJSON *root) {
    if (!cmd) return 0;
    if      (!strcmp(cmd, "sp500_list"))       { cmd_sp500_list(client_fd);             return 1; }
    else if (!strcmp(cmd, "bot_runs_list"))    { cmd_bot_runs_list(client_fd, root);    return 1; }
    else if (!strcmp(cmd, "bot_run_create"))   { cmd_bot_run_create(client_fd, root);   return 1; }
    else if (!strcmp(cmd, "bot_picks_ingest")) { cmd_bot_picks_ingest(client_fd, root); return 1; }
    else if (!strcmp(cmd, "bot_run_finish"))   { cmd_bot_run_finish(client_fd, root);   return 1; }
    else if (!strcmp(cmd, "aggregate_run"))    { cmd_aggregate_run(client_fd, root);    return 1; }
    else if (!strcmp(cmd, "backtest_run"))     { cmd_backtest_run(client_fd, root);     return 1; }
    else if (!strcmp(cmd, "crawl_news"))       { cmd_crawl_news(client_fd, root);       return 1; }
    else if (!strcmp(cmd, "get_news_digest"))  { cmd_get_news_digest(client_fd, root);  return 1; }
    return 0;
}
