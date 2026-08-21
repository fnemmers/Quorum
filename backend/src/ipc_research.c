/*
 * ipc_research.c  --  Backend command handlers for the Bates + AI-jump
 *                     project. All commands are stateless: they take
 *                     the parameters they need (including any symbols
 *                     universe) and compute-and-return. Only the raw
 *                     feed layers (price_cache, news_cache,
 *                     news_jump_signal) are persisted.
 */

#include "ipc_research.h"
#include "sp500_universe.h"
#include "crawler.h"
#include "market_data.h"
#include "heston.h"
#include "heston_surface.h"
#include "news_jump.h"
#include "bates_backtest.h"
#include "option_score.h"
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
 * Polygon free tier is 5 req/min — call sparingly.
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

/* ── crawl_news / get_news_digest ───────────────────────────────── */

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
    cJSON_AddStringToObject(o, "type",   "news_digest");
    cJSON_AddNumberToObject(o, "days",   days);
    cJSON_AddStringToObject(o, "digest", digest);
    send_obj(fd, o);
    cJSON_Delete(o);
    free(digest);
}

/* ── heston_path_bundle ─────────────────────────────────────────── */

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
    if (horizon <= 0 || horizon > 252)   horizon = 21;
    if (n_paths <= 0 || n_paths > 20000) n_paths = 5000;
    if (n_bkts  <= 0 || n_bkts  > 400)   n_bkts  = 100;

    ensure_daily_history(symbol, 180, 30);

    int64_t now      = (int64_t)time(NULL) * 1000LL;
    int64_t lookback = now - 180LL * 86400LL * 1000LL;

    OHLCBar bars[256];
    int n_bars = db_cache_load(symbol, "day", lookback, now, bars, 256);
    if (n_bars < 30) {
        send_error(fd, "heston_path_bundle: insufficient price history");
        return;
    }
    HestonParams hp;
    if (heston_calibrate_from_history(bars, n_bars,
                                      (double)horizon / 252.0,
                                      horizon, &hp) != 0) {
        send_error(fd, "heston_path_bundle: calibration failed");
        return;
    }

    /* News-driven jump overlay on the per-symbol Bates params. */
    news_jump_apply(&hp, symbol, now);

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

    cJSON *jtime = cJSON_AddArrayToObject(o, "time_days");
    for (int s = 0; s < n_cols; s++) {
        double t_days = (double)s * (double)horizon / (double)bundle.n_steps;
        cJSON_AddItemToArray(jtime, cJSON_CreateNumber(t_days));
    }

    cJSON *jdens = cJSON_AddArrayToObject(o, "density");
    for (int b = 0; b < bundle.n_buckets; b++) {
        cJSON *row = cJSON_CreateArray();
        for (int s = 0; s < n_cols; s++) {
            cJSON_AddItemToArray(row,
                cJSON_CreateNumber((double)bundle.density[b * n_cols + s]));
        }
        cJSON_AddItemToArray(jdens, row);
    }

    cJSON *jp05 = cJSON_AddArrayToObject(o, "p05");
    cJSON *jp50 = cJSON_AddArrayToObject(o, "p50");
    cJSON *jp95 = cJSON_AddArrayToObject(o, "p95");
    for (int s = 0; s < n_cols; s++) {
        cJSON_AddItemToArray(jp05, cJSON_CreateNumber(bundle.p05[s]));
        cJSON_AddItemToArray(jp50, cJSON_CreateNumber(bundle.p50[s]));
        cJSON_AddItemToArray(jp95, cJSON_CreateNumber(bundle.p95[s]));
    }

    /* Subsampled sample paths for the spaghetti overlay. Each path is
     * classified by its terminal outcome: 0 = tail (below p05_T),
     * 1 = middle, 2 = above p95_T. The frontend colours accordingly. */
    cJSON *jsp    = cJSON_AddArrayToObject(o, "sample_paths");
    cJSON *jcls   = cJSON_AddArrayToObject(o, "sample_class");
    cJSON_AddNumberToObject(o, "n_sample_paths", bundle.n_sample_paths);
    for (int i = 0; i < bundle.n_sample_paths; i++) {
        cJSON *row = cJSON_CreateArray();
        for (int s = 0; s < n_cols; s++) {
            cJSON_AddItemToArray(row,
                cJSON_CreateNumber(bundle.sample_paths[(size_t)i * (size_t)n_cols + s]));
        }
        cJSON_AddItemToArray(jsp, row);
        cJSON_AddItemToArray(jcls, cJSON_CreateNumber(bundle.sample_class[i]));
    }

    send_obj(fd, o);
    cJSON_Delete(o);
    heston_path_bundle_free(&bundle);
}

