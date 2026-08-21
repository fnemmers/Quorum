#include "news_jump.h"
#include "db.h"
#include <libpq-fe.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Keyword classifier ────────────────────────────────────────────
 *
 * Two priority-ordered pattern tables. First-hit-wins on `event_class`
 * across the merged list. Each hit contributes to (mu_j_bias,
 * sigma_j_bump) so the per-article contribution is bounded per-hit.
 * Case-insensitive substring match — cheap, deterministic, no regex.
 */

typedef struct {
    const char *needle;
    const char *event_class;
    int         priority;   /* higher wins if two hits on same article */
    double      d_mu;       /* additive mu_j delta                      */
    double      d_sigma;    /* additive sigma_j delta                   */
    double      d_sent;     /* additive sentiment scalar in [-1, 1]     */
} KeywordPattern;

static const KeywordPattern PATTERNS[] = {
    /* --- Negative events (order matters when tied on priority) --- */
    { "earnings miss",       "earn",    9, -0.020, 0.020, -0.6 },
    { "misses estimates",    "earn",    9, -0.020, 0.020, -0.6 },
    { "misses expectations", "earn",    9, -0.020, 0.020, -0.6 },
    { "below estimates",     "earn",    8, -0.015, 0.015, -0.4 },
    { "worse than expected", "earn",    8, -0.015, 0.015, -0.5 },
    { "cuts guidance",       "guide",   8, -0.020, 0.020, -0.6 },
    { "guidance cut",        "guide",   8, -0.020, 0.020, -0.6 },
    { "lowers guidance",     "guide",   8, -0.018, 0.018, -0.5 },
    { "warns on outlook",    "guide",   7, -0.015, 0.015, -0.5 },
    { "downgrade",           "down",    7, -0.015, 0.010, -0.4 },
    { "downgraded",          "down",    7, -0.015, 0.010, -0.4 },
    { "lawsuit",             "lit",     6, -0.010, 0.020, -0.3 },
    { "sued",                "lit",     6, -0.010, 0.020, -0.3 },
    { "sec charges",         "lit",     7, -0.020, 0.025, -0.5 },
    { "investigation",       "lit",     6, -0.010, 0.020, -0.3 },
    { "probe",               "lit",     5, -0.008, 0.015, -0.2 },
    { "layoffs",             "layoffs", 5, -0.008, 0.010, -0.3 },
    { "job cuts",            "layoffs", 5, -0.008, 0.010, -0.3 },
    { "restructuring",       "layoffs", 4, -0.005, 0.010, -0.2 },
    { "recall",              "lit",     6, -0.012, 0.020, -0.3 },
    { "bankruptcy",          "lit",    10, -0.050, 0.050, -1.0 },
    { "chapter 11",          "lit",    10, -0.050, 0.050, -1.0 },

    /* --- Positive events --- */
    { "beats estimates",     "beat",    8, +0.015, 0.005, +0.6 },
    { "tops estimates",      "beat",    8, +0.015, 0.005, +0.6 },
    { "beat expectations",   "beat",    8, +0.015, 0.005, +0.5 },
    { "raises guidance",     "guide",   8, +0.018, 0.005, +0.6 },
    { "lifts guidance",      "guide",   8, +0.018, 0.005, +0.6 },
    { "raises outlook",      "guide",   7, +0.015, 0.005, +0.5 },
    { "upgrade",             "up",      7, +0.015, 0.005, +0.4 },
    { "upgraded",            "up",      7, +0.015, 0.005, +0.4 },
    { "buy rating",          "up",      6, +0.012, 0.005, +0.4 },
    { "buyback",             "buyback", 6, +0.010, 0.005, +0.3 },
    { "share repurchase",    "buyback", 6, +0.010, 0.005, +0.3 },
    { "dividend hike",       "buyback", 5, +0.008, 0.005, +0.3 },
    { "acquisition",         "ma",      7, +0.015, 0.015, +0.4 },
    { "to acquire",          "ma",      7, +0.015, 0.015, +0.4 },
    { "acquires",            "ma",      7, +0.015, 0.015, +0.4 },
    { "merger",              "ma",      7, +0.012, 0.015, +0.3 },
    { "buyout offer",        "ma",      8, +0.030, 0.020, +0.7 },
};

