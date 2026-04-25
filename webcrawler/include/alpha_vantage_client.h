// alpha_vantage_client.h — HTTP client for the Alpha Vantage REST API.
//
// Endpoints used:
//   function=TIME_SERIES_DAILY   → OHLCV bars  ("Time Series (Daily)")
//   function=GLOBAL_QUOTE        → latest price/volume ("Global Quote")
//   function=NEWS_SENTIMENT      → headlines + AV sentiment (premium; graceful fallback)
//
// Drop-in replacement for PolygonClient — same three public method signatures:
//   fetch_ohlcv(ticker, from, to, limit)  → vector<OHLCVBar>
//   fetch_last_price(ticker)              → double
//   fetch_news(ticker, limit)             → vector<NewsItem>
//
// Alpha Vantage JSON quirk: every OHLCV value is a quoted string, not a number.
// e.g.  "1. open": "189.2300"   not   "1. open": 189.23
//
// Rate limits (free tier): 25 req/day, ~5 req/min.
// Call sleep_between_tickers() between consecutive ticker batches.
#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>
#include <sstream>
#include <curl/curl.h>
#include "market_data.h"

// ── JSON-aware HTTP fetch (AV returns application/json; the web-crawler Fetcher
//    sends Accept: text/html which causes AV to return HTTP 406) ───────────────

namespace av_http {

struct Result {
    bool        success  = false;
    std::string content;
    long        http_code = 0;
};

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* data) {
    auto* buf = static_cast<std::string*>(data);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

inline Result fetch(const std::string& url) {
    Result res;
    CURL* curl = curl_easy_init();
    if (!curl) return res;

    std::string body;
    char err_buf[CURL_ERROR_SIZE] = {};

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER,    err_buf);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING,"");  // auto decompress

    // AV requires Accept: application/json (or */*); text/html → HTTP 406
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    if (curl_easy_perform(curl) == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.http_code);
        if (res.http_code >= 200 && res.http_code < 300) {
            res.content = std::move(body);
            res.success = true;
        }
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return res;
}

} // namespace av_http

// ── Helpers ───────────────────────────────────────────────────────────────────

namespace av_util {

// Extract a string-valued field from JSON.
// Handles both  "key": "val"  and  "key":"val"  spacing.
inline std::string get_str(const std::string& json, const std::string& key) {
    for (const auto& needle : {'"' + key + "\": \"", '"' + key + "\":\""}) {
        auto pos = json.find(needle);
        if (pos == std::string::npos) continue;
        pos += needle.size();
        auto end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    }
    return "";
}

// Extract a numeric field that is stored as a JSON string, e.g. "05. price": "189.23"
inline double get_num_str(const std::string& json, const std::string& key) {
    const std::string s = get_str(json, key);
    if (s.empty()) return 0.0;
    try { return std::stod(s); } catch (...) { return 0.0; }
}

inline long long get_ll_str(const std::string& json, const std::string& key) {
    const std::string s = get_str(json, key);
    if (s.empty()) return 0LL;
    try { return std::stoll(s); } catch (...) { return 0LL; }
}

// Convert "YYYY-MM-DD" → Unix epoch milliseconds (midnight UTC).
// Uses timegm() which interprets tm as UTC (POSIX; available on macOS + Linux).
inline long long date_to_ms(const std::string& date) {
    if (date.size() < 10) return 0LL;
    struct tm t{};
    try {
        t.tm_year = std::stoi(date.substr(0, 4)) - 1900;
        t.tm_mon  = std::stoi(date.substr(5, 2)) - 1;
        t.tm_mday = std::stoi(date.substr(8, 2));
    } catch (...) { return 0LL; }
    time_t epoch = timegm(&t);
    return static_cast<long long>(epoch) * 1000LL;
}

// Parse every "YYYY-MM-DD": { ... } entry inside the "Time Series (Daily)" section.
// Alpha Vantage returns dates newest-first; we reverse to oldest-first to match
// the convention used by indicators (which expect chronological order).
inline std::vector<OHLCVBar> parse_time_series(const std::string& json) {
    std::vector<OHLCVBar> bars;

    // Find the time-series section
    const std::string section_key = "\"Time Series (Daily)\":";
    auto section_pos = json.find(section_key);
    if (section_pos == std::string::npos) return bars;
    size_t pos = section_pos + section_key.size();

    // Scan for date-format keys: "YYYY-MM-DD"
    while (pos < json.size()) {
        auto q1 = json.find('"', pos);
        if (q1 == std::string::npos) break;
        auto q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos) break;

        const std::string key = json.substr(q1 + 1, q2 - q1 - 1);

        // Date keys are exactly "YYYY-MM-DD" (10 chars with dashes at 4 and 7)
        bool is_date = (key.size() == 10 && key[4] == '-' && key[7] == '-');
        if (!is_date) {
            pos = q2 + 1;
            continue;
        }

        // Find the object { ... } that follows the colon
        auto colon = json.find(':', q2 + 1);
        if (colon == std::string::npos) break;
        auto brace_open = json.find('{', colon + 1);
        if (brace_open == std::string::npos) break;

        // Walk to matching close brace
        int depth = 0;
        size_t brace_close = brace_open;
        for (size_t i = brace_open; i < json.size(); ++i) {
            if      (json[i] == '{') ++depth;
            else if (json[i] == '}') { if (--depth == 0) { brace_close = i; break; } }
        }
        if (depth != 0) break;

        const std::string obj = json.substr(brace_open, brace_close - brace_open + 1);

        OHLCVBar bar;
        bar.timestamp_ms = date_to_ms(key);
        bar.open         = get_num_str(obj, "1. open");
        bar.high         = get_num_str(obj, "2. high");
        bar.low          = get_num_str(obj, "3. low");
        bar.close        = get_num_str(obj, "4. close");
        bar.volume       = get_ll_str (obj, "5. volume");

        if (bar.close > 0.0) bars.push_back(bar);

        pos = brace_close + 1;
    }

