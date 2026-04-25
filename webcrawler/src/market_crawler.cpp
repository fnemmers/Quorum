// market_crawler.cpp — Financial market data crawler.
//
// For each requested ticker:
//   1. Fetches OHLCV bars + latest price from Polygon.io
//   2. Fetches recent news headlines from Polygon.io
//   3. Computes technical indicators: SMA-20, EMA-12, RSI-14, MACD, Bollinger Bands
//   4. Detects volume spikes (current bar >= 2× 20-bar rolling average)
//   5. Scores news sentiment from headline keywords
//   6. Writes <out>/<TICKER>.json  (full MarketSnapshot)
//      Writes <out>/<TICKER>_ohlcv.csv  (raw bars for backtesting)
//   7. (Optional) sends TICKER IPC message to the signal engine
//
// Architecture:
//   - Tickers are distributed across a thread pool via BoundedQueue.
//   - Each worker gets its own PolygonClient (wraps one Fetcher / libcurl handle).
//   - IPC is optional; the crawler runs standalone if --ipc is not given.
//
// Usage:
//   ./market_crawler --tickers AAPL,MSFT,GOOG \
//                   --api-key <polygon_key>   \
//                   --from 2024-01-01          \
//                   --to   2024-04-19          \
//                   --out  data/market         \
//                   [-t <threads>]             \
//                   [--ipc /tmp/market_ipc.sock]
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <curl/curl.h>

#include "bounded_queue.h"
#include "market_data.h"
#include "technical_indicators.h"
#include "sentiment.h"
#include "polygon_client.h"

// ── Config ─────────────────────────────────────────────────────────────────────

struct MarketCrawlerConfig {
    std::vector<std::string> tickers;
    std::string api_key;
    std::string from_date   = "2024-01-01";
    std::string to_date     = "2024-04-19";
    std::string out_dir     = "data/market";
    std::string ipc_path    = "/tmp/market_ipc.sock";
    int         num_threads = 4;
    int         news_limit  = 10;
    int         bar_limit   = 120;
    int         delay_ms    = 0;
};

// ── IPC helpers ────────────────────────────────────────────────────────────────

static int ipc_connect(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    for (int i = 0; i < 3; ++i) {
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0)
            return fd;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    ::close(fd);
    return -1;
}

static std::mutex g_ipc_mu;
static void ipc_send(int fd, const std::string& msg) {
    if (fd < 0) return;
    std::lock_guard<std::mutex> lk(g_ipc_mu);
    size_t sent = 0;
    while (sent < msg.size()) {
        ssize_t n = ::write(fd, msg.c_str() + sent, msg.size() - sent);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

// ── Indicator computation ──────────────────────────────────────────────────────

static void populate_indicators(MarketSnapshot& snap) {
    if (snap.bars.empty()) return;

    std::vector<double> closes, vols;
    closes.reserve(snap.bars.size());
    vols.reserve(snap.bars.size());
    for (const auto& b : snap.bars) {
        closes.push_back(b.close);
        vols.push_back(static_cast<double>(b.volume));
    }

    // SMA-20
    { auto s = indicators::sma(closes, 20);   snap.sma_20 = s.back(); }

    // EMA-12
    { auto e = indicators::ema(closes, 12);   snap.ema_12 = e.back(); }

    // RSI-14
    { auto r = indicators::rsi(closes, 14);   snap.rsi_14 = r.back(); }

    // MACD (12-26-9)
    {
        auto m = indicators::macd(closes, 12, 26, 9);
        snap.macd_val = m.macd.back();
        snap.macd_sig = m.signal.back();
    }

    // Bollinger Bands (20, 2σ)
    {
        auto bb     = indicators::bollinger(closes, 20, 2.0);
        snap.bb_upper = bb.upper.back();
        snap.bb_lower = bb.lower.back();
    }

    // Volume spike: current bar >= 2× 20-bar average
    snap.vol_spike = indicators::volume_spike(vols, 20, 2.0);
}

// ── ISO-8601 UTC timestamp ─────────────────────────────────────────────────────

static std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// ── Per-ticker work ────────────────────────────────────────────────────────────

static std::mutex g_log_mu;

static void process_ticker(const std::string& ticker,
                           const MarketCrawlerConfig& cfg,
                           int ipc_fd) {
    PolygonClient client(cfg.api_key);

    MarketSnapshot snap;
    snap.ticker    = ticker;
    snap.timestamp = now_iso8601();

    // 1. OHLCV bars
    snap.bars = client.fetch_ohlcv(ticker, cfg.from_date, cfg.to_date, cfg.bar_limit);
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        std::cout << "[market] " << ticker << "  bars=" << snap.bars.size() << "\n";
    }

    // 2. Latest price from last trade; volume from most recent bar
    double quote_price = client.fetch_last_price(ticker);
    if (!snap.bars.empty()) {
        snap.price  = (quote_price > 0.0) ? quote_price : snap.bars.back().close;
        snap.volume = snap.bars.back().volume;
    } else {
        snap.price  = quote_price;
        snap.volume = 0;
    }

    // 3. Technical indicators (requires bars)
    populate_indicators(snap);

    // 4. News headlines + per-headline sentiment
    snap.news = client.fetch_news(ticker, cfg.news_limit);
    double sent_total = 0.0;
    for (auto& ni : snap.news) {
        ni.sentiment_score = sentiment::score_headline(ni.headline);
        sent_total += ni.sentiment_score;
    }
    if (!snap.news.empty())
        snap.news_sentiment = sent_total / static_cast<double>(snap.news.size());

    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        std::cout << "[market] " << std::setw(6) << ticker
                  << "  price="     << std::fixed << std::setprecision(2) << snap.price
                  << "  rsi="       << std::setprecision(1) << snap.rsi_14
                  << "  macd="      << std::setprecision(3) << snap.macd_val
                  << "  sentiment=" << std::setprecision(2) << snap.news_sentiment
                  << "  vol_spike=" << (snap.vol_spike ? "YES" : "no")
                  << "\n";
    }

    // 5. Write JSON snapshot
    const std::string json_path = cfg.out_dir + "/" + ticker + ".json";
    {
        std::ofstream ofs(json_path);
        if (ofs) ofs << snap.to_json();
        else {
            std::lock_guard<std::mutex> lk(g_log_mu);
            std::cerr << "[market] Cannot write " << json_path << "\n";
        }
    }

    // 6. Write OHLCV CSV (raw bars for backtesting)
    const std::string csv_path = cfg.out_dir + "/" + ticker + "_ohlcv.csv";
    {
        std::ofstream ofs(csv_path);
        if (ofs) {
            ofs << OHLCVBar::CSV_HEADER << "\n";
            for (const auto& b : snap.bars)
                ofs << b.to_csv_row() << "\n";
        }
    }

    // 7. IPC notification so signal engine can react immediately
    if (ipc_fd >= 0)
        ipc_send(ipc_fd, "TICKER\t" + ticker + "\t" + json_path + "\n");
}

