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
#include "convergence.h"
#include "sp500_universe.h"
#include "crawler.h"
#include "market_data.h"
#include "heston.h"
#include "heston_surface.h"
#include "risk_score.h"
#include "rebalance.h"
#include "polygon_rest.h"
#include "db.h"
#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* ── Helpers shared by heston/ranking/rebalance commands ────────── */

/*
 * Re-aggregate a run and write top-K into `top`. Returns the number of
 * entries written, or <0 on error. Allocates internally.
 */
static int load_top_k_for_run(long long run_id, int k, AggResult *top) {
    enum { MAX_PICKS = 50000 };
    char (*picks)[MAX_SYMBOL_LEN] = calloc(MAX_PICKS, sizeof(*picks));
    if (!picks) return -1;

    int n = bot_picks_load_run(run_id, picks, MAX_PICKS);
    if (n < 0) { free(picks); return -1; }

    Aggregator *agg = agg_create(2048);
    if (!agg) { free(picks); return -1; }
    for (int i = 0; i < n; i++) agg_add_pick(agg, picks[i]);

    int got = agg_top_k(agg, top, k);
    agg_free(agg);
    free(picks);
    return got;
}

/*
 * For each ticker in top, load ~180 daily bars from price_cache,
 * calibrate Heston params, run MC, write to heston_scores DB and
 * fill `out[i]`. Tickers without sufficient price history get a
 * zeroed HestonScore (caller should filter or de-weight them).
 *
 * horizon_days controls T and step count (e.g. 21 for ~3 weeks).
 */
static int compute_heston_for_top(long long run_id,
                                  const AggResult *top, int n,
                                  int horizon_days,
                                  HestonScore *out) {
    int64_t now    = (int64_t)time(NULL) * 1000LL;
    int64_t lookback = now - 180LL * 86400LL * 1000LL;

    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        strncpy(out[i].symbol, top[i].symbol, MAX_SYMBOL_LEN - 1);

        OHLCBar bars[256];
        int n_bars = db_cache_load(top[i].symbol, "day",
                                   lookback, now, bars, 256);
        if (n_bars < 30) continue;   /* not enough history - leave zeroed */

        HestonParams hp;
        if (heston_calibrate_from_history(bars, n_bars,
                                          (double)horizon_days / 252.0,
                                          horizon_days, &hp) != 0)
            continue;

        /* Per-ticker seed - deterministic given (run_id, symbol). */
        uint64_t seed = (uint64_t)run_id ^ 0x9E3779B97F4A7C15ULL;
        for (const char *c = top[i].symbol; *c; c++) seed = seed * 131 + (uint8_t)*c;

        if (heston_run(&hp, &out[i], 5000, 100000, 0.005, seed) != 0)
            continue;

        db_heston_save(run_id, out[i].symbol,
                       out[i].expected_return, out[i].forward_vol,
                       out[i].es_95, out[i].prob_loss_5,
                       out[i].n_paths_used, out[i].converged);
    }
    return 0;
}

/* Format a Unix-ms timestamp as YYYY-MM-DD (UTC). buf must be >= 11 bytes. */
static void format_ymd_utc(int64_t ms, char *buf) {
    time_t secs = (time_t)(ms / 1000LL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &secs);
#else
    gmtime_r(&secs, &tmv);
#endif
    strftime(buf, 11, "%Y-%m-%d", &tmv);
}

/*
 * Ensure `symbol` has at least `min_bars` daily bars in price_cache
 * covering the last `lookback_days`. If not, synchronously fetch from
 * Polygon and store. Returns the bar count after the fetch attempt.
 *
 * Use sparingly — Polygon free tier is 5 req/min, so calling this for
 * many symbols in a tight loop will rate-limit. Single-symbol viewer
 * triggers (heston_surface, heston_path_bundle) are fine.
 */
static int ensure_daily_history(const char *symbol, int lookback_days,
                                int min_bars) {
    if (!symbol || lookback_days <= 0 || min_bars <= 0) return -1;

    int64_t now    = (int64_t)time(NULL) * 1000LL;
    int64_t lookms = now - (int64_t)lookback_days * 86400LL * 1000LL;

    OHLCBar bars[256];
    int n = db_cache_load(symbol, "day", lookms, now, bars, 256);
    if (n >= min_bars) return n;

    printf("[IPC_RESEARCH] cache cold for %s (%d bars), fetching from Polygon\n",
           symbol, n);

    char from_buf[16], to_buf[16];
    format_ymd_utc(lookms, from_buf);
    format_ymd_utc(now,    to_buf);

    OHLCBar fresh[2048];
    int fetched = polygon_rest_aggregates(symbol, 1, "day", from_buf, to_buf,
                                          fresh, 2048);
    if (fetched > 0) {
        db_cache_store(symbol, "day", fresh, fetched);
        printf("[IPC_RESEARCH] stored %d bars for %s\n", fetched, symbol);
    } else {
        fprintf(stderr, "[IPC_RESEARCH] Polygon returned %d bars for %s "
                        "(rate limit or bad ticker?)\n", fetched, symbol);
    }

    return db_cache_load(symbol, "day", lookms, now, bars, 256);
}

/* Look up a disagreement variance from the optional JSON map. */
static double disagreement_for(cJSON *map, const char *symbol) {
    if (!map || !cJSON_IsObject(map)) return 0.0;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(map, symbol);
    if (v && cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        if (d < 0.0) d = 0.0;
        if (d > 1.0) d = 1.0;
        return d;
    }
    return 0.0;
}