static const int N_PATTERNS =
    (int)(sizeof(PATTERNS) / sizeof(PATTERNS[0]));

/* Case-insensitive substring search. */
static int contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle) return 0;
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return 1;
    }
    return 0;
}

typedef struct {
    int    n_articles;
    double mu_sum;
    double sigma_sum;
    double sent_sum;
    int    sent_n;
    int    best_priority;
    const char *best_class;
} SymbolAgg;

static void classify_article(const char *title, const char *description,
                             SymbolAgg *agg) {
    /* Concatenate title + " " + description into a scratch buffer for
     * the ci-substring pass. Cap at 2 KB — headlines are short, and the
     * description is usually the first 150 chars on Polygon anyway. */
    char blob[2048];
    int tl = title       ? (int)strlen(title)       : 0;
    int dl = description ? (int)strlen(description) : 0;
    if (tl + dl + 2 >= (int)sizeof(blob)) {
        int cap = (int)sizeof(blob) - 2;
        if (tl > cap) tl = cap;
        int dcap = cap - tl - 1;
        if (dl > dcap) dl = dcap < 0 ? 0 : dcap;
    }
    int off = 0;
    if (tl > 0) { memcpy(blob, title, (size_t)tl); off += tl; }
    blob[off++] = ' ';
    if (dl > 0) { memcpy(blob + off, description, (size_t)dl); off += dl; }
    blob[off] = '\0';

    for (int i = 0; i < N_PATTERNS; i++) {
        if (!contains_ci(blob, PATTERNS[i].needle)) continue;
        agg->mu_sum    += PATTERNS[i].d_mu;
        agg->sigma_sum += PATTERNS[i].d_sigma;
        agg->sent_sum  += PATTERNS[i].d_sent;
        agg->sent_n    += 1;
        if (PATTERNS[i].priority > agg->best_priority) {
            agg->best_priority = PATTERNS[i].priority;
            agg->best_class    = PATTERNS[i].event_class;
        }
    }
}

/* Split the tickers CSV in-place. Fills up to max symbols pointer-array,
 * returns the count. Whitespace-trims and uppercases. */
static int split_tickers(char *csv, char **out, int max) {
    int n = 0;
    char *p = csv;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        char *start = p;
        while (*p && *p != ',') p++;
        char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        char save = *end;
        *end = '\0';
        if (*start) {
            for (char *q = start; *q; q++) *q = (char)toupper((unsigned char)*q);
            out[n++] = start;
        }
        if (save == ',') p++;
    }
    return n;
}

/* ── DB upsert helper ─────────────────────────────────────────────── */

static int upsert_signal(PGconn *conn, const NewsJumpSignal *s) {
    char as_of[32], win[16], nart[16];
    char sent[32], lam[32], mu[32], sig[32];
    snprintf(as_of, sizeof(as_of), "%lld", (long long)s->as_of);
    snprintf(win,   sizeof(win),   "%d",   s->window_hours);
    snprintf(nart,  sizeof(nart),  "%d",   s->n_articles);
    snprintf(sent,  sizeof(sent),  "%.6f", s->sentiment_avg);
    snprintf(lam,   sizeof(lam),   "%.6f", s->lam_bump);
    snprintf(mu,    sizeof(mu),    "%.6f", s->mu_j_bias);
    snprintf(sig,   sizeof(sig),   "%.6f", s->sigma_j_bump);

    const char *vals[9] = {
        s->symbol, as_of, win, nart, sent,
        s->event_class[0] ? s->event_class : "",
        lam, mu, sig
    };

    PGresult *res = PQexecParams(conn,
        "INSERT INTO news_jump_signal "
        "(symbol, as_of, window_hours, n_articles, sentiment_avg, "
        " event_class, lam_bump, mu_j_bias, sigma_j_bump) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9) "
        "ON CONFLICT (symbol, as_of) DO UPDATE SET "
        "  window_hours  = EXCLUDED.window_hours, "
        "  n_articles    = EXCLUDED.n_articles, "
        "  sentiment_avg = EXCLUDED.sentiment_avg, "
        "  event_class   = EXCLUDED.event_class, "
        "  lam_bump      = EXCLUDED.lam_bump, "
        "  mu_j_bias     = EXCLUDED.mu_j_bias, "
        "  sigma_j_bump  = EXCLUDED.sigma_j_bump",
        9, NULL, vals, NULL, NULL, 0);

    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok)
        fprintf(stderr, "[news_jump] upsert %s failed: %s\n",
                s->symbol, PQerrorMessage(conn));
    PQclear(res);
    return ok ? 0 : -1;
}

