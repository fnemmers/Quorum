// tokenizer.h — Lowercasing, alphanumeric-split tokenizer with optional
//               English stop-word filtering.
#pragma once
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <cctype>

namespace tokenizer {

inline const std::unordered_set<std::string>& stop_words() {
    static const std::unordered_set<std::string> sw = {
        "a","an","the","and","or","but","in","on","at","to","for","of","with",
        "by","from","up","about","into","through","during","is","are","was",
        "were","be","been","being","have","has","had","do","does","did","will",
        "would","could","should","may","might","shall","can","need","dare",
        "that","this","these","those","it","its","i","we","you","he","she",
        "they","me","us","him","her","them","my","our","your","his","their",
        "what","which","who","whom","when","where","why","how","all","each",
        "both","few","more","most","not","no","nor","so","yet","just","very",
        "as","if","s","t","re","ve","ll","d","m"
    };
    return sw;
}

// Tokenize text: lowercase, split on non-alphanumeric, filter short tokens.
// filter_stops: if true, removes common English stop words.
inline std::vector<std::string> tokenize(const std::string& text,
                                          bool filter_stops = false) {
    std::vector<std::string> tokens;
    tokens.reserve(text.size() / 6);
    std::string tok;
    tok.reserve(32);

    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            tok += static_cast<char>(std::tolower(c));
        } else {
            if (tok.size() >= 2) {
                if (!filter_stops ||
                    stop_words().find(tok) == stop_words().end())
                    tokens.push_back(tok);
            }
            tok.clear();
        }
    }
    if (tok.size() >= 2) {
        if (!filter_stops ||
            stop_words().find(tok) == stop_words().end())
            tokens.push_back(tok);
    }

    return tokens;
}

} // namespace tokenizer