/* ── heston_score_run ──────────────────────────────────────────── */

static void cmd_heston_score_run(int fd, cJSON *root) {
    cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "run_id");
    cJSON *jk = cJSON_GetObjectItemCaseSensitive(root, "k");
    cJSON *jh = cJSON_GetObjectItemCaseSensitive(root, "horizon_days");

    if (!jr || !cJSON_IsNumber(jr)) {
        send_error(fd, "heston_score_run: missing run_id");
        return;
    }
    long long run_id = (long long)jr->valuedouble;
    int k = (jk && cJSON_IsNumber(jk)) ? (int)jk->valuedouble : 20;
    int horizon = (jh && cJSON_IsNumber(jh)) ? (int)jh->valuedouble : 21;
    if (k <= 0 || k > 500) k = 20;
    if (horizon <= 0 || horizon > 252) horizon = 21;

    AggResult top[500];
    int n = load_top_k_for_run(run_id, k, top);
    if (n <= 0) { send_error(fd, "no consensus picks for run"); return; }

    HestonScore *scores = calloc((size_t)n, sizeof(HestonScore));
    if (!scores) { send_error(fd, "oom"); return; }

    compute_heston_for_top(run_id, top, n, horizon, scores);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "heston_scored");
    cJSON_AddNumberToObject(o, "run_id", (double)run_id);
    cJSON_AddNumberToObject(o, "horizon_days", horizon);
    cJSON *arr = cJSON_AddArrayToObject(o, "scores");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "symbol",          scores[i].symbol);
        cJSON_AddNumberToObject(e, "expected_return", scores[i].expected_return);
        cJSON_AddNumberToObject(e, "forward_vol",     scores[i].forward_vol);
        cJSON_AddNumberToObject(e, "es_95",           scores[i].es_95);
        cJSON_AddNumberToObject(e, "prob_loss_5",     scores[i].prob_loss_5);
        cJSON_AddNumberToObject(e, "n_paths_used",    scores[i].n_paths_used);
        cJSON_AddBoolToObject  (e, "converged",       scores[i].converged);
        cJSON_AddItemToArray(arr, e);
    }
    send_obj(fd, o);
    cJSON_Delete(o);
    free(scores);
}

/* ── ranking_blend ──────────────────────────────────────────────── */
/*
 * {"cmd":"ranking_blend","run_id":42,"k":20,
 *  "horizon_days":21,
 *  "disagreement":{"AAPL":0.12,"NVDA":0.4, ...},
 *  "w_bot":0.6,"w_heston":0.4}
 *
 * Re-aggregates, runs (or reuses) Heston, builds the ScoredResult[]
 * sorted desc by blended_score. Returns the ranked array plus the
 * sigma_blend (sample stdev of blended scores) so callers can cache
 * it for rebalance evaluation.
 */
static void cmd_ranking_blend(int fd, cJSON *root) {
    cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "run_id");
    cJSON *jk = cJSON_GetObjectItemCaseSensitive(root, "k");
    cJSON *jh = cJSON_GetObjectItemCaseSensitive(root, "horizon_days");
    cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "disagreement");
    cJSON *jwb = cJSON_GetObjectItemCaseSensitive(root, "w_bot");
    cJSON *jwh = cJSON_GetObjectItemCaseSensitive(root, "w_heston");

    if (!jr || !cJSON_IsNumber(jr)) {
        send_error(fd, "ranking_blend: missing run_id");
        return;
    }
    long long run_id = (long long)jr->valuedouble;
    int k = (jk && cJSON_IsNumber(jk)) ? (int)jk->valuedouble : 20;
    int horizon = (jh && cJSON_IsNumber(jh)) ? (int)jh->valuedouble : 21;
    if (k <= 0 || k > 500) k = 20;

    AggResult top[500];
    int n = load_top_k_for_run(run_id, k, top);
    if (n <= 0) { send_error(fd, "no consensus picks for run"); return; }

    HestonScore *scores = calloc((size_t)n, sizeof(HestonScore));
    double      *dis    = calloc((size_t)n, sizeof(double));
    ScoredResult *ranked = calloc((size_t)n, sizeof(ScoredResult));
    if (!scores || !dis || !ranked) {
        free(scores); free(dis); free(ranked);
        send_error(fd, "oom");
        return;
    }

    compute_heston_for_top(run_id, top, n, horizon, scores);
    for (int i = 0; i < n; i++) dis[i] = disagreement_for(jd, top[i].symbol);

    RiskWeights w = risk_weights_default();
    if (jwb && cJSON_IsNumber(jwb)) w.w_bot    = jwb->valuedouble;
    if (jwh && cJSON_IsNumber(jwh)) w.w_heston = jwh->valuedouble;

    if (risk_score_build(top, scores, dis, n, w, ranked) != 0) {
        free(scores); free(dis); free(ranked);
        send_error(fd, "risk_score_build failed");
        return;
    }
    risk_score_sort(ranked, n);

    /* Compute sigma_blend (sample stdev) for the caller to cache. */
    double mean = 0.0;
    for (int i = 0; i < n; i++) mean += ranked[i].blended_score;
    mean /= (double)n;
    double var = 0.0;
    for (int i = 0; i < n; i++) {
        double d = ranked[i].blended_score - mean;
        var += d * d;
    }
    var /= (double)(n > 1 ? n - 1 : 1);
    double sigma = sqrt(var);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "ranking_blended");
    cJSON_AddNumberToObject(o, "run_id",       (double)run_id);
    cJSON_AddNumberToObject(o, "sigma_blend",  sigma);
    cJSON_AddNumberToObject(o, "w_bot",        w.w_bot);
    cJSON_AddNumberToObject(o, "w_heston",     w.w_heston);
    cJSON *arr = cJSON_AddArrayToObject(o, "ranked");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "symbol",          ranked[i].symbol);
        cJSON_AddNumberToObject(e, "rank",            i + 1);
        cJSON_AddNumberToObject(e, "blended_score",   ranked[i].blended_score);
        cJSON_AddNumberToObject(e, "z_bot",           ranked[i].z_bot);
        cJSON_AddNumberToObject(e, "z_heston",        ranked[i].z_heston);
        cJSON_AddNumberToObject(e, "bot_count",       ranked[i].bot_count);
        cJSON_AddNumberToObject(e, "bot_disagreement",ranked[i].bot_disagreement);
        cJSON_AddNumberToObject(e, "expected_return", ranked[i].heston_expected_ret);
        cJSON_AddNumberToObject(e, "forward_vol",     ranked[i].heston_forward_vol);
        cJSON_AddNumberToObject(e, "es_95",           ranked[i].heston_es_95);
        cJSON_AddNumberToObject(e, "prob_loss",       ranked[i].heston_prob_loss);
        cJSON_AddItemToArray(arr, e);
    }
    send_obj(fd, o);
    cJSON_Delete(o);

    free(scores);
    free(dis);
    free(ranked);
}

