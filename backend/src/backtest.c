#include "backtest.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdint.h>

#ifndef _WIN32
#define BACKTEST_TIMEGM timegm
#else
#define BACKTEST_TIMEGM _mkgmtime
#endif
#define DAY_MS 86400000LL
#define LOOKAHEAD_MS (7LL * DAY_MS)
#define TX_COST_PCT 0.2   /* 0.2 percentage points round-trip */

/*
 * Sample standard deviation.
 *
 * Used for Sharpe ratio:
 *   Sharpe = mean(daily_returns) / stddev(daily_returns) * sqrt(252)
 */
static double stddev(const double *xs, int n) {
    if (n < 2) {
        return 0.0;
    }

    double mean = 0.0;
    for (int i = 0; i < n; i++) {
        mean += xs[i];
    }
    mean /= (double)n;

    double sq = 0.0;
    for (int i = 0; i < n; i++) {
        double d = xs[i] - mean;
        sq += d * d;
    }

    return sqrt(sq / (double)(n - 1));
}

/*
 * Return the close price on or after target_ms.
 *
 * We query a 7-day window so weekends/holidays are handled naturally.
 * If the target date is not a trading day, we use the next available bar.
 */
static int price_on_or_after(const char *symbol, int64_t target_ms, double *out_price) {
    OHLCBar bars[8];
    int n;

    if (symbol == NULL || out_price == NULL) {
        return -1;
    }

    n = db_cache_load(symbol, "day",
                      target_ms,
                      target_ms + LOOKAHEAD_MS,
                      bars,
                      8);

    if (n <= 0) {
        return -1;
    }

    *out_price = bars[0].close;
    return 0;
}

/*
 * Load the full daily bar series for one ticker over the holding window.
 */
static int load_series(const char *symbol,
                       int64_t entry_ms,
                       int64_t exit_ms,
                       OHLCBar *bars,
                       int max_bars) {
    if (symbol == NULL || bars == NULL || max_bars <= 0) {
        return -1;
    }

    return db_cache_load(symbol, "day",
                         entry_ms,
                         exit_ms + LOOKAHEAD_MS,
                         bars,
                         max_bars);
}

/*
 * Parse "YYYY-MM-DD" into Unix milliseconds at UTC midnight.
 *
 * Basic validation only:
 *   - year in [1970, 2100]
 *   - month in [1, 12]
 *   - day in [1, 31]
 *
 * Returns -1 on invalid input.
 */
int64_t backtest_parse_date_ms(const char *yyyy_mm_dd) {
    int year, month, day;
    struct tm tmv;
    time_t t;

    if (yyyy_mm_dd == NULL) {
        return -1;
    }

    if (sscanf(yyyy_mm_dd, "%d-%d-%d", &year, &month, &day) != 3) {
        return -1;
    }

    if (year < 1970 || year > 2100) {
        return -1;
    }
    if (month < 1 || month > 12) {
        return -1;
    }
    if (day < 1 || day > 31) {
        return -1;
    }

    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = year - 1900;
    tmv.tm_mon  = month - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = 0;
    tmv.tm_min  = 0;
    tmv.tm_sec  = 0;
    tmv.tm_isdst = 0;

    t = BACKTEST_TIMEGM(&tmv);
    if (t < 0) {
        return -1;
    }

    return (int64_t)t * 1000LL;
}

/*
 * Format Unix milliseconds back into "YYYY-MM-DD".
 *
 * out must point to at least 11 writable bytes.
 */
int backtest_format_date(int64_t ms, char *out) {
    time_t secs;
    struct tm tmv;

    if (out == NULL) {
        return -1;
    }

    secs = (time_t)(ms / 1000LL);

#if defined(_WIN32)
    if (gmtime_s(&tmv, &secs) != 0) {
        return -1;
    }
#else
    if (gmtime_r(&secs, &tmv) == NULL) {
        return -1;
    }
#endif

    if (snprintf(out, 11, "%04d-%02d-%02d",
                 tmv.tm_year + 1900,
                 tmv.tm_mon + 1,
                 tmv.tm_mday) != 10) {
        return -1;
    }

    return 0;
}

