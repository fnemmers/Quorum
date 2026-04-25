// news_crawler.cpp — Live financial news crawler.
//
// For each ticker, crawls two public sources and extracts real headlines:
//   1. Yahoo Finance  https://finance.yahoo.com/quote/{T}/news/  → <h3> tags
//   2. Finviz         https://finviz.com/quote.ashx?t={T}        → class="tab-link-news"
//
// For each headline:
//   - Scores sentiment with the keyword table in sentiment.h
//   - Records source URL and crawl timestamp
//
// Output:
//   data/news/{TICKER}_news.json  — array of headline objects
//
// These files are consumed by signal_engine (--news-dir flag) to add real
// sentiment to the BUY/SELL/HOLD decision.
//
// Usage:
//   ./news_crawler --tickers AAPL,MSFT --out data/news [-t 2] [--delay-ms 2000]
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <curl/curl.h>

#include "bounded_queue.h"
#include "fetcher.h"
#include "html_parser.h"
#include "sentiment.h"

// ── Config ─────────────────────────────────────────────────────────────────────

struct NewsCrawlerConfig {
    std::vector<std::string> tickers;
    std::string out_dir     = "data/news";
    int         num_threads = 2;
    int         delay_ms    = 2000;   // between requests to the same site
    int         max_headlines = 50;   // cap per ticker
};

// ── One scraped headline ────────────────────────────────────────────────────────

struct RawHeadline {
    std::string text;
    std::string source;   // "yahoo" or "finviz"
    double      sentiment = 0.0;
};

// ── HTTP fetch (accepts text/html — correct for news sites) ───────────────────

static std::mutex g_log_mu;

static std::string fetch_html(const std::string& url) {
    // Fetcher already uses Accept: text/html which is correct here
    Fetcher f;
    auto res = f.fetch(url);
    if (!res.success) {
        std::lock_guard<std::mutex> lk(g_log_mu);
        std::cerr << "[news] FAIL " << url << "  (" << res.error << ")\n";
        return {};
    }
    return res.content;
}

// ── Source scrapers ────────────────────────────────────────────────────────────

// Yahoo Finance news page: headlines are in <h3> tags
static std::vector<RawHeadline> scrape_yahoo(const std::string& ticker) {
    const std::string url =
        "https://finance.yahoo.com/quote/" + ticker + "/news/";
    const std::string html = fetch_html(url);
    if (html.empty()) return {};

    auto texts = html_parser::extract_headlines(html);

    std::vector<RawHeadline> out;
    out.reserve(texts.size());
    for (auto& t : texts) {
        RawHeadline h;
        h.text      = std::move(t);
        h.source    = "yahoo";
        h.sentiment = sentiment::score_headline(h.text);
        out.push_back(std::move(h));
    }
    return out;
}

// Finviz quote page: news uses <a class="tab-link-news">
static std::vector<RawHeadline> scrape_finviz(const std::string& ticker) {
    const std::string url =
        "https://finviz.com/quote.ashx?t=" + ticker;
    const std::string html = fetch_html(url);
    if (html.empty()) return {};

    auto texts = html_parser::extract_anchor_text_by_class(html, "tab-link-news");

    std::vector<RawHeadline> out;
    out.reserve(texts.size());
    for (auto& t : texts) {
        RawHeadline h;
        h.text      = std::move(t);
        h.source    = "finviz";
        h.sentiment = sentiment::score_headline(h.text);
        out.push_back(std::move(h));
    }
    return out;
}

// ── Deduplication ──────────────────────────────────────────────────────────────

// Remove near-duplicate headlines: if the first 60 chars match, keep the first.
static void deduplicate(std::vector<RawHeadline>& headlines) {
    std::unordered_set<std::string> seen;
    auto it = headlines.begin();
    while (it != headlines.end()) {
        std::string key = it->text.substr(0, std::min<size_t>(60, it->text.size()));
        // Lowercase key for comparison
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (!seen.insert(key).second)
            it = headlines.erase(it);
        else
            ++it;
    }
}

// ── JSON output ────────────────────────────────────────────────────────────────

static std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

static std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else                out += c;
    }
    return out;
}