/* ── rebalance_check ───────────────────────────────────────────── */
/*
 * {"cmd":"rebalance_check","run_id":42,"k":20,"horizon_days":21,
 *  "disagreement":{...},
 *  "holdings":[{"symbol":"AAPL","old_blend":1.2,"days_held":4,
 *               "intended_hold_days":21}, ...],
 *  "exit_rank_band":40,    // exit threshold = score at rank 2*k
 *  "es_risk_limit":-0.10}
 *
 * For each holding, evaluates obscurity-routed decision and writes
 * to rebalance_events. AUTO decisions are pre-resolved with
 * action_taken=suggested_action so the audit log is complete.
 */
static void cmd_rebalance_check(int fd, cJSON *root) {
    cJSON *jr  = cJSON_GetObjectItemCaseSensitive(root, "run_id");
    cJSON *jk  = cJSON_GetObjectItemCaseSensitive(root, "k");
    cJSON *jh  = cJSON_GetObjectItemCaseSensitive(root, "horizon_days");
    cJSON *jd  = cJSON_GetObjectItemCaseSensitive(root, "disagreement");
    cJSON *jpf = cJSON_GetObjectItemCaseSensitive(root, "holdings");
    cJSON *jeb = cJSON_GetObjectItemCaseSensitive(root, "exit_rank_band");
    cJSON *jrl = cJSON_GetObjectItemCaseSensitive(root, "es_risk_limit");

    if (!jr || !cJSON_IsNumber(jr) ||
        !jpf || !cJSON_IsArray(jpf)) {
        send_error(fd, "rebalance_check: need run_id and holdings");
        return;
    }
    long long run_id = (long long)jr->valuedouble;
    int k = (jk && cJSON_IsNumber(jk)) ? (int)jk->valuedouble : 20;
    int horizon = (jh && cJSON_IsNumber(jh)) ? (int)jh->valuedouble : 21;
    int exit_band = (jeb && cJSON_IsNumber(jeb)) ? (int)jeb->valuedouble : 2 * k;
    if (k <= 0 || k > 500) k = 20;

    int eval_k = exit_band > k ? exit_band : k;
    if (eval_k > 500) eval_k = 500;

    AggResult top[500];
    int n = load_top_k_for_run(run_id, eval_k, top);
    if (n <= 0) { send_error(fd, "no consensus picks for run"); return; }

    HestonScore  *scores = calloc((size_t)n, sizeof(HestonScore));
    double       *dis    = calloc((size_t)n, sizeof(double));
    ScoredResult *ranked = calloc((size_t)n, sizeof(ScoredResult));
    if (!scores || !dis || !ranked) {
        free(scores); free(dis); free(ranked);
        send_error(fd, "oom"); return;
    }

    compute_heston_for_top(run_id, top, n, horizon, scores);
    for (int i = 0; i < n; i++) dis[i] = disagreement_for(jd, top[i].symbol);

    RiskWeights w = risk_weights_default();
    if (risk_score_build(top, scores, dis, n, w, ranked) != 0) {
        free(scores); free(dis); free(ranked);
        send_error(fd, "risk_score_build failed"); return;
    }
    risk_score_sort(ranked, n);

    /* sigma_blend across the evaluation universe. */
    double mean = 0.0;
    for (int i = 0; i < n; i++) mean += ranked[i].blended_score;
    mean /= (double)n;
    double var = 0.0;
    for (int i = 0; i < n; i++) {
        double d = ranked[i].blended_score - mean;
        var += d * d;
    }
    var /= (double)(n > 1 ? n - 1 : 1);

    RebalanceParams rp = rebalance_params_default();
    rp.sigma_blend = sqrt(var);
    if (jrl && cJSON_IsNumber(jrl)) rp.es_risk_limit = jrl->valuedouble;

    /* Exit threshold = score at rank `exit_band` (just past the trade
     * band). Anything below this is a candidate for sell/trim. */
    int exit_idx = exit_band - 1;
    if (exit_idx >= n) exit_idx = n - 1;
    double exit_threshold = ranked[exit_idx].blended_score;

    /* ── Iterate holdings, evaluate, persist, collect for reply. ── */

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "rebalance_check");
    cJSON_AddNumberToObject(o, "run_id",         (double)run_id);
    cJSON_AddNumberToObject(o, "sigma_blend",    rp.sigma_blend);
    cJSON_AddNumberToObject(o, "exit_threshold", exit_threshold);
    cJSON *out_arr = cJSON_AddArrayToObject(o, "events");

    int n_holdings = cJSON_GetArraySize(jpf);
    for (int i = 0; i < n_holdings; i++) {
        cJSON *h = cJSON_GetArrayItem(jpf, i);
        if (!h || !cJSON_IsObject(h)) continue;

        cJSON *jsym  = cJSON_GetObjectItemCaseSensitive(h, "symbol");
        cJSON *jold  = cJSON_GetObjectItemCaseSensitive(h, "old_blend");
        cJSON *jdh   = cJSON_GetObjectItemCaseSensitive(h, "days_held");
        cJSON *jih   = cJSON_GetObjectItemCaseSensitive(h, "intended_hold_days");
        if (!jsym || !cJSON_IsString(jsym)) continue;

        double old_blend = (jold && cJSON_IsNumber(jold)) ? jold->valuedouble : 0.0;
        int    days_held = (jdh  && cJSON_IsNumber(jdh))  ? (int)jdh->valuedouble : 0;
        int    hold_tgt  = (jih  && cJSON_IsNumber(jih))  ? (int)jih->valuedouble : 21;

        RebalanceEvent ev;
        if (rebalance_evaluate(jsym->valuestring, ranked, n,
                               old_blend, exit_threshold,
                               days_held, hold_tgt, rp, &ev) != 0) {
            continue;   /* symbol not in evaluation universe */
        }

        const char *dec_str  = (ev.decision == REBAL_AUTO_EXECUTE) ? "auto"
                            : (ev.decision == REBAL_AUTO_NOTIFY)   ? "notify"
                            : (ev.decision == REBAL_ESCALATE)      ? "escalate"
                            :                                         "hold";
        const char *act_str  = (ev.suggested_action == REBAL_ACTION_SELL) ? "sell"
                            : (ev.suggested_action == REBAL_ACTION_TRIM) ? "trim"
                            : (ev.suggested_action == REBAL_ACTION_FLIP) ? "flip"
                            :                                              "none";

        const char *taken = (ev.decision == REBAL_AUTO_EXECUTE) ? act_str : NULL;

        long long event_id = db_rebalance_save(
            ev.symbol, dec_str, act_str, taken,
            ev.old_blended_score, ev.new_blended_score, ev.exit_threshold,
            ev.obscurity.obscurity, ev.obscurity.clarity,
            ev.obscurity.primary_driver,
            ev.obscurity.score_gap_clarity, ev.obscurity.llm_agreement,
            ev.obscurity.heston_breach,    ev.obscurity.horizon_maturity,
            ev.days_held, ev.intended_hold_days, ev.debrief);

        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "event_id",          (double)event_id);
        cJSON_AddStringToObject(e, "symbol",            ev.symbol);
        cJSON_AddStringToObject(e, "decision",          dec_str);
        cJSON_AddStringToObject(e, "suggested_action",  act_str);
        cJSON_AddNumberToObject(e, "old_blend",         ev.old_blended_score);
        cJSON_AddNumberToObject(e, "new_blend",         ev.new_blended_score);
        cJSON_AddNumberToObject(e, "exit_threshold",    ev.exit_threshold);
        cJSON_AddNumberToObject(e, "obscurity",         ev.obscurity.obscurity);
        cJSON_AddNumberToObject(e, "clarity",           ev.obscurity.clarity);
        cJSON_AddStringToObject(e, "primary_driver",    ev.obscurity.primary_driver);
        cJSON_AddNumberToObject(e, "score_gap_clarity", ev.obscurity.score_gap_clarity);
        cJSON_AddNumberToObject(e, "llm_agreement",     ev.obscurity.llm_agreement);
        cJSON_AddNumberToObject(e, "heston_breach",     ev.obscurity.heston_breach);
        cJSON_AddNumberToObject(e, "horizon_maturity",  ev.obscurity.horizon_maturity);
        cJSON_AddNumberToObject(e, "days_held",         ev.days_held);
        cJSON_AddNumberToObject(e, "intended_hold_days",ev.intended_hold_days);
        cJSON_AddStringToObject(e, "debrief",           ev.debrief);
        cJSON_AddItemToArray(out_arr, e);
    }

    send_obj(fd, o);
    cJSON_Delete(o);

    free(scores);
    free(dis);
    free(ranked);
}

