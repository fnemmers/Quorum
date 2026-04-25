// url_utils.h — URL parsing, normalization, and resolution utilities.
#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace url_utils {

struct ParsedUrl {
    std::string scheme;   // "http" or "https"
    std::string host;     // lowercase host (may include port)
    std::string path;     // "/" prefixed path
    std::string query;    // "?..." or empty
};

inline ParsedUrl parse(const std::string& url) {
    ParsedUrl p;

    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return p;

    p.scheme = url.substr(0, scheme_end);
    std::transform(p.scheme.begin(), p.scheme.end(), p.scheme.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    size_t host_start = scheme_end + 3;

    // Find first of '/', '?', '#' after host
    size_t path_start  = url.find('/', host_start);
    size_t query_start = url.find('?', host_start);
    size_t frag_start  = url.find('#', host_start);

    // Host ends at the earliest separator
    size_t host_end = std::min({path_start, query_start, frag_start, url.size()});

    p.host = url.substr(host_start, host_end - host_start);
    std::transform(p.host.begin(), p.host.end(), p.host.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    if (path_start == std::string::npos) {
        p.path = "/";
        if (query_start != std::string::npos && query_start < frag_start)
            p.query = url.substr(query_start, frag_start - query_start);
    } else {
        size_t path_end = std::min(query_start, std::min(frag_start, url.size()));
        p.path = url.substr(path_start, path_end - path_start);
        if (p.path.empty()) p.path = "/";
        if (query_start != std::string::npos && query_start < frag_start)
            p.query = url.substr(query_start, frag_start - query_start);
    }

    return p;
}

inline bool is_valid(const std::string& url) {
    if (url.size() < 8) return false;
    std::string lo = url.substr(0, 8);
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return lo.substr(0,7) == "http://" || lo.substr(0,8) == "https://";
}

// Collapse dot-segments in a path (RFC 3986 §5.2.4).
// e.g. "/a/b/../../c" → "/c",  "/a/./b" → "/a/b"
inline std::string remove_dot_segments(const std::string& path) {
    std::vector<std::string> segments;
    size_t i = 0;
    while (i < path.size()) {
        size_t slash = path.find('/', i + 1);
        if (slash == std::string::npos) slash = path.size();
        std::string seg = path.substr(i, slash - i);  // includes leading '/'
        if (seg == "/." || seg == "/") {
            // stay in current directory (just ensure a trailing slash exists)
            if (segments.empty()) segments.push_back("/");
        } else if (seg == "/..") {
            if (!segments.empty()) segments.pop_back();
        } else {
            segments.push_back(seg);
        }
        i = slash;
    }
    if (segments.empty()) return "/";
    std::string result;
    for (const auto& s : segments) result += s;
    return result;
}

inline std::string normalize(const std::string& url) {
    auto p = parse(url);
    if (p.scheme.empty() || p.host.empty()) return "";
    if (p.scheme != "http" && p.scheme != "https") return "";
    return p.scheme + "://" + p.host + remove_dot_segments(p.path) + p.query;
}

inline std::string get_origin(const std::string& url) {
    auto p = parse(url);
    if (p.scheme.empty() || p.host.empty()) return "";
    return p.scheme + "://" + p.host;
}

// Resolve an href relative to a base URL.
// Returns empty string for non-HTTP(S) results or fragments.
inline std::string resolve(const std::string& base_url, const std::string& href) {
    if (href.empty() || href[0] == '#') return "";

    // Absolute URL
    {
        std::string lo = href.substr(0, std::min<size_t>(href.size(), 8));
        std::transform(lo.begin(), lo.end(), lo.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (lo.substr(0,7) == "http://" || lo.substr(0,8) == "https://")
            return normalize(href);
    }

    // Protocol-relative  //host/path
    if (href.size() >= 2 && href[0] == '/' && href[1] == '/') {
        auto p = parse(base_url);
        return normalize(p.scheme + ":" + href);
    }

    auto base = parse(base_url);
    if (base.scheme.empty() || base.host.empty()) return "";

    // Root-relative /path
    if (href[0] == '/')
        return normalize(base.scheme + "://" + base.host + href);

    // Query-only ?...
    if (href[0] == '?')
        return normalize(base.scheme + "://" + base.host + base.path + href);

    // Relative path – resolve against base directory
    std::string dir = base.path;
    size_t last_slash = dir.rfind('/');
    dir = (last_slash != std::string::npos) ? dir.substr(0, last_slash + 1) : "/";

    return normalize(base.scheme + "://" + base.host + dir + href);
}

} // namespace url_utils