// ── CLI parsing ────────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << "\n"
        << "  --tickers AAPL,MSFT,GOOG   (comma-separated)\n"
        << "  --api-key  <polygon_key>\n"
        << "  [--from    YYYY-MM-DD]     default: 2024-01-01\n"
        << "  [--to      YYYY-MM-DD]     default: 2024-04-19\n"
        << "  [--out      <dir>]          default: data/market\n"
        << "  [-t         <threads>]     default: 1  (AV free tier: single-threaded)\n"
        << "  [--delay-ms <ms>]          default: 15000  (15 s between tickers)\n"
        << "  [--ipc      <socket>]      optional: stream to signal engine\n";
}

static MarketCrawlerConfig parse_args(int argc, char* argv[]) {
    MarketCrawlerConfig cfg;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string f = argv[i], v = argv[i + 1];
        if (f == "--tickers") {
            std::istringstream ss(v);
            std::string tok;
            while (std::getline(ss, tok, ','))
                if (!tok.empty()) cfg.tickers.push_back(tok);
            ++i;
        } else if (f == "--api-key")   { cfg.api_key     = v; ++i; }
          else if (f == "--from")      { cfg.from_date   = v; ++i; }
          else if (f == "--to")        { cfg.to_date     = v; ++i; }
          else if (f == "--out")       { cfg.out_dir     = v; ++i; }
          else if (f == "-t")          { cfg.num_threads = std::stoi(v); ++i; }
          else if (f == "--ipc")       { cfg.ipc_path    = v; ++i; }
          else if (f == "--delay-ms")  { cfg.delay_ms    = std::stoi(v); ++i; }
    }
    return cfg;
}

// ── Main ───────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 5) { print_usage(argv[0]); return 1; }

    auto cfg = parse_args(argc, argv);

    if (cfg.tickers.empty()) {
        std::cerr << "[market] Error: --tickers is required\n"; return 1;
    }
    if (cfg.api_key.empty()) {
        std::cerr << "[market] Error: --api-key is required\n"; return 1;
    }

    std::filesystem::create_directories(cfg.out_dir);
    curl_global_init(CURL_GLOBAL_ALL);

    std::cout << "[market] tickers=" << cfg.tickers.size()
              << "  range=" << cfg.from_date << ".." << cfg.to_date
              << "  threads=" << cfg.num_threads
              << "  out=" << cfg.out_dir << "\n";

    // Try to connect to the signal engine IPC (optional)
    int ipc_fd = ipc_connect(cfg.ipc_path);
    if (ipc_fd >= 0)
        std::cout << "[market] Connected to signal engine IPC.\n";

    // Fill the work queue with all tickers, then shut it so workers drain + exit
    BoundedQueue<std::string> queue(cfg.tickers.size() + 4);
    for (const auto& t : cfg.tickers) queue.push(t);
    queue.shutdown();

    std::atomic<int> done{0};
    std::vector<std::thread> threads;
    threads.reserve(cfg.num_threads);
    for (int i = 0; i < cfg.num_threads; ++i) {
        threads.emplace_back([&]() {
            bool first = true;
            while (auto item = queue.pop()) {
                if (!first && cfg.delay_ms > 0)
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(cfg.delay_ms));
                first = false;
                process_ticker(*item, cfg, ipc_fd);
                ++done;
            }
        });
    }
    for (auto& t : threads) t.join();

    if (ipc_fd >= 0) {
        ipc_send(ipc_fd, "DONE\n");
        ::close(ipc_fd);
    }

    curl_global_cleanup();
    std::cout << "[market] Done. Processed " << done.load() << " ticker(s).\n";
    return 0;
}