/* ── heston_path_bundle ────────────────────────────────────────── */
/*
 * {"cmd":"heston_path_bundle","symbol":"NVDA",
 *  "horizon_days":21,"n_paths":5000,"n_buckets":100}
 *
 * Calibrates Heston from the last 6mo of daily closes, runs N_paths,
 * returns a density grid + quantile lines for the front-end MC viewer.
 */
static void cmd_heston_path_bundle(int fd, cJSON *root) {
    cJSON *jsym = cJSON_GetObjectItemCaseSensitive(root, "symbol");
    cJSON *jh   = cJSON_GetObjectItemCaseSensitive(root, "horizon_days");
    cJSON *jn   = cJSON_GetObjectItemCaseSensitive(root, "n_paths");
    cJSON *jb   = cJSON_GetObjectItemCaseSensitive(root, "n_buckets");

    if (!jsym || !cJSON_IsString(jsym)) {
        send_error(fd, "heston_path_bundle: missing symbol");
        return;
    }
    const char *symbol = jsym->valuestring;
    int horizon  = (jh && cJSON_IsNumber(jh)) ? (int)jh->valuedouble : 21;
    int n_paths  = (jn && cJSON_IsNumber(jn)) ? (int)jn->valuedouble : 5000;
    int n_bkts   = (jb && cJSON_IsNumber(jb)) ? (int)jb->valuedouble : 100;
    if (horizon <= 0 || horizon > 252) horizon = 21;
    if (n_paths <= 0 || n_paths > 20000) n_paths = 5000;
    if (n_bkts  <= 0 || n_bkts  > 400)   n_bkts  = 100;

    /* Warm the cache on demand so the user doesn't have to run backfill
     * before they can click a symbol. */
    ensure_daily_history(symbol, 180, 30);

    int64_t now      = (int64_t)time(NULL) * 1000LL;
    int64_t lookback = now - 180LL * 86400LL * 1000LL;

    OHLCBar bars[256];
    int n_bars = db_cache_load(symbol, "day", lookback, now, bars, 256);
    if (n_bars < 30) {
        send_error(fd, "heston_path_bundle: insufficient price history "
                       "(Polygon fetch failed — rate limit or bad ticker?)");
        return;
    }
    HestonParams hp;
    if (heston_calibrate_from_history(bars, n_bars,
                                      (double)horizon / 252.0,
                                      horizon, &hp) != 0) {
        send_error(fd, "heston_path_bundle: calibration failed");
        return;
    }

    /* Per-symbol deterministic seed. */
    uint64_t seed = 0xC0FFEEFEEDFACEEULL;
    for (const char *c = symbol; *c; c++) seed = seed * 131 + (uint8_t)*c;

    HestonPathBundle bundle = {0};
    if (heston_path_bundle(&hp, n_paths, n_bkts, 3.5, &bundle, seed) != 0) {
        send_error(fd, "heston_path_bundle: simulation failed");
        return;
    }

    int n_cols = bundle.n_steps + 1;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type",          "heston_path_bundle");
    cJSON_AddStringToObject(o, "symbol",        symbol);
    cJSON_AddNumberToObject(o, "horizon_days",  horizon);
    cJSON_AddNumberToObject(o, "n_paths",       bundle.n_paths_used);
    cJSON_AddNumberToObject(o, "n_steps",       bundle.n_steps);
    cJSON_AddNumberToObject(o, "n_buckets",     bundle.n_buckets);
    cJSON_AddNumberToObject(o, "spot",          bundle.s0);
    cJSON_AddNumberToObject(o, "price_min",     bundle.price_min);
    cJSON_AddNumberToObject(o, "price_max",     bundle.price_max);
    cJSON_AddNumberToObject(o, "expected_return", bundle.expected_return);
    cJSON_AddNumberToObject(o, "es_95",         bundle.es_95);

    /* Time axis as days from now (0, 1, 2, ..., horizon).
     * dt is uniform so we can just emit a linear range. */
    cJSON *jtime = cJSON_AddArrayToObject(o, "time_days");
    for (int s = 0; s < n_cols; s++) {
        double t_days = (double)s * (double)horizon / (double)bundle.n_steps;
        cJSON_AddItemToArray(jtime, cJSON_CreateNumber(t_days));
    }

    /* Density grid as [n_buckets][n_cols] - row-major, bucket 0 = lowest. */
    cJSON *jdens = cJSON_AddArrayToObject(o, "density");
    for (int b = 0; b < bundle.n_buckets; b++) {
        cJSON *row = cJSON_CreateArray();
        for (int s = 0; s < n_cols; s++) {
            cJSON_AddItemToArray(row,
                cJSON_CreateNumber((double)bundle.density[b * n_cols + s]));
        }
        cJSON_AddItemToArray(jdens, row);
    }

    /* Quantile lines. */
    cJSON *jp05 = cJSON_AddArrayToObject(o, "p05");
    cJSON *jp50 = cJSON_AddArrayToObject(o, "p50");
    cJSON *jp95 = cJSON_AddArrayToObject(o, "p95");
    for (int s = 0; s < n_cols; s++) {
        cJSON_AddItemToArray(jp05, cJSON_CreateNumber(bundle.p05[s]));
        cJSON_AddItemToArray(jp50, cJSON_CreateNumber(bundle.p50[s]));
        cJSON_AddItemToArray(jp95, cJSON_CreateNumber(bundle.p95[s]));
    }

    send_obj(fd, o);
    cJSON_Delete(o);
    heston_path_bundle_free(&bundle);
}

