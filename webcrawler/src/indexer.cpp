// indexer.cpp — Document indexer process.
//
// Listens on a UNIX-domain socket for the crawler's IPC messages:
//   DOC\t<docid>\t<url>\t<filepath>\t<depth>\n
//   DONE\n
//
// For every DOC received it reads the HTML file saved by the crawler,
// extracts text, tokenises it, and updates the in-memory inverted index.
// On DONE (or connection close) it persists the index to disk.
//
// Usage:
//   ./indexer --ipc <socket> --out <dir>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <optional>
#include <filesystem>
#include <cstring>
#include <sys/socket.h>  // socket(), bind(), listen(), accept()
#include <sys/un.h>      // sockaddr_un — UNIX-domain socket address struct
#include <unistd.h>      // read(), close(), unlink()

#include "inverted_index.h"  // InvertedIndex: maps tokens → list of (docid, positions)
#include "html_parser.h"     // html_parser::extract_text — strips tags, returns plain text
#include "tokenizer.h"       // tokenizer::tokenize — splits text into lowercase tokens

// ── Config ─────────────────────────────────────────────────────────────────────

// Holds the two runtime parameters: where the IPC socket lives and where to
// write output files. Defaults are usable for local testing.
struct IndexerConfig {
    std::string ipc_path = "/tmp/crawler_ipc.sock";  // UNIX socket the crawler connects to
    std::string out_dir  = "data";                   // root directory for pages/ and index/
};

// Parses --ipc and --out flags from argv, fills an IndexerConfig, and exits
// with a usage message if required arguments are missing.
static IndexerConfig parse_args(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " --ipc <socket> --out <dir>\n";
        std::exit(1);
    }
    IndexerConfig cfg;
    for (int i = 1; i < argc - 1; ++i) {
        std::string f = argv[i];   // flag name  (e.g. "--ipc")
        std::string v = argv[i + 1]; // flag value (e.g. "/tmp/crawler.sock")
        if      (f == "--ipc") { cfg.ipc_path = v; ++i; }  // skip the value on next iteration
        else if (f == "--out") { cfg.out_dir  = v; ++i; }
    }
    return cfg;
}

// ── IPC helpers ────────────────────────────────────────────────────────────────

// Reads one newline-terminated line from the file descriptor fd into `line`.
// Reads one byte at a time so it never over-reads into the next message.
// Returns true when a complete line is ready, false on EOF or socket error.
static bool read_line(int fd, std::string& line) {
    line.clear();
    char c;
    while (true) {
        ssize_t n = ::read(fd, &c, 1);
        if (n <= 0) return false;  // EOF (n==0) or error (n==-1)
        if (c == '\n') return true; // newline marks end of message
        line += c;
    }
}

// ── Main ───────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    IndexerConfig cfg = parse_args(argc, argv);
    std::cout << "[indexer] ipc=" << cfg.ipc_path
              << "  out=" << cfg.out_dir << "\n";

    // Create output directories up front so the crawler doesn't race to write
    // HTML files before the directories exist.
    std::filesystem::create_directories(cfg.out_dir + "/pages");  // crawler writes HTML here
    std::filesystem::create_directories(cfg.out_dir + "/index");  // indexer writes index here

    // ── Create UNIX-domain socket server ──────────────────────────────────────

    // Remove any leftover socket file from a prior run so bind() won't fail
    // with EADDRINUSE.
    ::unlink(cfg.ipc_path.c_str());

    // AF_UNIX = local IPC (no networking), SOCK_STREAM = reliable byte stream
    int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    // Fill in the socket address: family + filesystem path
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, cfg.ipc_path.c_str(), sizeof(addr.sun_path) - 1);

    // Bind the socket to the filesystem path so the crawler can find it
    if (::bind(srv, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
        { perror("bind"); return 1; }

    // Mark the socket as passive (server-side); backlog of 1 — only one
    // crawler connects at a time.
    if (::listen(srv, 1) < 0)
        { perror("listen"); return 1; }

    std::cout << "[indexer] Listening on " << cfg.ipc_path << " ...\n";

    // Block until the crawler connects. Returns a new fd for that connection;
    // `srv` stays open only to accept (potential) future connections.
    int cli = ::accept(srv, nullptr, nullptr);
    if (cli < 0) { perror("accept"); return 1; }
    std::cout << "[indexer] Crawler connected.\n";

    // ── Process messages ───────────────────────────────────────────────────────

    InvertedIndex index;            // in-memory token → postings list
    int docs_ok = 0, docs_skip = 0; // counters for final summary

    std::string line;
    // read_line returns false when the crawler closes the connection
    while (read_line(cli, line)) {
        if (line.empty()) continue;

        // "DONE" is a clean shutdown signal from the crawler
        if (line == "DONE") {
            std::cout << "[indexer] Received DONE.\n";
            break;
        }

        // All other valid messages start with "DOC"
        if (line.substr(0, 3) != "DOC") {
            std::cerr << "[indexer] Unknown message: " << line << "\n";
            continue;
        }

        // Parse tab-separated fields: DOC \t docid \t url \t filepath \t depth
        std::istringstream ss(line);
        std::string type, id_s, url, filepath, depth_s;
        std::getline(ss, type,     '\t');  // "DOC" — already checked above
        std::getline(ss, id_s,     '\t');  // numeric document ID (string form)
        std::getline(ss, url,      '\t');  // original URL of the page
        std::getline(ss, filepath, '\t');  // local path to the saved HTML file
        std::getline(ss, depth_s,  '\t');  // crawl depth (distance from seed URL)

        // Convert the string fields that need to be integers
        int docid, depth;
        try {
            docid = std::stoi(id_s);
            depth = std::stoi(depth_s);
        } catch (...) {
            // stoi throws if the string isn't a valid integer
            std::cerr << "[indexer] Malformed message: " << line << "\n";
            ++docs_skip;
            continue;
        }

        // Read the raw HTML that the crawler previously saved to disk
        std::ifstream html_f(filepath);
        if (!html_f) {
            std::cerr << "[indexer] Cannot open " << filepath << "\n";
            ++docs_skip;
            continue;
        }
        // Slurp the entire file into a string using iterator-range constructor
        std::string html((std::istreambuf_iterator<char>(html_f)),
                          std::istreambuf_iterator<char>());

        // Pipeline: raw HTML → plain text → token list → inverted index entry
        std::string text   = html_parser::extract_text(html);             // strip HTML tags
        auto        tokens = tokenizer::tokenize(text, /*filter_stops=*/true); // split + remove stopwords

        // Register this document in the index under all its tokens
        index.add_document(docid, url, filepath, depth, tokens);
        ++docs_ok;

        // Periodic progress log so the user can see the indexer is alive
        if (docs_ok % 10 == 0)
            std::cout << "[indexer] Indexed " << docs_ok << " docs...\n";
    }

    // Clean up both socket file descriptors and remove the socket file
    ::close(cli);
    ::close(srv);
    ::unlink(cfg.ipc_path.c_str());

    // ── Persist ────────────────────────────────────────────────────────────────

    // Write the in-memory index to disk so query processes can load it later
    std::cout << "[indexer] Saving index → " << cfg.out_dir << "/index/\n";
    index.save(cfg.out_dir + "/index");
    std::cout << "[indexer] Done. indexed=" << docs_ok
              << "  skipped=" << docs_skip << "\n";
    return 0;
}
