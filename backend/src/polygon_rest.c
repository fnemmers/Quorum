#include "polygon_rest.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POLYGON_BASE "https://api.polygon.io"

static char g_api_key[256];

/* ── libcurl write callback – accumulates response into a growable buffer ── */

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

/* ── Generic GET helper ─────────────────────────────────────────────── */

static char *polygon_get(const char *url) {
    CURL    *curl = curl_easy_init();
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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[REST] curl error: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    return buf.data;  /* caller must free */
}

/* ── Public API ─────────────────────────────────────────────────────── */

void polygon_rest_init(const char *api_key) {
    strncpy(g_api_key, api_key, sizeof(g_api_key) - 1);
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void polygon_rest_cleanup(void) {
    curl_global_cleanup();
}

int polygon_rest_aggregates(const char *symbol,
                             int         multiplier,
                             const char *timespan,
                             const char *from_date,
                             const char *to_date,
                             OHLCBar    *bars_out,
                             int         max_bars) {
    char url[1024];
    snprintf(url, sizeof(url),
        "%s/v2/aggs/ticker/%s/range/%d/%s/%s/%s"
        "?adjusted=true&sort=asc&limit=%d",
        POLYGON_BASE, symbol, multiplier, timespan,
        from_date, to_date, max_bars);

    char *resp = polygon_get(url);
    if (!resp) return -1;

    cJSON *root    = cJSON_Parse(resp);
    free(resp);
    if (!root) return -1;

    cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (!cJSON_IsArray(results)) { cJSON_Delete(root); return -1; }

    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (count >= max_bars) break;
        OHLCBar *b = &bars_out[count];
        b->timestamp = (int64_t)cJSON_GetNumberValue(
                            cJSON_GetObjectItemCaseSensitive(item, "t"));
        b->open  = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(item, "o"));
        b->high  = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(item, "h"));
        b->low   = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(item, "l"));
        b->close = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(item, "c"));
        b->volume = (int64_t)cJSON_GetNumberValue(
                            cJSON_GetObjectItemCaseSensitive(item, "v"));
        count++;
    }
    cJSON_Delete(root);
    return count;
}

int polygon_rest_snapshot(const char *symbol, Quote *out) {
    char url[512];
    snprintf(url, sizeof(url),
        "%s/v2/snapshot/locale/us/markets/stocks/tickers/%s",
        POLYGON_BASE, symbol);

    char *resp = polygon_get(url);
    if (!resp) return -1;

    cJSON *root   = cJSON_Parse(resp);
    free(resp);
    if (!root) return -1;

    cJSON *ticker  = cJSON_GetObjectItemCaseSensitive(root, "ticker");
    cJSON *day     = cJSON_GetObjectItemCaseSensitive(ticker, "day");
    cJSON *lastq   = cJSON_GetObjectItemCaseSensitive(ticker, "lastQuote");
    cJSON *lastt   = cJSON_GetObjectItemCaseSensitive(ticker, "lastTrade");

    if (!ticker) { cJSON_Delete(root); return -1; }

    strncpy(out->symbol, symbol, MAX_SYMBOL_LEN - 1);
    out->price = cJSON_GetNumberValue(
                     cJSON_GetObjectItemCaseSensitive(lastt, "p"));
    out->bid   = cJSON_GetNumberValue(
                     cJSON_GetObjectItemCaseSensitive(lastq, "P"));
    out->ask   = cJSON_GetNumberValue(
                     cJSON_GetObjectItemCaseSensitive(lastq, "P"));
    out->volume = (int64_t)cJSON_GetNumberValue(
                     cJSON_GetObjectItemCaseSensitive(day, "v"));
    out->timestamp = 0;
    out->valid     = 1;

    cJSON_Delete(root);
    return 0;
}