/* ── Public API ───────────────────────────────────────────────────── */

int news_jump_recompute(int64_t as_of_ms, int window_hours) {
    PGconn *conn = db_get_conn();
    if (!conn) return -1;
    if (window_hours <= 0) window_hours = 48;

    int64_t from_ms = as_of_ms - (int64_t)window_hours * 3600LL * 1000LL;

    char from_buf[32], to_buf[32];
    snprintf(from_buf, sizeof(from_buf), "%lld", (long long)from_ms);
    snprintf(to_buf,   sizeof(to_buf),   "%lld", (long long)as_of_ms);
    const char *vals[2] = { from_buf, to_buf };

    PGresult *res = PQexecParams(conn,
        "SELECT title, description, tickers "
        "FROM news_cache "
        "WHERE published_at > $1 AND published_at <= $2",
        2, NULL, vals, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "[news_jump] recompute select failed: %s\n",
                PQerrorMessage(conn));
        PQclear(res);
        return -1;
    }

    int n_rows = PQntuples(res);

    /* Open-addressing hash map: symbol -> SymbolAgg. Small — S&P 500 is
     * the universe upper bound, so 4096 buckets is more than enough. */
    enum { N_BUCKETS = 4096 };
    typedef struct { char sym[MAX_SYMBOL_LEN]; SymbolAgg agg; int used; } Bucket;
    Bucket *buckets = (Bucket *)calloc(N_BUCKETS, sizeof(Bucket));
    if (!buckets) { PQclear(res); return -1; }

    /* FNV-1a for the bucket index. */
    #define SYM_HASH(s, out) do {                                    \
        uint64_t _h = 0xcbf29ce484222325ULL;                          \
        for (const char *_c = (s); *_c; _c++) {                       \
            _h ^= (uint8_t)*_c;                                        \
            _h *= 0x100000001b3ULL;                                    \
        }                                                              \
        out = (size_t)(_h & (N_BUCKETS - 1));                          \
    } while (0)

    for (int r = 0; r < n_rows; r++) {
        const char *title       = PQgetvalue(res, r, 0);
        const char *description = PQgetvalue(res, r, 1);
        const char *tickers_csv = PQgetvalue(res, r, 2);
        if (!tickers_csv || !*tickers_csv) continue;

        /* Classify once per article; then attribute to every ticker. */
        SymbolAgg per_article = {0};
        classify_article(title, description, &per_article);

        char csv_copy[1024];
        strncpy(csv_copy, tickers_csv, sizeof(csv_copy) - 1);
        csv_copy[sizeof(csv_copy) - 1] = '\0';

        char *toks[32];
        int nt = split_tickers(csv_copy, toks, 32);

        for (int j = 0; j < nt; j++) {
            size_t idx;
            SYM_HASH(toks[j], idx);
            /* linear probe */
            for (int probe = 0; probe < N_BUCKETS; probe++) {
                Bucket *b = &buckets[(idx + probe) & (N_BUCKETS - 1)];
                if (!b->used) {
                    b->used = 1;
                    strncpy(b->sym, toks[j], MAX_SYMBOL_LEN - 1);
                    b->sym[MAX_SYMBOL_LEN - 1] = '\0';
                }
                if (strcmp(b->sym, toks[j]) == 0) {
                    b->agg.n_articles    += 1;
                    b->agg.mu_sum        += per_article.mu_sum;
                    b->agg.sigma_sum     += per_article.sigma_sum;
                    b->agg.sent_sum      += per_article.sent_sum;
                    b->agg.sent_n        += per_article.sent_n;
                    if (per_article.best_priority > b->agg.best_priority) {
                        b->agg.best_priority = per_article.best_priority;
                        b->agg.best_class    = per_article.best_class;
                    }
                    break;
                }
            }
        }
    }
    PQclear(res);

    int n_written = 0;
    for (int i = 0; i < N_BUCKETS; i++) {
        if (!buckets[i].used) continue;
        NewsJumpSignal s;
        memset(&s, 0, sizeof(s));
        strncpy(s.symbol, buckets[i].sym, MAX_SYMBOL_LEN - 1);
        s.as_of        = as_of_ms;
        s.window_hours = window_hours;
        s.n_articles   = buckets[i].agg.n_articles;

        /* lam_bump: 0.25 per article, capped at 4.0. */
        double lam = 0.25 * (double)s.n_articles;
        if (lam > 4.0) lam = 4.0;
        s.lam_bump = lam;

        /* mu / sigma bumps: raw sums with soft caps. */
        double mu    = buckets[i].agg.mu_sum;
        double sigma = buckets[i].agg.sigma_sum;
        if (mu    >  0.20) mu    =  0.20;
        if (mu    < -0.20) mu    = -0.20;
        if (sigma >  0.10) sigma =  0.10;
        if (sigma <  0.00) sigma =  0.00;
        s.mu_j_bias    = mu;
        s.sigma_j_bump = sigma;

        s.sentiment_avg = buckets[i].agg.sent_n > 0
            ? buckets[i].agg.sent_sum / (double)buckets[i].agg.sent_n
            : 0.0;
        if (s.sentiment_avg >  1.0) s.sentiment_avg =  1.0;
        if (s.sentiment_avg < -1.0) s.sentiment_avg = -1.0;

        if (buckets[i].agg.best_class) {
            strncpy(s.event_class, buckets[i].agg.best_class,
                    sizeof(s.event_class) - 1);
            s.event_class[sizeof(s.event_class) - 1] = '\0';
        }

        if (upsert_signal(conn, &s) == 0) n_written++;
    }

    free(buckets);
    return n_written;
    #undef SYM_HASH
}

