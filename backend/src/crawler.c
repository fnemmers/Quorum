/*
 * crawler.c  --  Polygon.io news fetcher + digest builder.
 *
 * Uses the same libcurl + cJSON patterns as polygon_rest.c.
 * Articles are stored in the news_cache Postgres table (schema in db.c).
 * The digest builder queries recent articles and concatenates them into
 * a plain-text market briefing suitable for an LLM system prompt.
 */

#include "crawler.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* db.c exposes its connection */
extern PGconn *db_get_conn(void);

static char g_api_key[256];

/* ── libcurl write callback (same pattern as polygon_rest.c) ────── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buffer;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t   bytes  = size * nmemb;
    Buffer  *buf    = (Buffer *)userdata;
    size_t   needed = buf->len + bytes + 1;

    if (needed > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 4096;
        while (new_cap < needed) new_cap *= 2;
        char *tmp = realloc(buf->data, new_cap);
        if (!tmp) return 0;
        buf->data = tmp;
        buf->cap  = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, bytes);
    buf->len += bytes;
    buf->data[buf->len] = '\0';
    return bytes;
}

/* ── HTTP GET helper ───────────────────────────────────────────── */

static char *http_get(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    Buffer buf = {0};

    struct curl_slist *headers = NULL;
    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", g_api_key);
    headers = curl_slist_append(headers, auth_hdr);

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "StockApp/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[CRAWLER] curl error: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

/* ── Parse ISO 8601 timestamp to Unix ms ─────────────────────── */

static int64_t parse_iso8601_ms(const char *s) {
    if (!s) return 0;
    int year, month, day, hour, min, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &year, &month, &day,
               &hour, &min, &sec) < 6)
        return 0;

    struct tm tmv = {0};
    tmv.tm_year = year - 1900;
    tmv.tm_mon  = month - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hour;
    tmv.tm_min  = min;
    tmv.tm_sec  = sec;

#ifdef _WIN32
    time_t t = _mkgmtime(&tmv);
#else
    time_t t = timegm(&tmv);
#endif
    return (int64_t)t * 1000LL;
}

/* ── Store one article in Postgres ───────────────────────────── */

static int store_article(const char *article_id,
                         const char *title,
                         const char *description,
                         const char *publisher,
                         const char *url,
                         const char *tickers_csv,
                         int64_t published_ms) {
    PGconn *conn = db_get_conn();
    if (!conn) return -1;

    char s_pub[32];
    snprintf(s_pub, sizeof(s_pub), "%lld", (long long)published_ms);

    const char *vals[] = {
        article_id,
        title       ? title       : "",
        description ? description : "",
        publisher   ? publisher   : "",
        url         ? url         : "",
        tickers_csv ? tickers_csv : "",
        s_pub
    };

    PGresult *res = PQexecParams(conn,
        "INSERT INTO news_cache "
        "  (article_id, title, description, publisher, url, tickers, published_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7) "
        "ON CONFLICT (article_id) DO NOTHING;",
        7, NULL, vals, NULL, NULL, 0);

    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK) ? 0 : -1;
    if (ok != 0)
        fprintf(stderr, "[CRAWLER] insert failed: %s\n", PQerrorMessage(conn));
    PQclear(res);
    return ok;
}

/* ── Public API ────────────────────────────────────────────────── */

void crawler_init(const char *polygon_api_key) {
    strncpy(g_api_key, polygon_api_key, sizeof(g_api_key) - 1);
    printf("[CRAWLER] Initialized\n");
}

int crawler_fetch_news(int limit) {
    if (limit <= 0) limit = 50;
    if (limit > 1000) limit = 1000;

    char url[1024];
    snprintf(url, sizeof(url),
        "https://api.polygon.io/v2/reference/news"
        "?order=desc&limit=%d&sort=published_utc",
        limit);

    printf("[CRAWLER] Fetching up to %d articles...\n", limit);

    char *resp = http_get(url);
    if (!resp) {
        fprintf(stderr, "[CRAWLER] HTTP request failed\n");
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        fprintf(stderr, "[CRAWLER] JSON parse failed\n");
        return -1;
    }

    cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (!cJSON_IsArray(results)) {
        fprintf(stderr, "[CRAWLER] No results array in response\n");
        cJSON_Delete(root);
        return -1;
    }

    int stored = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        cJSON *jid   = cJSON_GetObjectItemCaseSensitive(item, "id");
        cJSON *jtitl = cJSON_GetObjectItemCaseSensitive(item, "title");
        cJSON *jdesc = cJSON_GetObjectItemCaseSensitive(item, "description");
        cJSON *jpub  = cJSON_GetObjectItemCaseSensitive(item, "publisher");
        cJSON *jurl  = cJSON_GetObjectItemCaseSensitive(item, "article_url");
        cJSON *jtime = cJSON_GetObjectItemCaseSensitive(item, "published_utc");
        cJSON *jtick = cJSON_GetObjectItemCaseSensitive(item, "tickers");

        const char *aid = cJSON_IsString(jid) ? jid->valuestring : NULL;
        if (!aid) continue;

        const char *title = cJSON_IsString(jtitl) ? jtitl->valuestring : "";
        const char *desc  = cJSON_IsString(jdesc) ? jdesc->valuestring : "";
        const char *pub_name = "";
        if (jpub && cJSON_IsObject(jpub)) {
            cJSON *pn = cJSON_GetObjectItemCaseSensitive(jpub, "name");
            if (cJSON_IsString(pn)) pub_name = pn->valuestring;
        }
        const char *aurl = cJSON_IsString(jurl) ? jurl->valuestring : "";
        int64_t pub_ms = cJSON_IsString(jtime)
            ? parse_iso8601_ms(jtime->valuestring) : 0;

        /* Build comma-separated tickers string */
        char tickers_csv[512] = "";
        if (jtick && cJSON_IsArray(jtick)) {
            int pos = 0;
            cJSON *tk;
            cJSON_ArrayForEach(tk, jtick) {
                if (!cJSON_IsString(tk)) continue;
                if (pos > 0 && pos < (int)sizeof(tickers_csv) - 1)
                    tickers_csv[pos++] = ',';
                int rem = (int)sizeof(tickers_csv) - 1 - pos;
                int len = (int)strlen(tk->valuestring);
                if (len > rem) len = rem;
                memcpy(tickers_csv + pos, tk->valuestring, len);
                pos += len;
            }
            tickers_csv[pos] = '\0';
        }

        if (store_article(aid, title, desc, pub_name, aurl,
                          tickers_csv, pub_ms) == 0)
            stored++;
    }

    cJSON_Delete(root);
    printf("[CRAWLER] Stored %d articles\n", stored);
    return stored;
}