    // AV returns newest-first; reverse so bars are oldest-first (expected by indicators)
    std::reverse(bars.begin(), bars.end());
    return bars;
}

// Check if the response contains a rate-limit or access message from AV.
inline bool is_error_response(const std::string& body) {
    return body.find("\"Information\":") != std::string::npos ||
           body.find("\"Note\":") != std::string::npos ||
           body.find("\"Error Message\":") != std::string::npos;
}

} // namespace av_util

// ── AlphaVantageClient ────────────────────────────────────────────────────────

class AlphaVantageClient {
public:
    static constexpr const char* BASE = "https://www.alphavantage.co/query";

    explicit AlphaVantageClient(std::string api_key)
        : api_key_(std::move(api_key)) {}

    // Fetch up to `limit` daily OHLCV bars for `ticker`.
    // `from_date` / `to_date` are used for filtering after fetch
    // (AV's compact mode returns the last ~100 bars; full returns 20+ years).
    // Uses outputsize=compact when limit<=100, full otherwise.
    std::vector<OHLCVBar> fetch_ohlcv(const std::string& ticker,
                                       const std::string& from_date,
                                       const std::string& to_date,
                                       int limit = 100) {
        const std::string size = (limit <= 100) ? "compact" : "full";
        const std::string url =
            std::string(BASE) +
            "?function=TIME_SERIES_DAILY"
            "&symbol="      + ticker  +
            "&outputsize="  + size    +
            "&apikey="      + api_key_;

        auto res = av_http::fetch(url);
        if (!res.success || av_util::is_error_response(res.content)) return {};

        auto bars = av_util::parse_time_series(res.content);

        // Filter to the requested date range (AV doesn't support from/to params)
        if (!from_date.empty() || !to_date.empty()) {
            long long from_ms = from_date.empty() ? 0LL
                                                  : av_util::date_to_ms(from_date);
            long long to_ms   = to_date.empty()   ? LLONG_MAX
                                                  : av_util::date_to_ms(to_date) + 86400000LL;
            bars.erase(std::remove_if(bars.begin(), bars.end(), [&](const OHLCVBar& b) {
                return b.timestamp_ms < from_ms || b.timestamp_ms > to_ms;
            }), bars.end());
        }

        // Respect the caller's limit (take the most recent `limit` bars)
        if (static_cast<int>(bars.size()) > limit)
            bars.erase(bars.begin(), bars.begin() + (bars.size() - limit));

        return bars;
    }

    // Fetch current price from GLOBAL_QUOTE.
    // Field "05. price" is the last traded price.
    double fetch_last_price(const std::string& ticker) {
        const std::string url =
            std::string(BASE) +
            "?function=GLOBAL_QUOTE"
            "&symbol=" + ticker +
            "&apikey=" + api_key_;

        auto res = av_http::fetch(url);
        if (!res.success || av_util::is_error_response(res.content)) return 0.0;

        // Also capture volume from quote for the snapshot
        last_quote_volume_ = av_util::get_ll_str(res.content, "06. volume");
        return av_util::get_num_str(res.content, "05. price");
    }

    // Volume captured by the most recent fetch_last_price() call.
    long long last_quote_volume() const { return last_quote_volume_; }

    // Fetch recent news headlines for `ticker` via NEWS_SENTIMENT.
    // This is a premium endpoint — returns empty list if not available.
    // When available, stores AV's own sentiment score in NewsItem::sentiment_score.
    std::vector<NewsItem> fetch_news(const std::string& ticker, int limit = 10) {
        const std::string url =
            std::string(BASE) +
            "?function=NEWS_SENTIMENT"
            "&tickers=" + ticker +
            "&limit="   + std::to_string(limit) +
            "&sort=LATEST"
            "&apikey="  + api_key_;

        auto res = av_http::fetch(url);
        if (!res.success || av_util::is_error_response(res.content)) return {};

        // Parse "feed": [ { "title": "...", "url": "...",
        //                   "time_published": "20240419T120000",
        //                   "overall_sentiment_score": 0.35 }, ... ]
        std::vector<NewsItem> items;
        size_t feed_pos = res.content.find("\"feed\":");
        if (feed_pos == std::string::npos) return {};

        size_t pos = feed_pos;
        while (pos < res.content.size()) {
            auto brace = res.content.find('{', pos + 1);
            if (brace == std::string::npos) break;

            int depth = 0;
            size_t end = brace;
            for (size_t i = brace; i < res.content.size(); ++i) {
                if      (res.content[i] == '{') ++depth;
                else if (res.content[i] == '}') { if (--depth == 0) { end = i; break; } }
            }
            if (depth != 0) break;

            const std::string obj = res.content.substr(brace, end - brace + 1);
            const std::string title = av_util::get_str(obj, "title");
            if (title.empty()) { pos = end + 1; continue; }

            NewsItem ni;
            ni.headline     = title;
            ni.url          = av_util::get_str(obj, "url");
            ni.published_at = av_util::get_str(obj, "time_published");

            // Use AV's pre-computed score when present; caller can override with
            // the keyword scorer in sentiment.h if preferred.
            const std::string av_score_str = av_util::get_str(obj, "overall_sentiment_score");
            if (!av_score_str.empty()) {
                try { ni.sentiment_score = std::stod(av_score_str); } catch (...) {}
            }

            items.push_back(std::move(ni));
            pos = end + 1;
            if (static_cast<int>(items.size()) >= limit) break;
        }
        return items;
    }

private:
    std::string api_key_;
    long long   last_quote_volume_ = 0;
};