/* ── heston_surface ─────────────────────────────────────────────── */

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

    ensure_daily_history(symbol, 180, 30);

    int64_t now      = (int64_t)time(NULL) * 1000LL;
    int64_t lookback = now - 180LL * 86400LL * 1000LL;

    OHLCBar bars[256];
    int n_bars = db_cache_load(symbol, "day", lookback, now, bars, 256);
    if (n_bars < 30) {
        send_error(fd, "heston_surface: insufficient price history");
        return;
    }
    HestonParams hp;
    if (heston_calibrate_from_history(bars, n_bars,
                                      (double)max_md / 365.0,
                                      max_md, &hp) != 0) {
        send_error(fd, "heston_surface: calibration failed");
        return;
    }
    news_jump_apply(&hp, symbol, now);
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

/* ── heston_diagnostics ─────────────────────────────────────────── */

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

    int64_t now      = (int64_t)time(NULL) * 1000LL;
    int64_t lookback = now - 380LL * 86400LL * 1000LL;

    ensure_daily_history(symbol, 380, 60);

    OHLCBar bars[512];
    int n_bars = db_cache_load(symbol, "day", lookback, now, bars, 512);
    if (n_bars < 60) {
        send_error(fd, "heston_diagnostics: insufficient history (need ~60 daily bars)");
        return;
    }

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

/* ── news_jump_status ───────────────────────────────────────────── */

static void cmd_news_jump_status(int fd, cJSON *root) {
    int window_hours = 48;
    cJSON *jw = cJSON_GetObjectItemCaseSensitive(root, "window_hours");
    if (cJSON_IsNumber(jw) && jw->valuedouble > 0) window_hours = (int)jw->valuedouble;

    int64_t now = (int64_t)time(NULL) * 1000LL;
    (void)news_jump_recompute(now, window_hours);

    cJSON *rows = cJSON_CreateArray();
    cJSON *arr  = cJSON_GetObjectItemCaseSensitive(root, "symbols");
    if (cJSON_IsArray(arr)) {
        cJSON *el;
        cJSON_ArrayForEach(el, arr) {
            if (!cJSON_IsString(el) || !el->valuestring) continue;
            NewsJumpSignal s;
            int got = news_jump_get(el->valuestring, now, &s);
            cJSON *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "symbol", el->valuestring);
            if (got == 1) {
                cJSON_AddNumberToObject(row, "as_of",        (double)s.as_of);
                cJSON_AddNumberToObject(row, "window_hours", s.window_hours);
                cJSON_AddNumberToObject(row, "n_articles",   s.n_articles);
                cJSON_AddNumberToObject(row, "sentiment_avg",s.sentiment_avg);
                cJSON_AddStringToObject(row, "event_class",  s.event_class);
                cJSON_AddNumberToObject(row, "lam_bump",     s.lam_bump);
                cJSON_AddNumberToObject(row, "mu_j_bias",    s.mu_j_bias);
                cJSON_AddNumberToObject(row, "sigma_j_bump", s.sigma_j_bump);
            } else {
                cJSON_AddNumberToObject(row, "n_articles",   0);
                cJSON_AddNumberToObject(row, "sentiment_avg",0);
                cJSON_AddStringToObject(row, "event_class",  "");
                cJSON_AddNumberToObject(row, "lam_bump",     0);
                cJSON_AddNumberToObject(row, "mu_j_bias",    0);
                cJSON_AddNumberToObject(row, "sigma_j_bump", 0);
            }
            cJSON_AddItemToArray(rows, row);
        }
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type",         "news_jump");
    cJSON_AddNumberToObject(o, "as_of",        (double)now);
    cJSON_AddNumberToObject(o, "window_hours", window_hours);
    cJSON_AddItemToObject   (o, "rows",        rows);
    send_obj(fd, o);
    cJSON_Delete(o);
}

/* ── Universe parsing helper (symbols array -> C-string array) ──── */
/*
 * Reads a JSON symbols array, allocates a caller-owned char** with
 * up to MAX_SYMS entries. Each string is heap-allocated; free everything
 * with free_symbols(). Returns the number of symbols parsed.
 */
#define MAX_UNIVERSE 500