/* ── heston_surface ────────────────────────────────────────────── */
/*
 * {"cmd":"heston_surface","symbol":"NVDA",
 *  "n_strikes":21,"n_maturities":12,
 *  "moneyness_lo":0.7,"moneyness_hi":1.3,
 *  "max_mat_days":180,"r":0.04}
 *
 * Calibrates Heston from history, builds the model-implied BS
 * volatility surface across the (strike x maturity) grid, returns it
 * for the canonical 3D vol-surface plot.
 */
static void cmd_heston_surface(int fd, cJSON *root) {
    cJSON *jsym = cJSON_GetObjectItemCaseSensitive(root, "symbol");
    cJSON *jns  = cJSON_GetObjectItemCaseSensitive(root, "n_strikes");
    cJSON *jnm  = cJSON_GetObjectItemCaseSensitive(root, "n_maturities");
    cJSON *jml  = cJSON_GetObjectItemCaseSensitive(root, "moneyness_lo");
    cJSON *jmh  = cJSON_GetObjectItemCaseSensitive(root, "moneyness_hi");
    cJSON *jmd  = cJSON_GetObjectItemCaseSensitive(root, "max_mat_days");
    cJSON *jr   = cJSON_GetObjectItemCaseSensitive(root, "r");

    if (!jsym || !cJSON_IsString(jsym)) {
        send_error(fd, "heston_surface: missing symbol");
        return;
    }
    const char *symbol = jsym->valuestring;
    int    n_strikes = (jns && cJSON_IsNumber(jns)) ? (int)jns->valuedouble : 21;
    int    n_mats    = (jnm && cJSON_IsNumber(jnm)) ? (int)jnm->valuedouble : 12;
    double m_lo      = (jml && cJSON_IsNumber(jml)) ? jml->valuedouble : 0.7;
    double m_hi      = (jmh && cJSON_IsNumber(jmh)) ? jmh->valuedouble : 1.3;
    int    max_md    = (jmd && cJSON_IsNumber(jmd)) ? (int)jmd->valuedouble : 180;
    double r_rate    = (jr  && cJSON_IsNumber(jr))  ? jr->valuedouble : 0.04;

    if (n_strikes < 5 || n_strikes > 60) n_strikes = 21;
    if (n_mats    < 4 || n_mats    > 30) n_mats    = 12;
    if (max_md    < 7 || max_md    > 730) max_md   = 180;

    /* Warm the cache on demand. */
    ensure_daily_history(symbol, 180, 30);

    int64_t now      = (int64_t)time(NULL) * 1000LL;
    int64_t lookback = now - 180LL * 86400LL * 1000LL;

    OHLCBar bars[256];
    int n_bars = db_cache_load(symbol, "day", lookback, now, bars, 256);
    if (n_bars < 30) {
        send_error(fd, "heston_surface: insufficient price history "
                       "(Polygon fetch failed — rate limit or bad ticker?)");
        return;
    }
    HestonParams hp;
    if (heston_calibrate_from_history(bars, n_bars,
                                      (double)max_md / 365.0,
                                      max_md, &hp) != 0) {
        send_error(fd, "heston_surface: calibration failed");
        return;
    }
    double spot = bars[n_bars - 1].close;

    HestonSurface surf = {0};
    if (heston_surface_build(&hp, spot, m_lo, m_hi,
                             n_strikes, max_md, n_mats, r_rate,
                             &surf) != 0) {
        send_error(fd, "heston_surface: build failed");
        return;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type",         "heston_surface");
    cJSON_AddStringToObject(o, "symbol",       symbol);
    cJSON_AddNumberToObject(o, "spot",         surf.spot);
    cJSON_AddNumberToObject(o, "n_strikes",    surf.n_strikes);
    cJSON_AddNumberToObject(o, "n_maturities", surf.n_maturities);
    cJSON_AddNumberToObject(o, "iv_min",       surf.iv_min);
    cJSON_AddNumberToObject(o, "iv_max",       surf.iv_max);
    cJSON_AddNumberToObject(o, "n_failed",     surf.n_failed);
    cJSON_AddNumberToObject(o, "r",            r_rate);

    cJSON *jstrikes = cJSON_AddArrayToObject(o, "strikes");
    cJSON *jmone    = cJSON_AddArrayToObject(o, "moneyness");
    for (int i = 0; i < surf.n_strikes; i++) {
        cJSON_AddItemToArray(jstrikes, cJSON_CreateNumber(surf.strikes[i]));
        cJSON_AddItemToArray(jmone,    cJSON_CreateNumber(surf.moneyness[i]));
    }

    cJSON *jmatd = cJSON_AddArrayToObject(o, "maturities_days");
    cJSON *jmatT = cJSON_AddArrayToObject(o, "maturities_years");
    for (int j = 0; j < surf.n_maturities; j++) {
        cJSON_AddItemToArray(jmatd, cJSON_CreateNumber(surf.maturities_days[j]));
        cJSON_AddItemToArray(jmatT, cJSON_CreateNumber(surf.maturities_T[j]));
    }

    /* iv as [n_strikes][n_maturities]. */
    cJSON *jiv = cJSON_AddArrayToObject(o, "iv");
    for (int i = 0; i < surf.n_strikes; i++) {
        cJSON *row = cJSON_CreateArray();
        for (int j = 0; j < surf.n_maturities; j++) {
            cJSON_AddItemToArray(row,
                cJSON_CreateNumber(surf.iv[i * surf.n_maturities + j]));
        }
        cJSON_AddItemToArray(jiv, row);
    }

    send_obj(fd, o);
    cJSON_Delete(o);
    heston_surface_free(&surf);
}

/* ── heston_diagnostics ───────────────────────────────────────── */
/*
 * {"cmd":"heston_diagnostics","symbol":"NVDA","n_paths":4000}
 *
 * Runs Layer-1 calibration sanity checks for one ticker and returns
 * a JSON document with the Feller flag, simulated-vs-historical
 * moment comparison, realized-vol stability, and overall score.
 */
static void cmd_heston_diagnostics(int fd, cJSON *root) {
    cJSON *jsym = cJSON_GetObjectItemCaseSensitive(root, "symbol");
    cJSON *jnp  = cJSON_GetObjectItemCaseSensitive(root, "n_paths");
    if (!jsym || !cJSON_IsString(jsym)) {
        send_error(fd, "heston_diagnostics: missing symbol");
        return;
    }
    const char *symbol = jsym->valuestring;
    int n_paths = (jnp && cJSON_IsNumber(jnp)) ? (int)jnp->valuedouble : 4000;
    if (n_paths < 200)   n_paths = 200;
    if (n_paths > 20000) n_paths = 20000;

    /* 1 year of daily history. */
    int64_t now      = (int64_t)time(NULL) * 1000LL;
    int64_t lookback = now - 380LL * 86400LL * 1000LL;

    ensure_daily_history(symbol, 380, 60);

    OHLCBar bars[512];
    int n_bars = db_cache_load(symbol, "day", lookback, now, bars, 512);
    if (n_bars < 60) {
        send_error(fd, "heston_diagnostics: insufficient history (need ~60 daily bars)");
        return;
    }

    /* Per-symbol deterministic seed so repeat calls match. */
    uint64_t seed = 0xD1A60057ABCDEF12ULL;
    for (const char *c = symbol; *c; c++) seed = seed * 131 + (uint8_t)*c;

    HestonDiagnostics diag;
    if (heston_diagnostics(bars, n_bars, symbol, n_paths, seed, &diag) != 0) {
        send_error(fd, "heston_diagnostics: computation failed");
        return;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type",   "heston_diagnostics");
    cJSON_AddStringToObject(o, "symbol", diag.symbol);
    cJSON_AddNumberToObject(o, "n_history_bars",    diag.n_history_bars);
    cJSON_AddNumberToObject(o, "hist_window_years", diag.hist_window_years);
    cJSON_AddNumberToObject(o, "n_paths_used",      diag.n_paths_used);

    cJSON *params = cJSON_AddObjectToObject(o, "params");
    cJSON_AddNumberToObject(params, "v0",      diag.v0);
    cJSON_AddNumberToObject(params, "theta",   diag.theta);
    cJSON_AddNumberToObject(params, "kappa",   diag.kappa);
    cJSON_AddNumberToObject(params, "sigma_v", diag.sigma_v);
    cJSON_AddNumberToObject(params, "rho",     diag.rho);

    cJSON *feller = cJSON_AddObjectToObject(o, "feller");
    cJSON_AddNumberToObject(feller, "lhs", diag.feller_lhs);
    cJSON_AddNumberToObject(feller, "rhs", diag.feller_rhs);
    cJSON_AddBoolToObject  (feller, "ok",  diag.feller_ok);

    cJSON *hist = cJSON_AddObjectToObject(o, "historical");
    cJSON_AddNumberToObject(hist, "mean_ann",    diag.hist_mean_ann);
    cJSON_AddNumberToObject(hist, "std_ann",     diag.hist_std_ann);
    cJSON_AddNumberToObject(hist, "skew",        diag.hist_skew);
    cJSON_AddNumberToObject(hist, "kurt_excess", diag.hist_kurt_excess);

    cJSON *sim = cJSON_AddObjectToObject(o, "simulated");
    cJSON_AddNumberToObject(sim, "mean_ann",    diag.sim_mean_ann);
    cJSON_AddNumberToObject(sim, "std_ann",     diag.sim_std_ann);
    cJSON_AddNumberToObject(sim, "skew",        diag.sim_skew);
    cJSON_AddNumberToObject(sim, "kurt_excess", diag.sim_kurt_excess);

    cJSON *rv = cJSON_AddObjectToObject(o, "realized_vol");
    cJSON_AddNumberToObject(rv, "rv21_mean_vol",        diag.rv21_mean_vol);
    cJSON_AddNumberToObject(rv, "rv21_std_vol",         diag.rv21_std_vol);
    cJSON_AddNumberToObject(rv, "sqrt_theta",           diag.sqrt_theta);
    cJSON_AddNumberToObject(rv, "empirical_vol_of_vol", diag.empirical_vol_of_vol);

    cJSON *scores = cJSON_AddObjectToObject(o, "scores");
    cJSON_AddNumberToObject(scores, "moment_match",   diag.moment_match_score);
    cJSON_AddNumberToObject(scores, "mean_reversion", diag.mean_reversion_score);
    cJSON_AddNumberToObject(scores, "overall",        diag.overall_score);

    send_obj(fd, o);
    cJSON_Delete(o);
}

/* ── convergence_check ─────────────────────────────────────────── */
/*
 * {"cmd":"convergence_check","run_id":42,"k":20,
 *  "prev":["NVDA","AAPL",...],   // top-K from the previous wave
 *  "threshold":0.9}
 *
 * Re-aggregates the run, returns the current top-K, the Jaccard
 * similarity vs `prev`, and a `stable` flag (Jaccard >= threshold).
 * Used by the parallel orchestrator to short-circuit ensembles once
 * the consensus stops moving.
 */
static void cmd_convergence_check(int fd, cJSON *root) {
    cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "run_id");
    cJSON *jk = cJSON_GetObjectItemCaseSensitive(root, "k");
    cJSON *jp = cJSON_GetObjectItemCaseSensitive(root, "prev");
    cJSON *jt = cJSON_GetObjectItemCaseSensitive(root, "threshold");

    if (!jr || !cJSON_IsNumber(jr)) {
        send_error(fd, "convergence_check: missing run_id");
        return;
    }
    long long run_id = (long long)jr->valuedouble;
    int k = (jk && cJSON_IsNumber(jk)) ? (int)jk->valuedouble : 20;
    double threshold = (jt && cJSON_IsNumber(jt)) ? jt->valuedouble : 0.9;
    if (k <= 0 || k > 500) k = 20;

    AggResult top[500];
    int got = load_top_k_for_run(run_id, k, top);
    if (got < 0) { send_error(fd, "load_run failed"); return; }

    /* Build prev[] from the symbol-only JSON array. count is irrelevant
     * for Jaccard — convergence treats it as a set. */
    AggResult prev[500];
    int n_prev = 0;
    if (jp && cJSON_IsArray(jp)) {
        int n = cJSON_GetArraySize(jp);
        if (n > 500) n = 500;
        for (int i = 0; i < n; i++) {
            cJSON *e = cJSON_GetArrayItem(jp, i);
            if (!e || !cJSON_IsString(e)) continue;
            strncpy(prev[n_prev].symbol, e->valuestring, MAX_SYMBOL_LEN - 1);
            prev[n_prev].symbol[MAX_SYMBOL_LEN - 1] = '\0';
            prev[n_prev].count = 1;
            n_prev++;
        }
    }

    double jaccard = convergence_jaccard(top, got, prev, n_prev);
    int stable = convergence_is_stable(top, got, prev, n_prev, threshold);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type",      "convergence_check");
    cJSON_AddNumberToObject(o, "run_id",    (double)run_id);
    cJSON_AddNumberToObject(o, "jaccard",   jaccard);
    cJSON_AddNumberToObject(o, "threshold", threshold);
    cJSON_AddBoolToObject  (o, "stable",    stable);
    cJSON *arr = cJSON_AddArrayToObject(o, "top");
    for (int i = 0; i < got; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "symbol", top[i].symbol);
        cJSON_AddNumberToObject(e, "count",  top[i].count);
        cJSON_AddItemToArray(arr, e);
    }
    send_obj(fd, o);
    cJSON_Delete(o);
}

