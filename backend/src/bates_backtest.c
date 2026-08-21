#include "bates_backtest.h"
#include "heston.h"
#include "heston_surface.h"
#include "news_jump.h"
#include "db.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif

/* ── Local BS helpers (delta, gamma, price for call & put) ────────── */

static double bs_ncdf(double x) {
    return 0.5 * erfc(-x * M_SQRT1_2);
}

static double bs_npdf(double x) {
    return (1.0 / sqrt(2.0 * M_PI)) * exp(-0.5 * x * x);
}

static double bs_d1(double S, double K, double T, double r, double v) {
    return (log(S / K) + (r + 0.5 * v * v) * T) / (v * sqrt(T));
}

static double bs_call_price_v(double S, double K, double T, double r, double v) {
    if (v <= 0.0 || T <= 0.0)  return fmax(S - K * exp(-r * T), 0.0);
    double d1 = bs_d1(S, K, T, r, v);
    double d2 = d1 - v * sqrt(T);
    return S * bs_ncdf(d1) - K * exp(-r * T) * bs_ncdf(d2);
}

static double bs_put_price_v(double S, double K, double T, double r, double v) {
    /* Put-call parity: P = C - S + K*exp(-rT) */
    return bs_call_price_v(S, K, T, r, v) - S + K * exp(-r * T);
}

static double bs_call_delta(double S, double K, double T, double r, double v) {
    if (v <= 0.0 || T <= 0.0)  return S >= K ? 1.0 : 0.0;
    return bs_ncdf(bs_d1(S, K, T, r, v));
}

static double bs_put_delta(double S, double K, double T, double r, double v) {
    return bs_call_delta(S, K, T, r, v) - 1.0;
}

static double bs_gamma(double S, double K, double T, double r, double v) {
    if (v <= 0.0 || T <= 0.0)  return 0.0;
    double d1 = bs_d1(S, K, T, r, v);
    return bs_npdf(d1) / (S * v * sqrt(T));
}

static double bs_vega(double S, double K, double T, double r, double v) {
    if (v <= 0.0 || T <= 0.0)  return 0.0;
    double d1 = bs_d1(S, K, T, r, v);
    return S * bs_npdf(d1) * sqrt(T);
}

/* ── Deterministic Gaussian draw from a 64-bit seed ────────────────── */

static uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static double u01(uint64_t x) {
    uint64_t u = mix64(x) >> 11;
    double d = (double)u * (1.0 / 9007199254740992.0);
    if (d <= 0.0) d = 1e-300;
    if (d >= 1.0) d = 1.0 - 1e-16;
    return d;
}