static int parse_symbols(cJSON *root, char ***out_syms) {
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "symbols");
    if (!cJSON_IsArray(arr)) return 0;

    char **syms = (char **)calloc(MAX_UNIVERSE, sizeof(char *));
    if (!syms) return 0;

    int n = 0;
    cJSON *el;
    cJSON_ArrayForEach(el, arr) {
        if (!cJSON_IsString(el) || !el->valuestring) continue;
        if (n >= MAX_UNIVERSE) break;
        size_t L = strlen(el->valuestring);
        if (L == 0 || L >= MAX_SYMBOL_LEN) continue;
        syms[n] = (char *)malloc(L + 1);
        if (!syms[n]) break;
        memcpy(syms[n], el->valuestring, L + 1);
        n++;
    }

    *out_syms = syms;
    return n;
}

static void free_symbols(char **syms, int n) {
    if (!syms) return;
    for (int i = 0; i < n; i++) free(syms[i]);
    free(syms);
}

/* ── Bates option-level backtest ─────────────────────────────────── */
/*
 * Stateless.
 * Request:
 *   {"cmd":"bates_backtest_run",
 *    "symbols":["AAPL","MSFT",...],
 *    "horizon_days":30, "moneyness_lo":0.85, "moneyness_hi":1.15,
 *    "n_strikes":7, "expiries_days":[7,30,90],
 *    "n_paths":256, "noise_sigma_vol":0.015, "r":0.045, "seed":123}
 */
