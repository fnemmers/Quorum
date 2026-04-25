// signal_engine.cpp — Reads MarketSnapshot JSON files and emits BUY/SELL/HOLD.
//
// Strategy (priority order):
//   1. RSI + volume spike  →  strongest signal
//      BUY:  RSI < rsi_buy  AND volume spike
//      BUY:  RSI < rsi_buy  (no spike — weaker, conf=0.4)
//      SELL: RSI > rsi_sell
//   2. MACD crossover      →  secondary signal
//      BUY:  macd > signal AND macd > 0
//      SELL: macd < signal AND macd < 0
//   3. News sentiment only →  weakest signal (conf=0.3)
//      BUY:  sentiment > +0.4
//      SELL: sentiment < -0.4
//
// Sentiment modifier (applied to RSI thresholds):
//   sentiment > +0.3 → extend buy window by +5 RSI points
//   sentiment < -0.3 → tighten sell window by -5 RSI points
//
// Two operating modes:
//   Batch mode (default): reads every *.json in --in directory, prints table.
//   IPC mode (--ipc):     listens for the market crawler, processes in real-time.
//
// Optional:
//   --news-dir data/news   load crawled headlines from news_crawler for real sentiment
//
// Usage:
//   ./signal_engine --in data/market [--news-dir data/news] [--rsi-buy 30] [--rsi-sell 70]
//   ./signal_engine --ipc /tmp/market_ipc.sock [--news-dir data/news]
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <optional>
#include <filesystem>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// ── Lightweight JSON loader (no external dependency) ──────────────────────────

static double extract_num(const std::string& json, const std::string& key) {
    for (const auto& needle : {'"' + key + "\": ", '"' + key + "\":"}) {
        auto pos = json.find(needle);
        if (pos == std::string::npos) continue;
        pos += needle.size();
        while (pos < json.size() && json[pos] == ' ') ++pos;
        try { return std::stod(json.substr(pos)); } catch (...) { return 0.0; }
    }
    return 0.0;
}

static bool extract_bool(const std::string& json, const std::string& key) {
    for (const auto& needle : {'"' + key + "\": ", '"' + key + "\":"}) {
        auto pos = json.find(needle);
        if (pos == std::string::npos) continue;
        pos += needle.size();
        while (pos < json.size() && json[pos] == ' ') ++pos;
        return json.substr(pos, 4) == "true";
    }
    return false;
}