static void write_news_json(const std::string& ticker,
                            const std::vector<RawHeadline>& headlines,
                            const std::string& out_dir) {
    const std::string path = out_dir + "/" + ticker + "_news.json";
    std::ofstream f(path);
    if (!f) {
        std::lock_guard<std::mutex> lk(g_log_mu);
        std::cerr << "[news] Cannot write " << path << "\n";
        return;
    }

    // Compute aggregate sentiment
    double total = 0.0;
    for (const auto& h : headlines) total += h.sentiment;
    double avg_sent = headlines.empty() ? 0.0 : total / headlines.size();

    f << std::fixed << std::setprecision(4);
    f << "{\n"
      << "  \"ticker\": \""           << ticker       << "\",\n"
      << "  \"crawled_at\": \""       << now_iso8601()<< "\",\n"
      << "  \"headline_count\": "     << headlines.size() << ",\n"
      << "  \"avg_sentiment\": "      << avg_sent     << ",\n"
      << "  \"headlines\": [\n";

    for (size_t i = 0; i < headlines.size(); ++i) {
        const auto& h = headlines[i];
        f << "    { \"text\": \""      << escape_json(h.text) << "\""
          << ", \"source\": \""        << h.source             << "\""
          << ", \"sentiment\": "       << h.sentiment          << " }";
        if (i + 1 < headlines.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n}\n";
}

// ── Per-ticker work ────────────────────────────────────────────────────────────

static void process_ticker(const std::string& ticker,
                           const NewsCrawlerConfig& cfg) {
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        std::cout << "[news] crawling " << ticker << " ...\n";
    }

    // Fetch from both sources; brief delay between them
    auto yahoo  = scrape_yahoo(ticker);
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.delay_ms));
    auto finviz = scrape_finviz(ticker);

    // Merge, deduplicate, cap
    std::vector<RawHeadline> all;
    all.reserve(yahoo.size() + finviz.size());
    for (auto& h : yahoo)  all.push_back(std::move(h));
    for (auto& h : finviz) all.push_back(std::move(h));
    deduplicate(all);
    if (static_cast<int>(all.size()) > cfg.max_headlines)
        all.resize(cfg.max_headlines);

    // Summary
    double total = 0.0;
    for (const auto& h : all) total += h.sentiment;
    double avg = all.empty() ? 0.0 : total / all.size();

    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        std::cout << "[news] " << std::setw(6) << ticker
                  << "  headlines=" << all.size()
                  << "  avg_sentiment=" << std::fixed << std::setprecision(3) << avg
                  << "\n";
        // Print top headlines with their scores
        for (size_t i = 0; i < std::min<size_t>(5, all.size()); ++i) {
            const auto& h = all[i];
            std::cout << "       ["
                      << (h.sentiment > 0 ? "+" : "")
                      << std::setprecision(2) << h.sentiment
                      << "] " << h.text.substr(0, 80);
            if (h.text.size() > 80) std::cout << "...";
            std::cout << "\n";
        }
    }

    write_news_json(ticker, all, cfg.out_dir);
}

// ── CLI parsing ────────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << "\n"
        << "  --tickers  AAPL,MSFT,NVDA\n"
        << "  [--out     data/news]     output directory\n"
        << "  [-t        <threads>]     default: 2\n"
        << "  [--delay-ms <ms>]         delay between requests per ticker (default: 2000)\n";
}

static NewsCrawlerConfig parse_args(int argc, char* argv[]) {
    NewsCrawlerConfig cfg;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string f = argv[i], v = argv[i + 1];
        if (f == "--tickers") {
            std::istringstream ss(v);
            std::string tok;
            while (std::getline(ss, tok, ','))
                if (!tok.empty()) cfg.tickers.push_back(tok);
            ++i;
        } else if (f == "--out")       { cfg.out_dir     = v;               ++i; }
          else if (f == "-t")          { cfg.num_threads = std::stoi(v);    ++i; }
          else if (f == "--delay-ms")  { cfg.delay_ms    = std::stoi(v);    ++i; }
    }
    return cfg;
}

// ── Main ───────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 3) { print_usage(argv[0]); return 1; }

    auto cfg = parse_args(argc, argv);
    if (cfg.tickers.empty()) {
        std::cerr << "[news] Error: --tickers required\n"; return 1;
    }

    std::filesystem::create_directories(cfg.out_dir);
    curl_global_init(CURL_GLOBAL_ALL);

    std::cout << "[news] tickers=" << cfg.tickers.size()
              << "  out=" << cfg.out_dir
              << "  threads=" << cfg.num_threads << "\n";

    BoundedQueue<std::string> queue(cfg.tickers.size() + 4);
    for (const auto& t : cfg.tickers) queue.push(t);
    queue.shutdown();

    std::atomic<int> done{0};
    std::vector<std::thread> threads;
    threads.reserve(cfg.num_threads);
    for (int i = 0; i < cfg.num_threads; ++i) {
        threads.emplace_back([&]() {
            while (auto item = queue.pop()) {
                process_ticker(*item, cfg);
                ++done;
            }
        });
    }
    for (auto& t : threads) t.join();

    curl_global_cleanup();
    std::cout << "[news] Done. Crawled " << done.load() << " ticker(s).\n";
    std::cout << "[news] Run signals:  ./signal_engine --in data/market --news-dir data/news\n";
    return 0;
}