static void cmd_bates_backtest_run(int fd, cJSON *root) {
    cJSON *jh  = cJSON_GetObjectItemCaseSensitive(root, "horizon_days");
    cJSON *jml = cJSON_GetObjectItemCaseSensitive(root, "moneyness_lo");
    cJSON *jmh = cJSON_GetObjectItemCaseSensitive(root, "moneyness_hi");
    cJSON *jns = cJSON_GetObjectItemCaseSensitive(root, "n_strikes");
    cJSON *jex = cJSON_GetObjectItemCaseSensitive(root, "expiries_days");
    cJSON *jnp = cJSON_GetObjectItemCaseSensitive(root, "n_paths");
    cJSON *jsg = cJSON_GetObjectItemCaseSensitive(root, "noise_sigma_vol");
    cJSON *jr2 = cJSON_GetObjectItemCaseSensitive(root, "r");
    cJSON *jsd = cJSON_GetObjectItemCaseSensitive(root, "seed");

    int    horizon    = (jh && cJSON_IsNumber(jh))  ? (int)jh->valuedouble  : 30;
    double m_lo       = (jml && cJSON_IsNumber(jml))? jml->valuedouble      : 0.85;
    double m_hi       = (jmh && cJSON_IsNumber(jmh))? jmh->valuedouble      : 1.15;
    int    n_strikes  = (jns && cJSON_IsNumber(jns))? (int)jns->valuedouble : 7;
    int    n_paths    = (jnp && cJSON_IsNumber(jnp))? (int)jnp->valuedouble : 256;
    double sigma_vol  = (jsg && cJSON_IsNumber(jsg))? jsg->valuedouble      : 0.015;
    double r_rate     = (jr2 && cJSON_IsNumber(jr2))? jr2->valuedouble      : 0.045;
    uint64_t seed     = (jsd && cJSON_IsNumber(jsd))
        ? (uint64_t)jsd->valuedouble : 0xC0FFEE1234567890ULL;

    int  ex_default[3] = { 7, 30, 90 };
    int  ex_buf[16];
    int  n_ex = 0;
    if (cJSON_IsArray(jex)) {
        cJSON *el;
        cJSON_ArrayForEach(el, jex) {
            if (cJSON_IsNumber(el) && n_ex < 16)
                ex_buf[n_ex++] = (int)el->valuedouble;
        }
    }
    const int *expiries = n_ex > 0 ? ex_buf : ex_default;
    if (n_ex == 0) n_ex = 3;

    char **syms = NULL;
    int    n_syms = parse_symbols(root, &syms);
    if (n_syms <= 0) {
        send_error(fd, "bates_backtest_run: missing/empty symbols array");
        free_symbols(syms, n_syms);
        return;
    }

    int64_t session_id = (int64_t)time(NULL);

    BatesBacktestRow *rows = NULL;
    int              n_out = 0;
    BatesBacktestSummary sum = {0};

    if (bates_backtest_run((const char *const *)syms, n_syms,
                           session_id, horizon,
                           m_lo, m_hi, n_strikes,
                           expiries, n_ex,
                           n_paths, sigma_vol,
                           seed, r_rate,
                           &rows, &n_out, &sum) != 0) {
        send_error(fd, "bates_backtest_run failed");
        free(rows);
        free_symbols(syms, n_syms);
        return;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type",       "bates_backtest");
    cJSON_AddNumberToObject(o, "session_id", (double)session_id);

    cJSON *js = cJSON_CreateObject();
    cJSON_AddNumberToObject(js, "n_rows",          sum.n_rows);
    cJSON_AddNumberToObject(js, "n_symbols",       sum.n_symbols);
    cJSON_AddNumberToObject(js, "n_strikes",       sum.n_strikes);
    cJSON_AddNumberToObject(js, "n_expiries",      sum.n_expiries);
    cJSON_AddNumberToObject(js, "n_paths",         sum.n_paths);
    cJSON_AddNumberToObject(js, "horizon_days",    sum.horizon_days);
    cJSON_AddNumberToObject(js, "noise_sigma_vol", sum.noise_sigma_vol);
    cJSON_AddNumberToObject(js, "r",               sum.r);
    cJSON_AddNumberToObject(js, "mean_edge_vol_pts",     sum.mean_edge_vol_pts);
    cJSON_AddNumberToObject(js, "std_edge_vol_pts",      sum.std_edge_vol_pts);
    cJSON_AddNumberToObject(js, "hit_rate_edge_gt_1vol", sum.hit_rate_edge_gt_1vol);
    cJSON_AddNumberToObject(js, "avg_hedged_pnl",        sum.avg_hedged_pnl);
    cJSON_AddNumberToObject(js, "avg_sharpe_daily",      sum.avg_sharpe_daily);
    cJSON_AddItemToObject(o, "summary", js);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n_out; i++) {
        BatesBacktestRow *rw = &rows[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "symbol",       rw->symbol);
        cJSON_AddNumberToObject(e, "strike",       rw->strike);
        cJSON_AddNumberToObject(e, "dte_days",     rw->dte_days);
        char rch[2] = { rw->right, 0 };
        cJSON_AddStringToObject(e, "right",        rch);
        cJSON_AddNumberToObject(e, "spot",         rw->spot);
        cJSON_AddNumberToObject(e, "model_iv",     rw->model_iv);
        cJSON_AddNumberToObject(e, "market_iv",    rw->market_iv);
        cJSON_AddNumberToObject(e, "edge_vol_pts", rw->edge_vol_pts);
        cJSON_AddNumberToObject(e, "premium",      rw->premium);
        cJSON_AddNumberToObject(e, "delta",        rw->delta0);
        cJSON_AddNumberToObject(e, "gamma",        rw->gamma0);
        cJSON_AddNumberToObject(e, "vega",         rw->vega0);
        cJSON_AddNumberToObject(e, "expected_hedged_pnl", rw->expected_hedged_pnl);
        cJSON_AddNumberToObject(e, "std_hedged_pnl",      rw->std_hedged_pnl);
        cJSON_AddNumberToObject(e, "sharpe_daily",        rw->sharpe_daily);
        cJSON_AddNumberToObject(e, "n_paths",             rw->n_paths);
        cJSON_AddNumberToObject(e, "signed_edge",         rw->signed_edge);
        cJSON_AddNumberToObject(e, "dollar_edge",         rw->dollar_edge);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(o, "rows", arr);

    send_obj(fd, o);
    cJSON_Delete(o);
    free(rows);
    free_symbols(syms, n_syms);
}

/* ── Option-level z-score fusion (Bates edge + news + convex) ────── */
/*
 * Stateless.
 * Request:
 *   {"cmd":"option_ranking_blend",
 *    "symbols":["AAPL","MSFT",...],
 *    "horizon_days":30, "moneyness_lo":0.85, "moneyness_hi":1.15,
 *    "n_strikes":7, "expiries_days":[7,30,90],
 *    "n_paths":256, "noise_sigma_vol":0.015, "r":0.045, "seed":123,
 *    "w_edge":0.60, "w_news":0.27, "w_convex":0.13,
 *    "min_universe":8}
 */
static void cmd_option_ranking_blend(int fd, cJSON *root) {
    cJSON *jh  = cJSON_GetObjectItemCaseSensitive(root, "horizon_days");
    cJSON *jml = cJSON_GetObjectItemCaseSensitive(root, "moneyness_lo");
    cJSON *jmh = cJSON_GetObjectItemCaseSensitive(root, "moneyness_hi");
    cJSON *jns = cJSON_GetObjectItemCaseSensitive(root, "n_strikes");
    cJSON *jex = cJSON_GetObjectItemCaseSensitive(root, "expiries_days");
    cJSON *jnp = cJSON_GetObjectItemCaseSensitive(root, "n_paths");
    cJSON *jsg = cJSON_GetObjectItemCaseSensitive(root, "noise_sigma_vol");
    cJSON *jr2 = cJSON_GetObjectItemCaseSensitive(root, "r");
    cJSON *jsd = cJSON_GetObjectItemCaseSensitive(root, "seed");
    cJSON *jwe = cJSON_GetObjectItemCaseSensitive(root, "w_edge");
    cJSON *jwn = cJSON_GetObjectItemCaseSensitive(root, "w_news");
    cJSON *jwc = cJSON_GetObjectItemCaseSensitive(root, "w_convex");
    cJSON *jmu = cJSON_GetObjectItemCaseSensitive(root, "min_universe");

    int    horizon   = (jh && cJSON_IsNumber(jh))  ? (int)jh->valuedouble  : 30;
    double m_lo      = (jml && cJSON_IsNumber(jml))? jml->valuedouble      : 0.85;
    double m_hi      = (jmh && cJSON_IsNumber(jmh))? jmh->valuedouble      : 1.15;
    int    n_strikes = (jns && cJSON_IsNumber(jns))? (int)jns->valuedouble : 7;
    int    n_paths   = (jnp && cJSON_IsNumber(jnp))? (int)jnp->valuedouble : 256;
    double sigma_vol = (jsg && cJSON_IsNumber(jsg))? jsg->valuedouble      : 0.015;
    double r_rate    = (jr2 && cJSON_IsNumber(jr2))? jr2->valuedouble      : 0.045;
    uint64_t seed    = (jsd && cJSON_IsNumber(jsd))
        ? (uint64_t)jsd->valuedouble : 0xC0FFEE1234567890ULL;
    int  min_uni     = (jmu && cJSON_IsNumber(jmu))? (int)jmu->valuedouble : 8;

    OptionScoreWeights w;
    option_score_default_weights(&w);
    if (jwe && cJSON_IsNumber(jwe)) w.w_edge   = jwe->valuedouble;
    if (jwn && cJSON_IsNumber(jwn)) w.w_news   = jwn->valuedouble;
    if (jwc && cJSON_IsNumber(jwc)) w.w_convex = jwc->valuedouble;

    int  ex_default[3] = { 7, 30, 90 };
    int  ex_buf[16];
    int  n_ex = 0;
    if (cJSON_IsArray(jex)) {
        cJSON *el;
        cJSON_ArrayForEach(el, jex) {
            if (cJSON_IsNumber(el) && n_ex < 16)
                ex_buf[n_ex++] = (int)el->valuedouble;
        }
    }
    const int *expiries = n_ex > 0 ? ex_buf : ex_default;
    if (n_ex == 0) n_ex = 3;

    char **syms = NULL;
    int    n_syms = parse_symbols(root, &syms);
    if (n_syms <= 0) {
        send_error(fd, "option_ranking_blend: missing/empty symbols array");
        free_symbols(syms, n_syms);
        return;
    }

    int64_t session_id = (int64_t)time(NULL);
    int64_t now        = session_id * 1000LL;

    /* Bates backtest fills the option grid. */
    BatesBacktestRow *bates_rows = NULL;
    int                bates_n   = 0;
    BatesBacktestSummary sum     = {0};
    if (bates_backtest_run((const char *const *)syms, n_syms,
                           session_id, horizon,
                           m_lo, m_hi, n_strikes,
                           expiries, n_ex,
                           n_paths, sigma_vol,
                           seed, r_rate,
                           &bates_rows, &bates_n, &sum) != 0) {
        send_error(fd, "option_ranking_blend: Bates backtest failed");
        free(bates_rows);
        free_symbols(syms, n_syms);
        return;
    }

    /* Per-symbol fusion inputs: only the news scalar remains. */
    PerSymbolFusionInput *ps =
        (PerSymbolFusionInput *)calloc((size_t)n_syms, sizeof(PerSymbolFusionInput));
    if (!ps) { send_error(fd, "oom"); free(bates_rows); free_symbols(syms, n_syms); return; }

    for (int i = 0; i < n_syms; i++) {
        strncpy(ps[i].symbol, syms[i], MAX_SYMBOL_LEN - 1);
        NewsJumpSignal njs;
        int got = news_jump_get(syms[i], now, &njs);
        ps[i].news_scalar = (got == 1)
            ? njs.lam_bump + 5.0 * fabs(njs.mu_j_bias)
            : 0.0;
    }

    OptionScoredResult *ranked = NULL;
    if (option_score_run(bates_rows, bates_n, ps, n_syms, &w, min_uni,
                         &ranked) != 0) {
        send_error(fd, "option_ranking_blend: fusion failed");
        free(bates_rows); free(ps); free_symbols(syms, n_syms);
        return;
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type",       "option_ranking");
    cJSON_AddNumberToObject(o, "session_id", (double)session_id);
    cJSON_AddNumberToObject(o, "n_rows",     bates_n);

    cJSON *wj = cJSON_CreateObject();
    cJSON_AddNumberToObject(wj, "w_edge",   w.w_edge);
    cJSON_AddNumberToObject(wj, "w_news",   w.w_news);
    cJSON_AddNumberToObject(wj, "w_convex", w.w_convex);
    cJSON_AddItemToObject(o, "weights", wj);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < bates_n; i++) {
        OptionScoredResult *r = &ranked[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "rank",         r->rank);
        cJSON_AddStringToObject(e, "symbol",       r->bates.symbol);
        cJSON_AddNumberToObject(e, "strike",       r->bates.strike);
        cJSON_AddNumberToObject(e, "dte_days",     r->bates.dte_days);
        char rch[2] = { r->bates.right, 0 };
        cJSON_AddStringToObject(e, "right",        rch);
        cJSON_AddNumberToObject(e, "spot",         r->bates.spot);
        cJSON_AddNumberToObject(e, "model_iv",     r->bates.model_iv);
        cJSON_AddNumberToObject(e, "market_iv",    r->bates.market_iv);
        cJSON_AddNumberToObject(e, "edge_vol_pts", r->bates.edge_vol_pts);
        cJSON_AddNumberToObject(e, "dollar_edge",  r->bates.dollar_edge);
        cJSON_AddNumberToObject(e, "premium",      r->bates.premium);
        cJSON_AddNumberToObject(e, "delta",        r->bates.delta0);
        cJSON_AddNumberToObject(e, "gamma",        r->bates.gamma0);
        cJSON_AddNumberToObject(e, "vega",         r->bates.vega0);
        cJSON_AddNumberToObject(e, "expected_hedged_pnl", r->bates.expected_hedged_pnl);
        cJSON_AddNumberToObject(e, "sharpe_daily",        r->bates.sharpe_daily);
        cJSON_AddNumberToObject(e, "z_bates_edge",        r->z_bates_edge);
        cJSON_AddNumberToObject(e, "z_news_jump",         r->z_news_jump);
        cJSON_AddNumberToObject(e, "z_convexity",         r->z_convexity);
        cJSON_AddNumberToObject(e, "blended_option_score",r->blended_option_score);
        cJSON_AddNumberToObject(e, "signed_edge",         r->bates.signed_edge);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(o, "ranked", arr);

    send_obj(fd, o);
    cJSON_Delete(o);

    free(bates_rows);
    free(ps);
    free(ranked);
    free_symbols(syms, n_syms);
}

/* ── Public dispatch ────────────────────────────────────────────── */

void ipc_research_init(void)    { /* no-op */ }
void ipc_research_cleanup(void) { /* no-op */ }

int ipc_research_dispatch(int client_fd, const char *cmd, cJSON *root) {
    if (!cmd) return 0;
    if      (!strcmp(cmd, "sp500_list"))           { cmd_sp500_list(client_fd);                 return 1; }
    else if (!strcmp(cmd, "crawl_news"))           { cmd_crawl_news(client_fd, root);           return 1; }
    else if (!strcmp(cmd, "get_news_digest"))      { cmd_get_news_digest(client_fd, root);      return 1; }
    else if (!strcmp(cmd, "news_jump_status"))     { cmd_news_jump_status(client_fd, root);     return 1; }
    else if (!strcmp(cmd, "heston_path_bundle"))   { cmd_heston_path_bundle(client_fd, root);   return 1; }
    else if (!strcmp(cmd, "heston_surface"))       { cmd_heston_surface(client_fd, root);       return 1; }
    else if (!strcmp(cmd, "heston_diagnostics"))   { cmd_heston_diagnostics(client_fd, root);   return 1; }
    else if (!strcmp(cmd, "bates_backtest_run"))   { cmd_bates_backtest_run(client_fd, root);   return 1; }
    else if (!strcmp(cmd, "option_ranking_blend")) { cmd_option_ranking_blend(client_fd, root); return 1; }
    return 0;
}