/* ── rebalance_resolve ─────────────────────────────────────────── */
/*
 * {"cmd":"rebalance_resolve","event_id":17,"action":"sell",
 *  "override":"keep - earnings beat coming"}
 *
 * Marks an outstanding NOTIFY/ESCALATE event as resolved with the
 * user's chosen action (and optional override note).
 */
static void cmd_rebalance_resolve(int fd, cJSON *root) {
    cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "event_id");
    cJSON *jac = cJSON_GetObjectItemCaseSensitive(root, "action");
    cJSON *jov = cJSON_GetObjectItemCaseSensitive(root, "override");
    if (!jid || !cJSON_IsNumber(jid) ||
        !jac || !cJSON_IsString(jac)) {
        send_error(fd, "rebalance_resolve: need event_id and action");
        return;
    }
    long long id = (long long)jid->valuedouble;
    const char *act = jac->valuestring;
    const char *override = (jov && cJSON_IsString(jov) && jov->valuestring[0])
        ? jov->valuestring : NULL;

    if (db_rebalance_resolve(id, act, override) != 0) {
        send_error(fd, "rebalance_resolve failed");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "rebalance_resolved");
    cJSON_AddNumberToObject(o, "event_id", (double)id);
    send_obj(fd, o);
    cJSON_Delete(o);
}