char *crawler_build_digest(int max_chars, int days) {
    PGconn *conn = db_get_conn();
    if (!conn) return NULL;

    if (max_chars <= 0) max_chars = 32000;
    if (days <= 0) days = 7;

    /* Compute cutoff timestamp */
    int64_t now_ms = (int64_t)time(NULL) * 1000LL;
    int64_t cutoff = now_ms - (int64_t)days * 86400000LL;
    char s_cutoff[32], s_limit[16];
    snprintf(s_cutoff, sizeof(s_cutoff), "%lld", (long long)cutoff);
    snprintf(s_limit,  sizeof(s_limit),  "%d",   200);

    const char *vals[] = { s_cutoff, s_limit };
    PGresult *res = PQexecParams(conn,
        "SELECT title, description, tickers, published_at, publisher "
        "FROM news_cache "
        "WHERE published_at >= $1 "
        "ORDER BY published_at DESC "
        "LIMIT $2;",
        2, NULL, vals, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "[CRAWLER] digest query failed: %s\n",
                PQerrorMessage(conn));
        PQclear(res);
        return NULL;
    }

    int n_rows = PQntuples(res);
    if (n_rows == 0) {
        PQclear(res);
        char *empty = malloc(64);
        if (empty) strcpy(empty, "(No recent news articles available.)");
        return empty;
    }

    /* Allocate output buffer */
    char *digest = malloc((size_t)max_chars + 1);
    if (!digest) { PQclear(res); return NULL; }

    int pos = 0;
    int written = snprintf(digest + pos, max_chars - pos,
        "=== MARKET BRIEFING (last %d days, %d articles) ===\n\n",
        days, n_rows);
    if (written > 0) pos += written;

    for (int i = 0; i < n_rows && pos < max_chars - 200; i++) {
        const char *title   = PQgetvalue(res, i, 0);
        const char *desc    = PQgetvalue(res, i, 1);
        const char *tickers = PQgetvalue(res, i, 2);
        int64_t pub_ms      = atoll(PQgetvalue(res, i, 3));
        const char *pub     = PQgetvalue(res, i, 4);

        /* Format date */
        time_t secs = (time_t)(pub_ms / 1000LL);
        struct tm tmv;
#ifdef _WIN32
        gmtime_s(&tmv, &secs);
#else
        gmtime_r(&secs, &tmv);
#endif
        char date_str[20];
        strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M", &tmv);

        written = snprintf(digest + pos, max_chars - pos,
            "[%s] %s\n", date_str, title);
        if (written > 0) pos += written;

        if (tickers[0] != '\0') {
            written = snprintf(digest + pos, max_chars - pos,
                "  Tickers: %s\n", tickers);
            if (written > 0) pos += written;
        }

        if (pub[0] != '\0') {
            written = snprintf(digest + pos, max_chars - pos,
                "  Source: %s\n", pub);
            if (written > 0) pos += written;
        }

        if (desc[0] != '\0' && pos < max_chars - 100) {
            /* Truncate long descriptions to keep digest balanced */
            int desc_budget = max_chars / n_rows;
            if (desc_budget < 200) desc_budget = 200;
            if (desc_budget > 800) desc_budget = 800;

            int desc_len = (int)strlen(desc);
            if (desc_len > desc_budget) {
                written = snprintf(digest + pos, max_chars - pos,
                    "  %.*s...\n\n", desc_budget, desc);
            } else {
                written = snprintf(digest + pos, max_chars - pos,
                    "  %s\n\n", desc);
            }
            if (written > 0) pos += written;
        } else {
            written = snprintf(digest + pos, max_chars - pos, "\n");
            if (written > 0) pos += written;
        }
    }

    digest[pos] = '\0';
    PQclear(res);

    printf("[CRAWLER] Built digest: %d chars from %d articles\n", pos, n_rows);
    return digest;
}
