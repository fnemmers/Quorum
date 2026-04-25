// sentiment.h — Lightweight keyword-based sentiment scorer for financial headlines.
//
// Assigns a score in [-1.0, 1.0] from a curated bullish/bearish keyword table.
// No external dependencies — suitable for real-time scoring inside the crawler.
//
// Not a substitute for an ML model, but it catches the highest-alpha keywords
// ("earnings beat", "bankruptcy", "SEC charges") with no latency cost.
#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

namespace sentiment {

// Lower-case copy of s
inline std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return out;
}

// Case-insensitive substring check
inline bool contains(const std::string& text, const std::string& phrase) {
    return text.find(phrase) != std::string::npos;
}

// Score a single headline.
// Each matching keyword adds its weight (negative weights for bearish phrases).
// Final score is clamped to [-1.0, 1.0].
inline double score_headline(const std::string& raw) {
    const std::string h = to_lower(raw);

    struct KW { const char* phrase; double weight; };

    // ── Bullish keywords ───────────────────────────────────────────────────────
    static const KW bullish[] = {
        {"earnings beat",        0.50}, {"beat expectations",   0.50},
        {"beat estimates",       0.45}, {"above expectations",  0.40},
        {"record revenue",       0.45}, {"record earnings",     0.45},
        {"revenue growth",       0.35}, {"profit surge",        0.40},
        {"raised guidance",      0.40}, {"positive outlook",    0.30},
        {"upgrade",              0.35}, {"strong buy",          0.40},
        {"buy rating",           0.35}, {"price target raised", 0.30},
        {"dividend increase",    0.30}, {"share buyback",       0.25},
        {"all-time high",        0.30}, {"breakout",            0.25},
        {"fda approval",         0.50}, {"approved",            0.30},
        {"partnership",          0.20}, {"acquisition",         0.15},
        {"contract",             0.15}, {"deal",                0.15},
        {"outperform",           0.35}, {"overweight",          0.30},
    };

    // ── Bearish keywords ───────────────────────────────────────────────────────
    static const KW bearish[] = {
        {"earnings miss",        -0.50}, {"missed expectations",  -0.50},
        {"miss estimates",       -0.45}, {"below expectations",   -0.40},
        {"revenue decline",      -0.40}, {"profit warning",       -0.40},
        {"lowered guidance",     -0.40}, {"guidance cut",         -0.40},
        {"downgrade",            -0.35}, {"sell rating",          -0.35},
        {"price target cut",     -0.30}, {"underperform",         -0.35},
        {"underweight",          -0.30}, {"lawsuit",              -0.40},
        {"investigation",        -0.35}, {"fraud",                -0.50},
        {"recall",               -0.30}, {"bankruptcy",           -0.60},
        {"layoffs",              -0.25}, {"restructuring",        -0.20},
        {"ceo resigned",         -0.30}, {"ceo fired",            -0.35},
        {"sec charges",          -0.50}, {"regulatory fine",      -0.35},
        {"data breach",          -0.35}, {"cyberattack",          -0.30},
        {"supply chain issues",  -0.25}, {"margin compression",   -0.30},
        {"debt downgrade",       -0.40}, {"credit downgrade",     -0.40},
    };

    double score = 0.0;

    for (const auto& kw : bullish)
        if (contains(h, kw.phrase)) score += kw.weight;

    for (const auto& kw : bearish)
        if (contains(h, kw.phrase)) score += kw.weight;   // weights are negative

    // Clamp to [-1.0, 1.0]
    if (score >  1.0) score =  1.0;
    if (score < -1.0) score = -1.0;
    return score;
}

// Average sentiment across a collection of headlines (0.0 for empty list)
inline double aggregate(const std::vector<std::string>& headlines) {
    if (headlines.empty()) return 0.0;
    double total = 0.0;
    for (const auto& h : headlines) total += score_headline(h);
    return total / static_cast<double>(headlines.size());
}

} // namespace sentiment
