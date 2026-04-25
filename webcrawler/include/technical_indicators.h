// technical_indicators.h — Compute SMA, EMA, RSI, MACD, Bollinger Bands, and
//                          volume-spike detection from price/volume series.
//
// All functions operate on std::vector<double> and return a vector of the same
// length as the input.  Values before the first full window are 0.0 (or 50.0
// for RSI, which is the neutral midpoint).
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace indicators {

// ── Simple Moving Average ──────────────────────────────────────────────────────

// Returns SMA over a rolling `period`-bar window.
// result[i] = 0.0 for i < period-1 (not enough history).
inline std::vector<double> sma(const std::vector<double>& prices, int period) {
    std::vector<double> result(prices.size(), 0.0);
    if (period <= 0 || static_cast<int>(prices.size()) < period) return result;

    // Seed the first window
    double window_sum = 0.0;
    for (int i = 0; i < period; ++i) window_sum += prices[i];
    result[period - 1] = window_sum / period;

    // Slide forward: add newest, drop oldest
    for (int i = period; i < static_cast<int>(prices.size()); ++i) {
        window_sum += prices[i] - prices[i - period];
        result[i]   = window_sum / period;
    }
    return result;
}

// ── Exponential Moving Average ────────────────────────────────────────────────

// Standard EMA: multiplier k = 2 / (period + 1).
// Seeded with the SMA of the first `period` prices.
inline std::vector<double> ema(const std::vector<double>& prices, int period) {
    std::vector<double> result(prices.size(), 0.0);
    if (period <= 0 || static_cast<int>(prices.size()) < period) return result;

    // Seed
    double seed = 0.0;
    for (int i = 0; i < period; ++i) seed += prices[i];
    seed /= period;
    result[period - 1] = seed;

    const double k = 2.0 / (period + 1.0);
    for (int i = period; i < static_cast<int>(prices.size()); ++i)
        result[i] = prices[i] * k + result[i - 1] * (1.0 - k);

    return result;
}

// ── RSI (Wilder smoothing) ─────────────────────────────────────────────────────

// Returns RSI values in [0, 100].
// result[i] = 50.0 (neutral) before the first full window.
inline std::vector<double> rsi(const std::vector<double>& prices, int period = 14) {
    const int n = static_cast<int>(prices.size());
    std::vector<double> result(n, 50.0);
    if (period <= 0 || n <= period) return result;

    // Build gain/loss arrays vs. previous close
    std::vector<double> gains(n, 0.0), losses(n, 0.0);
    for (int i = 1; i < n; ++i) {
        double diff = prices[i] - prices[i - 1];
        if (diff > 0) gains[i]  =  diff;
        else          losses[i] = -diff;
    }

    // Seed: simple average over first window (indices 1..period)
    double avg_gain = 0.0, avg_loss = 0.0;
    for (int i = 1; i <= period; ++i) {
        avg_gain  += gains[i];
        avg_loss  += losses[i];
    }
    avg_gain /= period;
    avg_loss /= period;

    auto to_rsi = [](double ag, double al) -> double {
        if (al == 0.0) return 100.0;
        double rs = ag / al;
        return 100.0 - 100.0 / (1.0 + rs);
    };
    result[period] = to_rsi(avg_gain, avg_loss);

    // Wilder smoothing for the rest
    for (int i = period + 1; i < n; ++i) {
        avg_gain = (avg_gain * (period - 1) + gains[i])  / period;
        avg_loss = (avg_loss * (period - 1) + losses[i]) / period;
        result[i] = to_rsi(avg_gain, avg_loss);
    }
    return result;
}

// ── MACD ──────────────────────────────────────────────────────────────────────

struct MacdResult {
    std::vector<double> macd;       // EMA(fast) − EMA(slow)
    std::vector<double> signal;     // EMA(signal_period) of macd line
    std::vector<double> histogram;  // macd − signal
};

// Standard parameters: fast=12, slow=26, signal=9.
inline MacdResult macd(const std::vector<double>& prices,
                       int fast = 12, int slow = 26, int signal_period = 9) {
    const int n = static_cast<int>(prices.size());
    MacdResult res;
    res.macd.resize(n, 0.0);
    res.signal.resize(n, 0.0);
    res.histogram.resize(n, 0.0);

    auto fast_ema = ema(prices, fast);
    auto slow_ema = ema(prices, slow);

    for (int i = 0; i < n; ++i)
        res.macd[i] = fast_ema[i] - slow_ema[i];

    res.signal = ema(res.macd, signal_period);

    for (int i = 0; i < n; ++i)
        res.histogram[i] = res.macd[i] - res.signal[i];

    return res;
}

// ── Bollinger Bands ────────────────────────────────────────────────────────────

struct BollingerBands {
    std::vector<double> upper;
    std::vector<double> middle;   // identical to SMA
    std::vector<double> lower;
};

// Standard: SMA(period) ± k × population std-dev over the same window.
inline BollingerBands bollinger(const std::vector<double>& prices,
                                int period = 20, double k = 2.0) {
    const int n = static_cast<int>(prices.size());
    BollingerBands bb;
    bb.upper.resize(n, 0.0);
    bb.middle = sma(prices, period);
    bb.lower.resize(n, 0.0);

    for (int i = period - 1; i < n; ++i) {
        double mean   = bb.middle[i];
        double sq_sum = 0.0;
        for (int j = i - period + 1; j <= i; ++j)
            sq_sum += (prices[j] - mean) * (prices[j] - mean);
        double stddev = std::sqrt(sq_sum / period);
        bb.upper[i]   = mean + k * stddev;
        bb.lower[i]   = mean - k * stddev;
    }
    return bb;
}

// ── Volume spike detection ─────────────────────────────────────────────────────

// Returns true when the most recent volume bar is >= `threshold` × the rolling
// average of the preceding `lookback` bars.
// Typical usage: lookback=20, threshold=2.0  (2× average = spike)
inline bool volume_spike(const std::vector<double>& volumes,
                         int lookback = 20, double threshold = 2.0) {
    const int n = static_cast<int>(volumes.size());
    if (n < lookback + 1) return false;

    double avg = 0.0;
    for (int i = n - lookback - 1; i < n - 1; ++i)
        avg += volumes[i];
    avg /= lookback;

    return avg > 0.0 && (volumes[n - 1] / avg) >= threshold;
}

} // namespace indicators