int news_jump_get(const char *symbol, int64_t as_of_ms, NewsJumpSignal *out) {
    if (!symbol || !out) return -1;
    PGconn *conn = db_get_conn();
    if (!conn) return -1;

    char as_of_buf[32];
    snprintf(as_of_buf, sizeof(as_of_buf), "%lld", (long long)as_of_ms);
    const char *vals[2] = { symbol, as_of_buf };

    PGresult *res = PQexecParams(conn,
        "SELECT as_of, window_hours, n_articles, sentiment_avg, "
        "       event_class, lam_bump, mu_j_bias, sigma_j_bump "
        "FROM news_jump_signal "
        "WHERE symbol = $1 AND as_of <= $2 "
        "ORDER BY as_of DESC LIMIT 1",
        2, NULL, vals, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        return -1;
    }
    if (PQntuples(res) == 0) { PQclear(res); return 0; }

    memset(out, 0, sizeof(*out));
    strncpy(out->symbol, symbol, MAX_SYMBOL_LEN - 1);
    out->as_of        = (int64_t)atoll(PQgetvalue(res, 0, 0));
    out->window_hours = atoi(PQgetvalue(res, 0, 1));
    out->n_articles   = atoi(PQgetvalue(res, 0, 2));
    out->sentiment_avg= atof(PQgetvalue(res, 0, 3));
    strncpy(out->event_class, PQgetvalue(res, 0, 4),
            sizeof(out->event_class) - 1);
    out->lam_bump     = atof(PQgetvalue(res, 0, 5));
    out->mu_j_bias    = atof(PQgetvalue(res, 0, 6));
    out->sigma_j_bump = atof(PQgetvalue(res, 0, 7));

    PQclear(res);
    return 1;
}

int news_jump_apply(HestonParams *p, const char *symbol, int64_t as_of_ms) {
    if (!p || !symbol) return -1;
    NewsJumpSignal s;
    int r = news_jump_get(symbol, as_of_ms, &s);
    if (r <= 0) return r;

    p->lam     += s.lam_bump;
    p->mu_j    += s.mu_j_bias;
    p->sigma_j += s.sigma_j_bump;
    if (p->sigma_j < 0.0) p->sigma_j = 0.0;
    return 1;
}
