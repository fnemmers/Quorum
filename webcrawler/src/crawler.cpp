// crawler.cpp — Concurrent web crawler.
//
// Architecture:
//   - Bounded frontier queue (BoundedQueue<UrlTask>) shared by all workers.
//   - Fixed-size thread pool: each worker pops a task, fetches the page with
//     libcurl, saves the HTML to disk, extracts links, and pushes new tasks.
//   - Thread-safe visited set guards against duplicate crawls.
//   - Atomic tasks_in_flight counter: incremented before every push,
//     decremented after every task completion; when it reaches 0 the crawl
//     is done.
//   - Metadata (docid, url, filepath, depth) is forwarded to the indexer
//     process via a UNIX-domain stream socket using the text protocol:
//       DOC\t<id>\t<url>\t<filepath>\t<depth>\n
//       DONE\n   (sent once at the end)
//
// Usage:
//   ./crawler --seed <url> --max-depth <D> --max-pages <N>
//             -t <threads> --out <dir> --ipc <socket>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <thread>
#include <optional>
#include <condition_variable>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <sys/socket.h>   // socket(), connect(), write()
#include <sys/un.h>       // sockaddr_un — UNIX-domain socket address
#include <unistd.h>       // close()
#include <curl/curl.h>    // HTTP fetching via libcurl

#include "bounded_queue.h"  // Thread-safe blocking queue with a capacity cap
#include "fetcher.h"        // Fetcher: wraps libcurl; returns {success, url, content, error}
#include "html_parser.h"    // html_parser::extract_links — parses <a href> tags
#include "url_utils.h"      // URL normalisation / deduplication helpers

// ── Data structures ────────────────────────────────────────────────────────────

// One unit of work placed on the frontier: a URL to visit and the depth at
// which it was discovered (0 = seed, 1 = one hop away, etc.)
struct UrlTask {
    std::string url;
    int         depth = 0;
};

// All tuneable parameters gathered in one place; populated from CLI flags.
struct CrawlerConfig {
    std::string seed_url;
    int         max_depth   = 3;    // stop following links beyond this hop count
    int         max_pages   = 100;  // hard cap on total pages saved
    int         num_threads = 4;    // worker thread count
    int         queue_cap   = 500;  // max pending tasks in the frontier queue
    std::string out_dir     = "data";
    std::string ipc_path    = "/tmp/crawler_ipc.sock";  // socket to reach the indexer
};

// ── Shared state (one instance, passed by pointer to each worker) ──────────────

// Everything the worker threads need to share. Each field is either immutable
// after construction or protected by its own mutex / atomic.
struct State {
    CrawlerConfig config;

    BoundedQueue<UrlTask>            frontier;    // work queue; blocks workers when empty
    std::unordered_set<std::string>  visited;     // URLs already enqueued (not re-enqueued)
    std::mutex                       visited_mu;  // guards `visited`

    std::atomic<int>  pages_saved{0};       // also serves as the next docid to assign
    std::atomic<int>  tasks_in_flight{0};   // #tasks pushed but not yet finished

    // Main thread waits on done_cv until tasks_in_flight drops to 0
    std::mutex              done_mu;
    std::condition_variable done_cv;
    bool                    all_done = false;

    int        ipc_fd = -1;   // file descriptor for the indexer socket (-1 = not connected)
    std::mutex ipc_mu;        // serialises writes to ipc_fd across worker threads

    std::mutex log_mu;        // serialises stdout/stderr so lines don't interleave

    explicit State(const CrawlerConfig& cfg)
        : config(cfg), frontier(cfg.queue_cap) {}
};

// ── IPC helpers ────────────────────────────────────────────────────────────────

// Connects to the indexer's UNIX-domain socket. Retries every 500 ms for up
// to 5 seconds so the crawler can be launched slightly before the indexer.
// Returns true on success and stores the fd in st.ipc_fd.
static bool ipc_connect(State& st) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, st.config.ipc_path.c_str(),
                 sizeof(addr.sun_path) - 1);

    // Retry for up to 5 seconds while the indexer starts up
    for (int i = 0; i < 10; ++i) {
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr)) == 0) {
            st.ipc_fd = fd;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    ::close(fd);
    return false;
}