static double norm_from(uint64_t s) {
    double u1 = u01(s);
    double u2 = u01(s ^ 0x9E3779B97F4A7C15ULL);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ── Synthetic market-IV surface ───────────────────────────────────── */
/*
 * Skew term: negative slope in log(K/S) (put IV > call IV) with a
 * gentle smile curvature, decayed by sqrt(T) so shorter-dated smiles
 * are steeper — matches empirical US equity index skew.
 */
static double synth_skew(double moneyness, double T_years) {
    double m = log(moneyness);
    double base = -0.05 * m + 0.03 * m * m;
    double tau  = T_years > 0.02 ? T_years : 0.02;
    return base / sqrt(tau / 0.25);   /* normalized to 3-month benchmark */
}

static double synth_market_iv(double model_iv, double moneyness,
                              double T_years, double noise_sigma_vol,
                              uint64_t seed_kt) {
    double noise = noise_sigma_vol * norm_from(seed_kt);
    double skew  = synth_skew(moneyness, T_years);
    double iv    = model_iv + noise + skew;
    if (iv < 0.02) iv = 0.02;   /* keep it in a physically sane range   */
    if (iv > 3.00) iv = 3.00;
    return iv;
}

/* ── Delta-hedged P&L for one (option, path) ──────────────────────── */
/*
 * We're the seller of the option at t=0 for `premium`, then rebalance
 * a BS-delta hedge daily at frozen market_iv. Convention: short 1
 * option, hold `delta_t` shares of stock. Cash accumulates from
 * rebalances at the risk-free rate.
 *
 * Terminal P&L = +premium - option_payoff + terminal_cash + delta_T*S_T
 *                     - initial_delta_0 * S_0
 * Since we net the equity leg out through daily rebalancing, this
 * simplifies to:
 *   pnl = premium
 *       + Σ_t delta_t * (S_{t+1} - S_t)   (hedge cash flows)
 *       - option_payoff_T
 * plus interest on the cash carry (small, we ignore for the demo).
 */
static double delta_hedged_pnl(const double *path,   /* length steps+1 */
                               int steps,
                               double S0, double K,
                               double T, double r,
                               double market_iv,
                               double premium,
                               char   right) {
    (void)S0;   /* path[0] already carries the spot */
    double dt = T / (double)steps;
    double hedge_cash = 0.0;

    for (int i = 0; i < steps; i++) {
        double t_rem = T - (double)i * dt;
        double S = path[i];
        double delta;
        if (right == 'C')
            delta = bs_call_delta(S, K, t_rem, r, market_iv);
        else
            delta = bs_put_delta(S, K, t_rem, r, market_iv);
        double dS = path[i + 1] - S;
        hedge_cash += delta * dS;
    }

    double S_T = path[steps];
    double payoff = right == 'C'
        ? fmax(S_T - K, 0.0)
        : fmax(K - S_T, 0.0);

    /* Sign convention: we sold the option, so we KEEP the premium and
     * OWE the payoff. Delta on a call is positive → we long stock →
     * hedge_cash > 0 when stock rises, matching short-call loss. */
    return premium + hedge_cash - payoff;
}

/* ── Main backtest loop ───────────────────────────────────────────── */

int bates_backtest_run(const char *const *symbols, int n_symbols,
                       int64_t  session_id,
                       int      horizon_days,
                       double   moneyness_lo,
                       double   moneyness_hi,
                       int      n_strikes,
                       const int *expiries_days,
                       int      n_expiries,
                       int      n_paths,
                       double   noise_sigma_vol,
                       uint64_t seed,
                       double   r,
                       BatesBacktestRow **out_rows,
                       int      *out_n,
                       BatesBacktestSummary *out_summary) {
    if (!symbols || !expiries_days || !out_rows || !out_n || !out_summary)
        return -1;
    if (n_symbols <= 0 || n_strikes < 2 || n_expiries < 1) return -1;
    if (n_paths      < 16)  n_paths      = 256;
    if (horizon_days < 5)   horizon_days = 21;
    if (moneyness_lo <= 0)  moneyness_lo = 0.85;
    if (moneyness_hi <= moneyness_lo) moneyness_hi = 1.15;
    if (noise_sigma_vol < 0) noise_sigma_vol = 0.015;
    if (r < 0) r = 0.04;
    if (!seed) seed = 0xC0FFEE1234567890ULL;

    int64_t now      = (int64_t)time(NULL) * 1000LL;
    int64_t lookback = now - 180LL * 86400LL * 1000LL;

    /* Two rows per (symbol, K, T) — call and put. */
    int rows_per_symbol = n_strikes * n_expiries * 2;
    int max_rows        = n_symbols * rows_per_symbol;
    BatesBacktestRow *rows =
        (BatesBacktestRow *)calloc((size_t)max_rows, sizeof(BatesBacktestRow));
    if (!rows) return -1;

    double *log_lo_hi = malloc(sizeof(double) * (size_t)n_strikes);
    double *moneyness = malloc(sizeof(double) * (size_t)n_strikes);
    if (!log_lo_hi || !moneyness) { free(rows); free(log_lo_hi); free(moneyness); return -1; }

    double log_lo = log(moneyness_lo);
    double log_hi = log(moneyness_hi);
    for (int s = 0; s < n_strikes; s++) {
        double t = (double)s / (double)(n_strikes - 1);
        moneyness[s] = exp(log_lo + t * (log_hi - log_lo));
        log_lo_hi[s] = log(moneyness[s]);
    }

    /* Refresh the news-jump rollup once; each symbol's calibrate then
     * applies the freshest signal. */
    (void)news_jump_recompute(now, 48);

    /* Path buffer, allocated once and reused per option. */
    int steps      = horizon_days;
    int path_len   = steps + 1;
    double *path   = malloc(sizeof(double) * (size_t)path_len);
    double *pnls   = malloc(sizeof(double) * (size_t)n_paths);
    if (!path || !pnls) {
        free(rows); free(log_lo_hi); free(moneyness); free(path); free(pnls);
        return -1;
    }

    int n_out = 0;
    double sum_edge = 0.0, sumsq_edge = 0.0;
    double sum_hit  = 0.0;
    double sum_pnl  = 0.0;
    double sum_sharpe = 0.0;

    for (int i = 0; i < n_symbols; i++) {
        const char *symbol = symbols[i];
        if (!symbol || !*symbol) continue;

        OHLCBar bars[256];
        int n_bars = db_cache_load(symbol, "day", lookback, now, bars, 256);
        if (n_bars < 30) continue;

        double spot = bars[n_bars - 1].close;

        for (int e = 0; e < n_expiries; e++) {
            int    dte   = expiries_days[e];
            double T_yrs = (double)dte / 365.0;

            HestonParams hp;
            /* Calibrate at the option's own horizon so v0/theta reflect
             * annualized vol; T is set to the option DTE for pricing. */
            if (heston_calibrate_from_history(bars, n_bars,
                                              T_yrs, dte, &hp) != 0)
                continue;
            news_jump_apply(&hp, symbol, now);

            /* Path-sim horizon override — we hedge for min(dte, horizon). */
            int    hedge_days = dte < horizon_days ? dte : horizon_days;
            hp.T     = (double)hedge_days / 365.0;
            hp.steps = hedge_days;

            for (int s = 0; s < n_strikes; s++) {
                double K = spot * moneyness[s];

                /* 1. Model prices via Bates CF (T is option DTE). */
                double model_call = heston_call_price(&hp, spot, K, T_yrs, r);
                if (!isfinite(model_call) || model_call <= 0) continue;
                double model_call_iv = bs_implied_vol_call(model_call,
                                                           spot, K, T_yrs, r);
                if (!isfinite(model_call_iv)) continue;

                for (int rt = 0; rt < 2; rt++) {
                    char right = rt == 0 ? 'C' : 'P';

                    /* 2. Model IV — put shares the same vol via parity. */
                    double model_iv = model_call_iv;

                    /* 3. Synthetic market IV — deterministic per key. */
                    uint64_t seed_kt = seed
                        ^ ((uint64_t)session_id * 0x9E37ULL)
                        ^ ((uint64_t)i * 0xDEADBEEFULL)
                        ^ ((uint64_t)s * 0xCAFEBABEULL)
                        ^ ((uint64_t)e * 0xB055C0DEULL)
                        ^ ((uint64_t)right);
                    double market_iv = synth_market_iv(model_iv,
                                                       moneyness[s],
                                                       T_yrs,
                                                       noise_sigma_vol,
                                                       seed_kt);

                    /* 4. Premium & greeks at market IV. */
                    double premium = right == 'C'
                        ? bs_call_price_v(spot, K, T_yrs, r, market_iv)
                        : bs_put_price_v (spot, K, T_yrs, r, market_iv);
                    double delta = right == 'C'
                        ? bs_call_delta(spot, K, T_yrs, r, market_iv)
                        : bs_put_delta (spot, K, T_yrs, r, market_iv);
                    double gamma = bs_gamma(spot, K, T_yrs, r, market_iv);
                    double vega  = bs_vega (spot, K, T_yrs, r, market_iv);

                    /* 5. MC hedged-P&L bundle. */
                    double pnl_sum = 0.0, pnl_sumsq = 0.0;
                    int    good_paths = 0;
                    for (int p = 0; p < n_paths; p++) {
                        uint64_t pseed = seed_kt ^ ((uint64_t)p * 0xB5297A4DULL);
                        HestonParams hp_p = hp;
                        hp_p.s0 = spot;
                        if (heston_simulate_price_path(&hp_p, pseed, path) != 0)
                            continue;
                        double pnl = delta_hedged_pnl(path, hedge_days,
                                                     spot, K,
                                                     hp_p.T, r,
                                                     market_iv, premium, right);
                        pnls[good_paths++] = pnl;
                        pnl_sum   += pnl;
                        pnl_sumsq += pnl * pnl;
                    }
                    if (good_paths < 2) continue;

                    double mean_pnl = pnl_sum / (double)good_paths;
                    double var_pnl  = (pnl_sumsq / (double)good_paths) - mean_pnl * mean_pnl;
                    if (var_pnl < 0) var_pnl = 0;
                    double std_pnl  = sqrt(var_pnl);
                    double sharpe   = std_pnl > 1e-9
                        ? mean_pnl / std_pnl / sqrt((double)hedge_days)
                        : 0.0;

                    double edge = market_iv - model_iv;

                    BatesBacktestRow *out = &rows[n_out++];
                    strncpy(out->symbol, symbol, MAX_SYMBOL_LEN - 1);
                    out->strike               = K;
                    out->dte_days             = dte;
                    out->right                = right;
                    out->spot                 = spot;
                    out->model_iv             = model_iv;
                    out->market_iv            = market_iv;
                    out->edge_vol_pts         = edge;
                    out->premium              = premium;
                    out->delta0               = delta;
                    out->gamma0               = gamma;
                    out->vega0                = vega;
                    out->expected_hedged_pnl  = mean_pnl;
                    out->std_hedged_pnl       = std_pnl;
                    out->sharpe_daily         = sharpe;
                    out->n_paths              = good_paths;
                    out->signed_edge          = edge >= 0 ? +1 : -1;
                    /* dollar_edge: MM sizing. |delta| * premium * edge
                     * gives $ P&L per contract if the vol edge realises
                     * out via delta-hedged theta harvesting. */
                    out->dollar_edge          = fabs(delta) * premium * edge;

                    sum_edge   += edge;
                    sumsq_edge += edge * edge;
                    if (fabs(edge) > 0.01) sum_hit += 1.0;
                    sum_pnl    += mean_pnl;
                    sum_sharpe += sharpe;
                }
            }
        }
    }

    free(path);
    free(pnls);
    free(log_lo_hi);
    free(moneyness);

    out_summary->run_id            = session_id;
    out_summary->n_rows            = n_out;
    out_summary->n_symbols         = n_symbols;
    out_summary->n_strikes         = n_strikes;
    out_summary->n_expiries        = n_expiries;
    out_summary->n_paths           = n_paths;
    out_summary->horizon_days      = horizon_days;
    out_summary->noise_sigma_vol   = noise_sigma_vol;
    out_summary->r                 = r;
    if (n_out > 0) {
        double mean = sum_edge / (double)n_out;
        double var  = sumsq_edge / (double)n_out - mean * mean;
        if (var < 0) var = 0;
        out_summary->mean_edge_vol_pts       = mean;
        out_summary->std_edge_vol_pts        = sqrt(var);
        out_summary->hit_rate_edge_gt_1vol   = sum_hit  / (double)n_out;
        out_summary->avg_hedged_pnl          = sum_pnl  / (double)n_out;
        out_summary->avg_sharpe_daily        = sum_sharpe / (double)n_out;
    } else {
        out_summary->mean_edge_vol_pts       = 0.0;
        out_summary->std_edge_vol_pts        = 0.0;
        out_summary->hit_rate_edge_gt_1vol   = 0.0;
        out_summary->avg_hedged_pnl          = 0.0;
        out_summary->avg_sharpe_daily        = 0.0;
    }

    *out_rows = rows;
    *out_n    = n_out;
    return 0;
}