int backtest_run(const char *const *tickers,
                 int n_tickers,
                 const char *start_date,
                 int hold_days,
                 BacktestResult *out) {
    int64_t entry_ms, exit_ms;
    double *entry_prices = NULL;
    double *exit_prices = NULL;
    double sum_ret = 0.0;
    int n_used = 0;
    int n_skipped = 0;
    int n_positive = 0;

    if (out) {
        memset(out, 0, sizeof(*out));
    }

    if (tickers == NULL || out == NULL) {
        return -1;
    }

    if (n_tickers <= 0 || hold_days <= 0) {
        return -1;
    }

    entry_ms = backtest_parse_date_ms(start_date);
    if (entry_ms < 0) {
        return -1;
    }

    exit_ms = entry_ms + (int64_t)hold_days * DAY_MS;

    entry_prices = malloc((size_t)n_tickers * sizeof(double));
    exit_prices  = malloc((size_t)n_tickers * sizeof(double));
    if (entry_prices == NULL || exit_prices == NULL) {
        free(entry_prices);
        free(exit_prices);
        return -1;
    }

    /*
     * Echo input metadata into the result.
     */
    out->n_tickers_requested = n_tickers;
    out->n_tickers_used = 0;
    out->n_skipped = 0;
    out->hold_days = hold_days;
    out->transaction_cost_pct = TX_COST_PCT;

    if (backtest_format_date(entry_ms, out->start_date) != 0) {
        free(entry_prices);
        free(exit_prices);
        return -1;
    }

    if (backtest_format_date(exit_ms, out->end_date) != 0) {
        free(entry_prices);
        free(exit_prices);
        return -1;
    }

    /*
     * Main loop:
     * try to get entry and exit prices for each ticker.
     *
     * If either side is missing, skip the ticker entirely.
     */
    for (int i = 0; i < n_tickers; i++) {
        double entry_p, exit_p;

        if (tickers[i] == NULL || tickers[i][0] == '\0') {
            n_skipped++;
            continue;
        }

        if (price_on_or_after(tickers[i], entry_ms, &entry_p) < 0) {
            n_skipped++;
            continue;
        }

        if (price_on_or_after(tickers[i], exit_ms, &exit_p) < 0) {
            n_skipped++;
            continue;
        }

        if (entry_p <= 0.0) {
            n_skipped++;
            continue;
        }

        entry_prices[n_used] = entry_p;
        exit_prices[n_used] = exit_p;
        n_used++;
    }

    /*
     * No valid tickers means no usable backtest.
     */
    if (n_used == 0) {
        free(entry_prices);
        free(exit_prices);
        return -1;
    }

    /*
     * Equal-weight portfolio return:
     * average of the per-ticker returns.
     *
     * Per ticker:
     *   r = (exit - entry) / entry
     */
    for (int i = 0; i < n_used; i++) {
        double r = (exit_prices[i] - entry_prices[i]) / entry_prices[i];
        sum_ret += r;

        if (r > 0.0) {
            n_positive++;
        }
    }

    {
        double portfolio_return_decimal = sum_ret / (double)n_used;
        double portfolio_return_pct = portfolio_return_decimal * 100.0;

        /*
         * Subtract fixed transaction cost after converting to percentage.
         */
        portfolio_return_pct -= TX_COST_PCT;

        out->portfolio_return_pct = portfolio_return_pct;
        out->hit_rate_pct = ((double)n_positive / (double)n_used) * 100.0;
    }

    out->n_tickers_used = n_used;
    out->n_skipped = n_skipped;

    /*
     * Benchmark against SPY.
     *
     * If SPY data is unavailable, keep benchmark and alpha at 0.
     */
    {
        double spy_entry, spy_exit;

        if (price_on_or_after("SPY", entry_ms, &spy_entry) == 0 &&
            price_on_or_after("SPY", exit_ms, &spy_exit) == 0 &&
            spy_entry > 0.0) {
            out->benchmark_return_pct = ((spy_exit - spy_entry) / spy_entry) * 100.0;
            out->alpha_pct = out->portfolio_return_pct - out->benchmark_return_pct;
        } else {
            fprintf(stderr, "warning: benchmark lookup failed for SPY\n");
            out->benchmark_return_pct = 0.0;
            out->alpha_pct = 0.0;
        }
    }

    /*
     * Daily-series logic for Sharpe ratio and max drawdown.
     *
     * We use daily close sequences to build a normalized portfolio value path.
     * Since OHLCBar has both .timestamp and .close, this code is fully valid
     * with your market_data.h definition.
     */
    {
        int max_bars = hold_days + 10;
        OHLCBar *all_bars = NULL;
        int *bar_counts = NULL;
        double *port_vals = NULL;
        double *daily_rets = NULL;
        int daily_count = 0;

        all_bars = malloc((size_t)n_tickers * (size_t)max_bars * sizeof(OHLCBar));
        bar_counts = calloc((size_t)n_tickers, sizeof(int));
        port_vals = malloc((size_t)max_bars * sizeof(double));
        daily_rets = malloc((size_t)max_bars * sizeof(double));

        out->max_drawdown_pct = 0.0;
        out->sharpe_ratio = 0.0;

        if (all_bars != NULL && bar_counts != NULL &&
            port_vals != NULL && daily_rets != NULL) {

            /*
             * Load daily series for each ticker.
             */
            for (int i = 0; i < n_tickers; i++) {
                if (tickers[i] == NULL || tickers[i][0] == '\0') {
                    bar_counts[i] = 0;
                    continue;
                }

                bar_counts[i] = load_series(tickers[i],
                                            entry_ms,
                                            exit_ms,
                                            &all_bars[i * max_bars],
                                            max_bars);

                if (bar_counts[i] < 0) {
                    bar_counts[i] = 0;
                }
            }

            /*
             * Find the shortest non-empty series length.
             *
             * This is a simple way to keep all tickers aligned by index
             * without dealing with per-day timestamp matching for v1.
             */
            {
                int common_days = 0;

                for (int i = 0; i < n_tickers; i++) {
                    if (bar_counts[i] <= 0) {
                        continue;
                    }

                    if (common_days == 0 || bar_counts[i] < common_days) {
                        common_days = bar_counts[i];
                    }
                }

                /*
                 * Build normalized equal-weight portfolio values:
                 *   value[d] = average of (close[d] / close[0]) across names
                 */
                if (common_days >= 2) {
                    for (int d = 0; d < common_days; d++) {
                        double sum_val = 0.0;
                        int contributors = 0;

                        for (int i = 0; i < n_tickers; i++) {
                            OHLCBar *bars = &all_bars[i * max_bars];

                            if (bar_counts[i] < common_days) {
                                continue;
                            }
                            if (bars[0].close <= 0.0) {
                                continue;
                            }

                            sum_val += bars[d].close / bars[0].close;
                            contributors++;
                        }

                        if (contributors > 0) {
                            port_vals[daily_count++] = sum_val / (double)contributors;
                        }
                    }
                }
            }

            /*
             * From the portfolio value path:
             *   - compute daily returns
             *   - track peak and drawdown
             *   - compute annualized Sharpe
             */
            if (daily_count >= 2) {
                double peak = port_vals[0];
                double max_dd = 0.0;
                int ret_n = 0;
                double mean = 0.0;

                for (int d = 1; d < daily_count; d++) {
                    double prev = port_vals[d - 1];
                    double cur = port_vals[d];

                    if (prev > 0.0) {
                        double r = (cur - prev) / prev;
                        daily_rets[ret_n++] = r;
                        mean += r;
                    }

                    if (cur > peak) {
                        peak = cur;
                    }

                    if (peak > 0.0) {
                        double dd = (cur - peak) / peak; /* negative or zero */
                        if (dd < max_dd) {
                            max_dd = dd;
                        }
                    }
                }

                out->max_drawdown_pct = max_dd * 100.0;

                if (ret_n >= 2) {
                    double sd;

                    mean /= (double)ret_n;
                    sd = stddev(daily_rets, ret_n);

                    if (sd > 0.0) {
                        out->sharpe_ratio = (mean / sd) * sqrt(252.0);
                    }
                }
            }
        }

        free(all_bars);
        free(bar_counts);
        free(port_vals);
        free(daily_rets);
    }

    free(entry_prices);
    free(exit_prices);
    return 0;
}