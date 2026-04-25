// html_parser.h — Link extraction and text stripping for HTML documents.
// Uses a state-machine approach to avoid problematic multiline regex.
#pragma once
#include <string>
#include <vector>
#include <regex>
#include <algorithm>
#include <cctype>
#include <cstring>
#include "url_utils.h"

namespace html_parser {

// Remove all content inside <tag>...</tag> blocks (case-insensitive).
inline std::string remove_tag_blocks(const std::string& html,
                                     const std::string& tag_name) {
    // Build a lowercase copy for searching; use original for output.
    std::string lo = html;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    std::string open  = "<" + tag_name;          // e.g. "<script"
    std::string close = "</" + tag_name + ">";   // e.g. "</script>"

    std::string result;
    result.reserve(html.size());

    size_t search_from = 0;
    while (true) {
        size_t open_pos = lo.find(open, search_from);
        // Verify the character after the tag name is a tag boundary, not a
        // longer tag name (e.g. reject "<scriptures>" when searching for "<script")
        while (open_pos != std::string::npos) {
            size_t after = open_pos + open.size();
            if (after >= lo.size()) { open_pos = std::string::npos; break; }
            char ch = lo[after];
            if (ch == '>' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '/' || ch == '\r')
                break;  // confirmed tag boundary
            open_pos = lo.find(open, after);
        }
        if (open_pos == std::string::npos) {
            result.append(html, search_from, std::string::npos);
            break;
        }
        result.append(html, search_from, open_pos - search_from);

        size_t gt = lo.find('>', open_pos);
        if (gt == std::string::npos) { result += ' '; break; }

        size_t close_pos = lo.find(close, gt + 1);
        if (close_pos == std::string::npos) { result += ' '; break; }

        result += ' ';
        search_from = close_pos + close.size();
    }
    return result;
}

// Strip all HTML tags using a simple state machine.
inline std::string strip_tags(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    for (unsigned char c : html) {
        if      (c == '<') { in_tag = true;  out += ' '; }
        else if (c == '>') { in_tag = false; }
        else if (!in_tag)  { out += static_cast<char>(c); }
    }
    return out;
}

// Decode common HTML entities in-place.
inline void decode_entities(std::string& s) {
    static const struct { const char* enc; const char* dec; } table[] = {
        {"&amp;", "&"}, {"&lt;", "<"},  {"&gt;", ">"},
        {"&nbsp;"," "}, {"&#39;","'"}, {"&quot;","\""},
        {nullptr, nullptr}
    };
    for (int i = 0; table[i].enc; ++i) {
        const char* enc = table[i].enc;
        const char* dec = table[i].dec;
        size_t enc_len = std::strlen(enc);
        size_t dec_len = std::strlen(dec);
        size_t pos = 0;
        while ((pos = s.find(enc, pos)) != std::string::npos) {
            s.replace(pos, enc_len, dec);
            pos += dec_len;
        }
    }
}

// Extract all href links from an HTML document and resolve them against base_url.
// Returns a deduplicated, sorted list of absolute HTTP(S) URLs.
inline std::vector<std::string> extract_links(const std::string& html,
                                               const std::string& base_url) {
    static const size_t MAX_LINKS = 100;   // cap per page to bound frontier growth
    std::vector<std::string> links;
    links.reserve(64);

    // Match both double-quoted and single-quoted href values.
    // Use escaped strings (not raw literals) to avoid delimiter collision with '"'.
    static const std::regex re_dq("href\\s*=\\s*\"([^\"]+)\"", std::regex::icase);
    static const std::regex re_sq("href\\s*=\\s*'([^']+)'",    std::regex::icase);

    auto process = [&](const std::regex& re) {
        auto it  = std::sregex_iterator(html.begin(), html.end(), re);
        auto end = std::sregex_iterator{};
        for (; it != end && links.size() < MAX_LINKS; ++it) {
            std::string href = (*it)[1].str();
            if (href.empty() || href[0] == '#') continue;
            // Skip non-navigable schemes
            auto lo = href.substr(0, std::min<size_t>(href.size(), 12));
            std::transform(lo.begin(), lo.end(), lo.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            if (lo.find("javascript") == 0 ||
                lo.find("mailto:")    == 0 ||
                lo.find("tel:")       == 0 ||
                lo.find("data:")      == 0) continue;

            std::string resolved = url_utils::resolve(base_url, href);
            if (!resolved.empty()) links.push_back(std::move(resolved));
        }
    };

    process(re_dq);
    process(re_sq);

    std::sort(links.begin(), links.end());
    links.erase(std::unique(links.begin(), links.end()), links.end());
    return links;
}

// Extract visible text content from an HTML document.
inline std::string extract_text(const std::string& html) {
    std::string result = remove_tag_blocks(html, "script");
    result = remove_tag_blocks(result, "style");
    result = remove_tag_blocks(result, "noscript");
    result = strip_tags(result);
    decode_entities(result);
    return result;
}

// Extract visible text from all h1 / h2 / h3 tags in `html`.
// Returns one trimmed string per tag; skips anything shorter than 20 chars.
inline std::vector<std::string> extract_headlines(const std::string& html) {
    std::vector<std::string> out;

    // Build a lowercase shadow for searching; index offsets match the original
    std::string lo = html;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    for (const char* tag : {"h1", "h2", "h3"}) {
        const std::string open_prefix = std::string("<") + tag;
        const std::string close_tag   = std::string("</") + tag + ">";
        size_t pos = 0;
        while (pos < lo.size()) {
            auto found = lo.find(open_prefix, pos);
            if (found == std::string::npos) break;

            // Confirm tag boundary (not e.g. <h3x>)
            size_t after = found + open_prefix.size();
            if (after < lo.size()) {
                char ch = lo[after];
                if (ch != '>' && ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                    pos = after; continue;
                }
            }

            auto gt = lo.find('>', found);
            if (gt == std::string::npos) break;

            auto end = lo.find(close_tag, gt + 1);
            if (end == std::string::npos) { pos = gt + 1; continue; }

            // Extract from original (preserves original casing)
            std::string content = html.substr(gt + 1, end - gt - 1);
            content = strip_tags(content);
            decode_entities(content);

            // Trim leading/trailing whitespace
            auto s = content.find_first_not_of(" \t\n\r");
            if (s != std::string::npos) {
                auto e = content.find_last_not_of(" \t\n\r");
                content = content.substr(s, e - s + 1);
            } else {
                content.clear();
            }

            if (content.size() >= 20) out.push_back(std::move(content));
            pos = end + close_tag.size();
        }
    }
    return out;
}

// Extract the text of every <a> tag whose class attribute contains `cls_substr`.
// Used to target e.g. Finviz's class="tab-link-news" links.
inline std::vector<std::string> extract_anchor_text_by_class(
        const std::string& html, const std::string& cls_substr) {
    std::vector<std::string> out;

    std::string lo = html;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    const std::string cls_lo = [&]{
        std::string s = cls_substr;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return s;
    }();

    size_t pos = 0;
    while (pos < lo.size()) {
        auto a_open = lo.find("<a ", pos);
        if (a_open == std::string::npos) break;

        auto gt = lo.find('>', a_open);
        if (gt == std::string::npos) break;

        // Only process <a> tags whose opening tag contains the class fragment
        std::string tag_src = lo.substr(a_open, gt - a_open + 1);
        if (tag_src.find(cls_lo) != std::string::npos) {
            auto a_close = lo.find("</a>", gt);
            if (a_close != std::string::npos) {
                std::string content = html.substr(gt + 1, a_close - gt - 1);
                content = strip_tags(content);
                decode_entities(content);
                auto s = content.find_first_not_of(" \t\n\r");
                if (s != std::string::npos) {
                    auto e = content.find_last_not_of(" \t\n\r");
                    content = content.substr(s, e - s + 1);
                } else { content.clear(); }
                if (content.size() >= 20) out.push_back(std::move(content));
                pos = a_close + 4;
                continue;
            }
        }
        pos = gt + 1;
    }
    return out;
}

} // namespace html_parser