static std::string extract_str(const std::string& json, const std::string& key) {
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

// Load avg_sentiment from a crawled news file (data/news/{TICKER}_news.json).
// Returns 0.0 if the file doesn't exist or can't be parsed.
static double load_crawled_sentiment(const std::string& ticker,
                                     const std::string& news_dir) {
    if (news_dir.empty()) return 0.0;
    const std::string path = news_dir + "/" + ticker + "_news.json";
    std::ifstream f(path);
    if (!f) return 0.0;
    std::string json((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    return extract_num(json, "avg_sentiment");
}

// Fields we care about from a MarketSnapshot JSON file
struct SnapshotSummary {
    std::string ticker;
    double      price          = 0.0;
    long long   volume         = 0;
    double      news_sentiment = 0.0;
    double      sma_20         = 0.0;
    double      ema_12         = 0.0;
    double      rsi_14         = 50.0;
    double      macd_val       = 0.0;
    double      macd_sig       = 0.0;
    double      bb_upper       = 0.0;
    double      bb_lower       = 0.0;
    bool        vol_spike      = false;
    std::string timestamp;
};

static std::optional<SnapshotSummary> load_snapshot(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::string json((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    SnapshotSummary s;
    s.ticker         = extract_str(json, "ticker");
    s.timestamp      = extract_str(json, "timestamp");
    s.price          = extract_num(json, "price");
    s.volume         = static_cast<long long>(extract_num(json, "volume"));
    s.news_sentiment = extract_num(json, "news_sentiment");

    // Technical indicators are nested inside "indicators": { ... }
    auto ind_pos = json.find("\"indicators\":");
    const std::string& ind = (ind_pos != std::string::npos)
                             ? json.substr(ind_pos)
                             : json;
    s.sma_20   = extract_num(ind, "sma_20");
    s.ema_12   = extract_num(ind, "ema_12");
    s.rsi_14   = extract_num(ind, "rsi_14");
    s.macd_val = extract_num(ind, "macd");
    s.macd_sig = extract_num(ind, "macd_signal");
    s.bb_upper = extract_num(ind, "bb_upper");
    s.bb_lower = extract_num(ind, "bb_lower");
    s.vol_spike = extract_bool(ind, "vol_spike");

    if (s.ticker.empty()) return std::nullopt;
    return s;
}

// ── Signal generation ──────────────────────────────────────────────────────────

enum class Signal { BUY, SELL, HOLD };

static const char* sig_str(Signal s) {
    switch (s) {
        case Signal::BUY:  return "BUY ";
        case Signal::SELL: return "SELL";
        default:           return "HOLD";
    }
}

struct SignalResult {
    Signal      signal;
    double      confidence = 0.0;   // [0.0, 1.0]
    std::string reason;
};

static SignalResult evaluate(const SnapshotSummary& s,
                             double rsi_buy  = 30.0,
                             double rsi_sell = 70.0) {
    // Sentiment adjusts thresholds (more positive news = wider buy window)
    double buy_thr  = rsi_buy;
    double sell_thr = rsi_sell;
    if      (s.news_sentiment >  0.3) buy_thr  += 5.0;
    else if (s.news_sentiment < -0.3) sell_thr -= 5.0;

    // ── Primary: RSI + volume ──────────────────────────────────────────────────
    if (s.rsi_14 < buy_thr) {
        double base_conf = (buy_thr - s.rsi_14) / buy_thr;
        if (s.vol_spike) {
            // Strong: oversold AND institutional volume entering
            double conf = std::min(1.0, base_conf + 0.2);
            if (s.news_sentiment > 0) conf = std::min(1.0, conf + 0.1);
            return {Signal::BUY, conf,
                    "RSI=" + std::to_string(static_cast<int>(s.rsi_14)) +
                    " oversold + volume spike"};
        }
        // Weaker: oversold but no volume confirmation
        return {Signal::BUY, 0.4,
                "RSI=" + std::to_string(static_cast<int>(s.rsi_14)) +
                " oversold (no vol spike)"};
    }

    if (s.rsi_14 > sell_thr) {
        double conf = (s.rsi_14 - sell_thr) / (100.0 - sell_thr);
        if (s.news_sentiment < 0) conf = std::min(1.0, conf + 0.1);
        return {Signal::SELL, std::min(1.0, conf),
                "RSI=" + std::to_string(static_cast<int>(s.rsi_14)) + " overbought"};
    }

    // ── Secondary: MACD crossover ──────────────────────────────────────────────
    if (s.macd_val > s.macd_sig && s.macd_val > 0)
        return {Signal::BUY,  0.45, "MACD bullish crossover"};
    if (s.macd_val < s.macd_sig && s.macd_val < 0)
        return {Signal::SELL, 0.45, "MACD bearish crossover"};

    // ── Tertiary: sentiment-only (weakest) ─────────────────────────────────────
    if (s.news_sentiment >  0.4)
        return {Signal::BUY,  0.30, "Strong positive news sentiment"};
    if (s.news_sentiment < -0.4)
        return {Signal::SELL, 0.30, "Strong negative news sentiment"};

    return {Signal::HOLD, 1.0, "No clear signal — wait"};
}

// ── Print helpers ──────────────────────────────────────────────────────────────

static void print_row(const SnapshotSummary& s, const SignalResult& r) {
    std::cout << std::setw(6)  << s.ticker
              << "  " << sig_str(r.signal)
              << "  conf="   << std::fixed << std::setprecision(2) << r.confidence
              << "  price="  << std::setprecision(2) << s.price
              << "  rsi="    << std::setprecision(1) << s.rsi_14
              << "  sent="   << std::setprecision(2) << s.news_sentiment
              << "  spike="  << (s.vol_spike ? "Y" : "n")
              << "  →  " << r.reason << "\n";
}

// ── IPC listener mode ──────────────────────────────────────────────────────────

static void run_ipc_mode(const std::string& ipc_path,
                         double rsi_buy, double rsi_sell) {
    ::unlink(ipc_path.c_str());
    int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, ipc_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(srv, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
        { perror("bind"); ::close(srv); return; }
    if (::listen(srv, 1) < 0)
        { perror("listen"); ::close(srv); return; }

    std::cout << "[signal] Listening on " << ipc_path << " ...\n";
    int cli = ::accept(srv, nullptr, nullptr);
    if (cli < 0) { perror("accept"); ::close(srv); return; }
    std::cout << "[signal] Market crawler connected.\n\n";

    std::cout << std::string(80, '-') << "\n";

    // Read lines from the crawler: TICKER\t<ticker>\t<json_path>\n  or  DONE\n
    auto read_line = [](int fd, std::string& line) -> bool {
        line.clear();
        char c;
        while (true) {
            ssize_t n = ::read(fd, &c, 1);
            if (n <= 0) return false;
            if (c == '\n') return true;
            line += c;
        }
    };

    std::string line;
    int buys = 0, sells = 0, holds = 0;
    while (read_line(cli, line)) {
        if (line.empty()) continue;
        if (line == "DONE") break;
        if (line.substr(0, 6) != "TICKER") continue;

        std::istringstream ss(line);
        std::string type, ticker, path;
        std::getline(ss, type,   '\t');
        std::getline(ss, ticker, '\t');
        std::getline(ss, path,   '\t');

        auto snap = load_snapshot(path);
        if (!snap) { std::cerr << "[signal] Cannot load " << path << "\n"; continue; }

        auto result = evaluate(*snap, rsi_buy, rsi_sell);
        print_row(*snap, result);

        switch (result.signal) {
            case Signal::BUY:  ++buys;  break;
            case Signal::SELL: ++sells; break;
            default:           ++holds; break;
        }
    }
    std::cout << std::string(80, '-') << "\n"
              << "Summary: BUY=" << buys << "  SELL=" << sells << "  HOLD=" << holds << "\n";

    ::close(cli);
    ::close(srv);
    ::unlink(ipc_path.c_str());
}

// ── Batch mode ─────────────────────────────────────────────────────────────────

static void run_batch_mode(const std::string& in_dir,
                           const std::string& news_dir,
                           double rsi_buy, double rsi_sell) {
    std::cout << std::string(80, '-') << "\n"
              << std::setw(6)  << "TICKER"
              << "  SIG "
              << " CONF  PRICE    RSI    SENT  SPK  REASON\n"
              << std::string(80, '-') << "\n";

    int buys = 0, sells = 0, holds = 0;
    for (const auto& entry : std::filesystem::directory_iterator(in_dir)) {
        if (entry.path().extension() != ".json") continue;
        // Skip OHLCV files named <TICKER>_ohlcv.csv — shouldn't be here, but guard
        const std::string stem = entry.path().stem().string();
        if (stem.find("_ohlcv") != std::string::npos) continue;

        auto snap = load_snapshot(entry.path().string());
        if (!snap) continue;

        // Override sentiment with real crawled data if available
        double crawled = load_crawled_sentiment(snap->ticker, news_dir);
        if (crawled != 0.0) snap->news_sentiment = crawled;

        auto result = evaluate(*snap, rsi_buy, rsi_sell);
        print_row(*snap, result);

        switch (result.signal) {
            case Signal::BUY:  ++buys;  break;
            case Signal::SELL: ++sells; break;
            default:           ++holds; break;
        }
    }
    std::cout << std::string(80, '-') << "\n"
              << "Summary: BUY=" << buys << "  SELL=" << sells << "  HOLD=" << holds << "\n";
}

// ── CLI ────────────────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cerr
        << "Usage:\n"
        << "  Batch: " << prog << " --in <data/market> [--rsi-buy 30] [--rsi-sell 70]\n"
        << "  IPC:   " << prog << " --ipc <socket>     [--rsi-buy 30] [--rsi-sell 70]\n\n"
        << "  Reads MarketSnapshot JSON files and prints BUY/SELL/HOLD signals.\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) { print_usage(argv[0]); return 1; }

    std::string in_dir, ipc_path, news_dir;
    double rsi_buy = 30.0, rsi_sell = 70.0;

    for (int i = 1; i + 1 < argc; ++i) {
        std::string f = argv[i], v = argv[i + 1];
        if      (f == "--in")        { in_dir   = v;               ++i; }
        else if (f == "--ipc")       { ipc_path = v;               ++i; }
        else if (f == "--news-dir")  { news_dir = v;               ++i; }
        else if (f == "--rsi-buy")   { rsi_buy  = std::stod(v);    ++i; }
        else if (f == "--rsi-sell")  { rsi_sell = std::stod(v);    ++i; }
    }

    if (!ipc_path.empty())
        run_ipc_mode(ipc_path, rsi_buy, rsi_sell);
    else if (!in_dir.empty())
        run_batch_mode(in_dir, news_dir, rsi_buy, rsi_sell);
    else {
        print_usage(argv[0]); return 1;
    }
    return 0;
}