// Sends a raw string over the IPC socket, looping until every byte is written.
// Grabs ipc_mu so multiple worker threads can call this concurrently.
static void ipc_write(State& st, const std::string& msg) {
    if (st.ipc_fd < 0) return;  // IPC not available; silently skip
    std::lock_guard<std::mutex> lk(st.ipc_mu);
    size_t sent = 0;
    while (sent < msg.size()) {
        ssize_t n = ::write(st.ipc_fd, msg.c_str() + sent, msg.size() - sent);
        if (n <= 0) break;  // socket closed or error; give up
        sent += n;
    }
}

// Formats and sends a DOC message so the indexer can pick up this document.
// Protocol:  DOC \t docid \t url \t filepath \t depth \n
static void ipc_send_doc(State& st, int docid, const std::string& url,
                         const std::string& filepath, int depth) {
    ipc_write(st, "DOC\t" + std::to_string(docid) + "\t" + url +
                  "\t" + filepath + "\t" + std::to_string(depth) + "\n");
}

// ── Worker thread ──────────────────────────────────────────────────────────────

// Each worker runs this loop until the frontier queue is shut down.
// One Fetcher per thread so each has its own libcurl handle (not thread-safe
// to share).
static void worker(State* st) {
    Fetcher fetcher;

    while (true) {
        // Blocks until a task is available or the queue is shut down
        auto task_opt = st->frontier.pop();
        if (!task_opt) break;          // queue shut down — time to exit
        UrlTask task = std::move(*task_opt);

        // ── Check page limit ─────────────────────────────────────────────────
        // Relaxed load is fine: we'll do an atomic fetch_add below for the
        // exact check. This is just a cheap early exit.
        if (st->pages_saved.load(std::memory_order_relaxed) >= st->config.max_pages)
            goto finish_task;

        {
            // ── Fetch ────────────────────────────────────────────────────────
            auto res = fetcher.fetch(task.url);
            if (!res.success) {
                std::lock_guard<std::mutex> lk(st->log_mu);
                std::cerr << "[crawler] SKIP " << task.url
                          << "  (" << res.error << ")\n";
                goto finish_task;
            }

            // Atomically claim the next docid. If we've already hit max_pages
            // between the check above and here, give the id back and bail.
            int docid = st->pages_saved.fetch_add(1);
            if (docid >= st->config.max_pages) {
                st->pages_saved.fetch_sub(1);
                goto finish_task;
            }

            // ── Save HTML ────────────────────────────────────────────────────
            // File is named by docid so it matches what the indexer expects
            std::string filepath = st->config.out_dir + "/pages/" +
                                   std::to_string(docid) + ".html";
            {
                std::ofstream ofs(filepath);
                if (!ofs) {
                    std::lock_guard<std::mutex> lk(st->log_mu);
                    std::cerr << "[crawler] Cannot write " << filepath << "\n";
                    st->pages_saved.fetch_sub(1);
                    goto finish_task;
                }
                ofs << res.content;
            }

            {
                std::lock_guard<std::mutex> lk(st->log_mu);
                std::cout << "[crawler] [" << std::setw(4) << docid << "] "
                          << res.url << "  (depth=" << task.depth << ")\n";
            }

            // ── Notify indexer ───────────────────────────────────────────────
            // Tell the indexer process about this document over the IPC socket
            ipc_send_doc(*st, docid, res.url, filepath, task.depth);

            // ── Enqueue child links ──────────────────────────────────────────
            // Only follow links if we haven't reached the depth limit
            if (task.depth < st->config.max_depth) {
                auto links = html_parser::extract_links(res.content, res.url);
                for (const auto& link : links) {
                    // Coarse capacity guard: if the queue is already very full
                    // relative to max_pages, stop adding more tasks to avoid
                    // unbounded memory use.
                    int in_flight = st->tasks_in_flight.load(std::memory_order_relaxed);
                    int saved     = st->pages_saved.load(std::memory_order_relaxed);
                    if (saved + in_flight >= st->config.max_pages * 3) break;

                    // Insert into visited set; only enqueue if this URL is new
                    bool inserted = false;
                    {
                        std::lock_guard<std::mutex> lk(st->visited_mu);
                        auto [_, ok] = st->visited.insert(link);
                        inserted = ok;
                    }
                    if (inserted) {
                        // Increment BEFORE push so tasks_in_flight is never
                        // under-counted when another thread checks it
                        st->tasks_in_flight.fetch_add(1);
                        if (!st->frontier.push({link, task.depth + 1})) {
                            // Queue was shut down between the check and push;
                            // undo the pre-increment to keep the count correct
                            st->tasks_in_flight.fetch_sub(1);
                        }
                    }
                }
            }
        }

        // Decrement the in-flight counter. If we're the last task, signal the
        // main thread that the crawl is complete.
        finish_task:
        {
            int remaining = st->tasks_in_flight.fetch_sub(1) - 1;
            if (remaining == 0) {
                std::lock_guard<std::mutex> lk(st->done_mu);
                st->all_done = true;
                st->done_cv.notify_one();
            }
        }
    }
}

