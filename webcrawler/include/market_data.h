// market_data.h — Core data structures for market price, volume, and news data.
//
// Defines:
//   OHLCVBar       — one candlestick (open/high/low/close/volume + timestamp)
//   NewsItem       — scraped headline with a sentiment score attached
//   MarketSnapshot — fully-populated per-ticker snapshot written as JSON
#pragma once
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

// ── One OHLCV candlestick ──────────────────────────────────────────────────────

struct OHLCVBar {
    long long timestamp_ms = 0;   // Unix epoch milliseconds (Polygon's "t" field)
    double    open         = 0.0;
    double    high         = 0.0;
    double    low          = 0.0;
    double    close        = 0.0;
    long long volume       = 0;

    static constexpr const char* CSV_HEADER =
        "timestamp_ms,open,high,low,close,volume";

    std::string to_csv_row() const {
        std::ostringstream oss;
        oss << timestamp_ms << ","
            << std::fixed << std::setprecision(4)
            << open  << "," << high << ","
            << low   << "," << close << ","
            << volume;
        return oss.str();
    }
};

// ── One scraped news headline ──────────────────────────────────────────────────

struct NewsItem {
    std::string headline;
    std::string url;
    std::string published_at;
    double      sentiment_score = 0.0;  // [-1.0 bearish … +1.0 bullish]
};

// ── Full per-ticker snapshot ───────────────────────────────────────────────────

// MarketSnapshot is the canonical output of the market crawler.
// It is serialised to JSON (one file per ticker) and forwarded to the signal
// engine via IPC.  The JSON format matches the architecture spec:
//
//   {
//     "ticker": "AAPL",
//     "price": 189.23,
//     "volume": 1200000,
//     "news_sentiment": 0.7,
//     "timestamp": "...",
//     "indicators": { ... },
//     "news": [ ... ]
//   }
struct MarketSnapshot {
    std::string           ticker;
    double                price          = 0.0;
    long long             volume         = 0;
    double                bid            = 0.0;   // best bid (0 if unavailable)
    double                ask            = 0.0;   // best ask (0 if unavailable)
    double                news_sentiment = 0.0;   // average across recent news items
    std::string           timestamp;              // ISO-8601 UTC

    std::vector<OHLCVBar> bars;   // historical bars, oldest-first
    std::vector<NewsItem> news;   // recent headlines (newest-first from API)

    // Derived technical indicators — populated after bars are loaded
    double sma_20    = 0.0;
    double ema_12    = 0.0;
    double rsi_14    = 50.0;   // default 50 = neutral until computed
    double macd_val  = 0.0;
    double macd_sig  = 0.0;
    double bb_upper  = 0.0;
    double bb_lower  = 0.0;
    bool   vol_spike = false;

    // Serialise to JSON string
    std::string to_json() const {
        std::ostringstream j;
        j << std::fixed << std::setprecision(4);
        j << "{\n"
          << "  \"ticker\": \""       << ticker         << "\",\n"
          << "  \"price\": "          << price          << ",\n"
          << "  \"volume\": "         << volume         << ",\n"
          << "  \"bid\": "            << bid            << ",\n"
          << "  \"ask\": "            << ask            << ",\n"
          << "  \"news_sentiment\": " << news_sentiment << ",\n"
          << "  \"timestamp\": \""    << timestamp      << "\",\n"
          << "  \"indicators\": {\n"
          << "    \"sma_20\": "       << sma_20         << ",\n"
          << "    \"ema_12\": "       << ema_12         << ",\n"
          << "    \"rsi_14\": "       << rsi_14         << ",\n"
          << "    \"macd\": "         << macd_val       << ",\n"
          << "    \"macd_signal\": "  << macd_sig       << ",\n"
          << "    \"bb_upper\": "     << bb_upper       << ",\n"
          << "    \"bb_lower\": "     << bb_lower       << ",\n"
          << "    \"vol_spike\": "    << (vol_spike ? "true" : "false") << "\n"
          << "  },\n"
          << "  \"news\": [\n";

        for (size_t i = 0; i < news.size(); ++i) {
            const auto& n = news[i];
            // Escape double-quotes inside headline text
            std::string h = n.headline;
            for (size_t p = 0; (p = h.find('"', p)) != std::string::npos; p += 2)
                h.replace(p, 1, "\\\"");
            j << "    { \"headline\": \""   << h
              << "\", \"sentiment\": "      << n.sentiment_score
              << ", \"published_at\": \""   << n.published_at << "\" }";
            if (i + 1 < news.size()) j << ",";
            j << "\n";
        }
        j << "  ]\n}\n";
        return j.str();
    }
};