/* ── Public dispatch ────────────────────────────────────────────── */

void ipc_research_init(void)    { /* no-op for now */ }
void ipc_research_cleanup(void) { /* no-op for now */ }

int ipc_research_dispatch(int client_fd, const char *cmd, cJSON *root) {
    if (!cmd) return 0;
    if      (!strcmp(cmd, "sp500_list"))         { cmd_sp500_list(client_fd);              return 1; }
    else if (!strcmp(cmd, "bot_runs_list"))      { cmd_bot_runs_list(client_fd, root);     return 1; }
    else if (!strcmp(cmd, "bot_run_create"))     { cmd_bot_run_create(client_fd, root);    return 1; }
    else if (!strcmp(cmd, "bot_picks_ingest"))   { cmd_bot_picks_ingest(client_fd, root);  return 1; }
    else if (!strcmp(cmd, "bot_run_finish"))     { cmd_bot_run_finish(client_fd, root);    return 1; }
    else if (!strcmp(cmd, "aggregate_run"))      { cmd_aggregate_run(client_fd, root);     return 1; }
    else if (!strcmp(cmd, "backtest_run"))       { cmd_backtest_run(client_fd, root);      return 1; }
    else if (!strcmp(cmd, "crawl_news"))         { cmd_crawl_news(client_fd, root);        return 1; }
    else if (!strcmp(cmd, "get_news_digest"))    { cmd_get_news_digest(client_fd, root);   return 1; }
    else if (!strcmp(cmd, "heston_score_run"))   { cmd_heston_score_run(client_fd, root);  return 1; }
    else if (!strcmp(cmd, "ranking_blend"))      { cmd_ranking_blend(client_fd, root);     return 1; }
    else if (!strcmp(cmd, "rebalance_check"))    { cmd_rebalance_check(client_fd, root);   return 1; }
    else if (!strcmp(cmd, "rebalance_resolve"))  { cmd_rebalance_resolve(client_fd, root); return 1; }
    else if (!strcmp(cmd, "convergence_check"))  { cmd_convergence_check(client_fd, root); return 1; }
    else if (!strcmp(cmd, "heston_path_bundle")) { cmd_heston_path_bundle(client_fd, root); return 1; }
    else if (!strcmp(cmd, "heston_surface"))     { cmd_heston_surface(client_fd, root);     return 1; }
    else if (!strcmp(cmd, "heston_diagnostics")) { cmd_heston_diagnostics(client_fd, root); return 1; }
    return 0;
}