// ── CLI parsing ────────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --seed <url>  --max-depth <D>  --max-pages <N>"
              << "  -t <threads>  --out <dir>  --ipc <socket>\n";
}

// Parses all recognised flags into a CrawlerConfig.
// Returns nullopt if --seed is missing or --help is requested.
static std::optional<CrawlerConfig> parse_args(int argc, char* argv[]) {
    CrawlerConfig cfg;
    bool has_seed = false;

    for (int i = 1; i < argc; ++i) {
        std::string f = argv[i];
        if (i + 1 >= argc) {
            if (f == "--help" || f == "-h") { return std::nullopt; }
            break;
        }
        std::string v = argv[i + 1];
        if      (f == "--seed")      { cfg.seed_url    = v; has_seed = true; ++i; }
        else if (f == "--max-depth") { cfg.max_depth   = std::stoi(v); ++i; }
        else if (f == "--max-pages") { cfg.max_pages   = std::stoi(v); ++i; }
        else if (f == "-t")          { cfg.num_threads = std::stoi(v); ++i; }
        else if (f == "--out")       { cfg.out_dir     = v; ++i; }
        else if (f == "--ipc")       { cfg.ipc_path    = v; ++i; }
    }
    if (!has_seed) return std::nullopt;  // --seed is required
    return cfg;
}

// ── Main ───────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    auto cfg_opt = parse_args(argc, argv);
    if (!cfg_opt) { print_usage(argv[0]); return 1; }
    CrawlerConfig cfg = *cfg_opt;

    std::cout << "[crawler] seed="     << cfg.seed_url
              << "  depth="    << cfg.max_depth
              << "  pages="    << cfg.max_pages
              << "  threads="  << cfg.num_threads
              << "  out="      << cfg.out_dir
              << "  ipc="      << cfg.ipc_path << "\n";

    // Ensure output directories exist before workers try to write HTML files
    std::filesystem::create_directories(cfg.out_dir + "/pages");
    std::filesystem::create_directories(cfg.out_dir + "/index");

    // Must be called once globally before any Fetcher is created
    curl_global_init(CURL_GLOBAL_ALL);

    State state(cfg);

    // Connect to the indexer's IPC socket. Non-fatal if the indexer isn't up
    // yet — the crawler will just drop IPC messages and still save HTML files.
    if (!ipc_connect(state))
        std::cerr << "[crawler] WARNING: could not connect to indexer at "
                  << cfg.ipc_path << " — proceeding without IPC\n";
    else
        std::cout << "[crawler] Connected to indexer.\n";

    // Seed the frontier with the starting URL (depth 0)
    // tasks_in_flight must be 1 before the first push so the done check works
    state.visited.insert(cfg.seed_url);
    state.tasks_in_flight.store(1);
    state.frontier.push({cfg.seed_url, 0});

    // Spawn the worker thread pool; each thread gets a raw pointer to State
    std::vector<std::thread> workers;
    workers.reserve(cfg.num_threads);
    for (int i = 0; i < cfg.num_threads; ++i)
        workers.emplace_back(worker, &state);

    // Block here until the last worker decrements tasks_in_flight to 0
    {
        std::unique_lock<std::mutex> lk(state.done_mu);
        state.done_cv.wait(lk, [&state]{ return state.all_done; });
    }

    // Shut down the queue so workers blocked on pop() wake up and exit,
    // then send "DONE" to the indexer and close the socket.
    state.frontier.shutdown();
    ipc_write(state, "DONE\n");
    if (state.ipc_fd >= 0) ::close(state.ipc_fd);

    // Wait for all worker threads to finish before cleaning up libcurl
    for (auto& t : workers) t.join();
    curl_global_cleanup();

    std::cout << "[crawler] Finished. Pages saved: "
              << state.pages_saved.load() << "\n";
    return 0;
}
