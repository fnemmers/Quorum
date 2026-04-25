// polygon_client.h — Thin HTTP client for the Polygon.io REST API.
//
// Endpoints used:
//   /v2/aggs/ticker/{T}/range/1/day/{from}/{to}   → OHLCV bars
//   /v2/last/trade/{T}                             → latest trade price
//   /v2/reference/news?ticker={T}                 → news headlines
//
// JSON is parsed with lightweight manual scanning — no external library needed.
// Reuses the existing Fetcher class (one instance per PolygonClient).
#pragma once
#include <string>
#include <vector>
#include <functional>
#include "fetcher.h"
#include "market_data.h"

// ── Minimal JSON helpers ───────────────────────────────────────────────────────

namespace json_util {

// Extract the first numeric value for `key` from a JSON string.
// Handles both "key": value  and  "key":value formats.
inline double get_double(const std::string& json, const std::string& key) {
    for (const auto& needle : {'"' + key + "\": ", '"' + key + "\":"}) {
        auto pos = json.find(needle);
        if (pos == std::string::npos) continue;
        pos += needle.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n')) ++pos;
        try { return std::stod(json.substr(pos)); } catch (...) { return 0.0; }
    }
    return 0.0;
}

inline long long get_ll(const std::string& json, const std::string& key) {
    return static_cast<long long>(get_double(json, key));
}

// Extract first string value for `key`.
inline std::string get_string(const std::string& json, const std::string& key) {
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

// Iterate over every top-level JSON object { ... } found in `json`.
// Each object substring is passed to `cb`.  Handles nested braces correctly.
inline void each_object(const std::string& json,
                        std::function<void(const std::string&)> cb) {
    size_t pos = 0;
    while ((pos = json.find('{', pos)) != std::string::npos) {
        int   depth = 0;
        size_t start = pos;
        for (size_t i = pos; i < json.size(); ++i) {
            if      (json[i] == '{') ++depth;
            else if (json[i] == '}') {
                if (--depth == 0) {
                    cb(json.substr(start, i - start + 1));
                    pos = i + 1;
                    break;
                }
            }
        }
        if (depth != 0) break;   // unmatched brace — malformed JSON
    }
}

} // namespace json_util

// ── PolygonClient ──────────────────────────────────────────────────────────────

class PolygonClient {
public:
    explicit PolygonClient(std::string api_key)
        : api_key_(std::move(api_key)) {}

    // Fetch daily OHLCV bars for `ticker` between `from_date` and `to_date`
    // (format: "YYYY-MM-DD").  Returns bars oldest-first.
    std::vector<OHLCVBar> fetch_ohlcv(const std::string& ticker,
                                      const std::string& from_date,
                                      const std::string& to_date,
                                      int limit = 120) {
        const std::string url =
            "https://api.polygon.io/v2/aggs/ticker/" + ticker +
            "/range/1/day/" + from_date + "/" + to_date +
            "?adjusted=true&sort=asc&limit=" + std::to_string(limit) +
            "&apiKey=" + api_key_;

        auto res = fetcher_.fetch(url);
        if (!res.success) return {};

        std::vector<OHLCVBar> bars;
        json_util::each_object(res.content, [&](const std::string& obj) {
            // Bar objects contain "o" (open); skip the outer response wrapper
            if (obj.find("\"o\":") == std::string::npos &&
                obj.find("\"o\": ") == std::string::npos) return;

            OHLCVBar b;
            b.timestamp_ms = json_util::get_ll    (obj, "t");
            b.open         = json_util::get_double(obj, "o");
            b.high         = json_util::get_double(obj, "h");
            b.low          = json_util::get_double(obj, "l");
            b.close        = json_util::get_double(obj, "c");
            b.volume       = json_util::get_ll    (obj, "v");
            if (b.close > 0) bars.push_back(b);
        });
        return bars;
    }

    // Fetch the last trade price for `ticker`.  Returns 0.0 on failure.
    double fetch_last_price(const std::string& ticker) {
        const std::string url =
            "https://api.polygon.io/v2/last/trade/" + ticker +
            "?apiKey=" + api_key_;
        auto res = fetcher_.fetch(url);
        if (!res.success) return 0.0;
        return json_util::get_double(res.content, "p");
    }

    // Fetch recent news headlines for `ticker` (newest-first).
    // Returns up to `limit` items with headline, URL, and published timestamp.
    std::vector<NewsItem> fetch_news(const std::string& ticker, int limit = 10) {
        const std::string url =
            "https://api.polygon.io/v2/reference/news"
            "?ticker=" + ticker +
            "&limit=" + std::to_string(limit) +
            "&order=desc&sort=published_utc" +
            "&apiKey=" + api_key_;

        auto res = fetcher_.fetch(url);
        if (!res.success) return {};

        std::vector<NewsItem> items;
        json_util::each_object(res.content, [&](const std::string& obj) {
            const std::string headline = json_util::get_string(obj, "title");
            if (headline.empty()) return;
            NewsItem ni;
            ni.headline     = headline;
            ni.url          = json_util::get_string(obj, "article_url");
            ni.published_at = json_util::get_string(obj, "published_utc");
            items.push_back(std::move(ni));
        });
        return items;
    }

private:
    std::string api_key_;
    Fetcher     fetcher_;
};
