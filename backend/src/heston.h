#ifndef HESTON_H
#define HESTON_H

#include "market_data.h"   /* MAX_SYMBOL_LEN, OHLCBar */
#include <stdint.h>

/*
 * heston  -  Heston stochastic-volatility Monte Carlo per stock,
 *            with an optional Merton-style jump-diffusion overlay
 *            (Bates 1996) driven by (lam, mu_j, sigma_j). Set lam=0
 *            to recover pure Heston.
 *
 * Model (Heston 1993 + Bates jumps), under a chosen measure:
 *     dS_t = (mu - lam*k) * S_t dt + sqrt(v_t) * S_t dW1 + (J-1) S_t dN
 *     dv_t = kappa (theta - v_t) dt + sigma_v * sqrt(v_t) dW2
 *     <dW1, dW2> = rho dt
 *     N_t ~ Poisson(lam),  ln J ~ Normal(mu_j, sigma_j^2)
 *     k    = E[J-1] = exp(mu_j + 0.5*sigma_j^2) - 1     (compensator)
 *
 * Discretization: log-Euler on S with per-step Poisson jump draw,
 *                 full-truncation on v.
 *     v_{t+dt} = v_t + kappa (theta - max(v_t,0)) dt
 *                  + sigma_v sqrt(max(v_t,0)) sqrt(dt) Z2
 *     ln S_{t+dt} = ln S_t + (mu - lam*k - 0.5 * max(v_t,0)) dt
 *                  + sqrt(max(v_t,0)) sqrt(dt) Z1
 *                  + sum_{i=1..N_step} (mu_j + sigma_j * Zi)
 *     Z2 = rho Z1 + sqrt(1 - rho^2) eps
 *     N_step ~ Poisson(lam * dt)
 *
 * Output is a single forward-looking risk profile per stock that the
 * blended ranking layer consumes. Per stock the function runs paths in
 * batches and stops once the 95% expected-shortfall estimate is stable
 * to within `es_tol_rel` (relative). That's where Monte Carlo
 * convergence belongs - estimates that get tighter with more samples.
 */

typedef struct {
    double s0;          /* current spot                              */
    double mu;          /* annualized drift (realized or risk-free)  */
    double v0;          /* initial variance (annualized)             */
    double kappa;       /* mean-reversion speed                       */
    double theta;       /* long-run variance                          */
    double sigma_v;     /* vol-of-vol                                 */
    double rho;         /* correlation of price/vol Brownians         */
    double T;           /* horizon in years (e.g. 21/252 for 3w)     */
    int    steps;       /* discretization steps over the horizon     */
    /* Bates jump component. lam == 0 collapses back to pure Heston. */
    double lam;         /* Poisson intensity, jumps per year         */
    double mu_j;        /* mean of ln(1+J), the log-jump             */
    double sigma_j;     /* stdev of ln(1+J)                          */
} HestonParams;

typedef struct {
    char   symbol[MAX_SYMBOL_LEN];
    double expected_return;   /* mean of (S_T - S_0)/S_0                */
    double forward_vol;       /* stdev of returns, annualized           */
    double es_95;             /* expected shortfall at 95% (negative)   */
    double prob_loss_5;       /* P(return < -5%)                        */
    int    n_paths_used;
    int    converged;         /* 1 if ES_95 stabilized before max_paths */
    /* Bates diagnostics. Both zero when lam == 0. */
    int    n_jumps_total;     /* total Poisson jumps summed over paths  */
    double jump_contribution; /* n_jumps_total / n_paths_used            */
} HestonScore;

/*
 * Run the Heston MC for one stock and fill `out`.
 *
 *   paths_per_batch  - samples per convergence check (e.g. 5000)
 *   max_paths        - hard cap (e.g. 200000)
 *   es_tol_rel       - convergence tolerance on |dES/ES| (e.g. 0.005)
 *
 * Returns 0 on success, -1 on bad inputs / allocation failure.
 *
 * Thread-safety: each call owns its own RNG state, so multiple stocks
 * can be scored in parallel from different threads.
 */
int heston_run(const HestonParams *p,
               HestonScore *out,
               int paths_per_batch,
               int max_paths,
               double es_tol_rel,
               uint64_t rng_seed);

/*
 * Helper - rough parameter calibration from a price-cache bar history.
 *
 * Computes annualized realized variance from log returns and uses it
 * as v0 and theta. kappa, sigma_v, rho default to literature values
 * for US equities (kappa=3.0, sigma_v=0.4, rho=-0.7). Caller can
 * override afterwards. mu = annualized mean log return.
 *
 * `bars` should be DAILY closes ordered ascending. Returns 0 on
 * success, -1 if too few bars (< 30).
 *
 * This is a placeholder calibration - it's good enough for a forward
 * variance estimate and for getting the ranking signal off the ground.
 * A real options-market calibration would fit the params to the
 * volatility surface, which is a separate project.
 */
int heston_calibrate_from_history(const OHLCBar *bars, int n_bars,
                                  double horizon_years, int steps,
                                  HestonParams *out);

/*
 * Risk-adjusted scalar collapsing a HestonScore to one sortable float.
 *     score = expected_return
 *             - lambda1 * fabs(es_95)
 *             - lambda2 * prob_loss_5
 *
 * The blended-ranking layer z-scores this across the universe and
 * combines it with the bot consensus z-score.
 */
double heston_risk_adjusted_score(const HestonScore *s,
                                  double lambda1, double lambda2);

/* ── Path-bundle visualisation ──────────────────────────────────
 *
 * For the front-end MC viewer: run N_paths of the model, accumulate a
 * (price_bucket x time_step) density grid and per-step quantile lines.
 * The frontend renders the density as a heatmap and overlays the
 * quantile bands. The lower edge of the density (the 5th-percentile
 * band) is the visible ES tail.
 *
 * Caller owns the buffers in HestonPathBundle and must call
 * heston_path_bundle_free() when done.
 */

typedef struct {
    int     n_steps;          /* time steps in the horizon (== params.steps) */
    int     n_buckets;        /* price buckets in the density grid           */
    double  price_min;
    double  price_max;
    double  s0;
    double  T;                /* horizon in years (echo of params.T)         */

    /* density[bucket * (n_steps + 1) + step] = count of paths whose
     * price at `step` falls in `bucket`. Row-major by bucket. */
    int    *density;

    /* Per-step quantile of price. Length n_steps + 1. */
    double *p05;
    double *p50;
    double *p95;

    /* Subsampled sample paths for the spaghetti overlay.
     * Row-major: sample_paths[i * (n_steps + 1) + step] = price on
     * sample i at time step. n_sample_paths ≤ min(n_paths, 200).
     * Also emit the terminal-quantile classification so the frontend
     * can colour the path (0 = tail below p05, 1 = middle, 2 = above
     * p95). */
    double *sample_paths;
    int    *sample_class;      /* length n_sample_paths */
    int     n_sample_paths;

    /* Distribution-level summary for the dashboard. */
    double  es_95;            /* expected shortfall of terminal returns      */
    double  expected_return;
    int     n_paths_used;
} HestonPathBundle;

/*
 * Run `n_paths` paths and populate `out`.
 *   n_buckets       - density grid resolution, e.g. 100.
 *   price_span_sigmas - how many sqrt(v0*T) std-devs above/below s0 to
 *                       span on each side (3.5 is a sensible default).
 * Returns 0 on success, -1 on bad inputs / allocation failure.
 * `out` must be zeroed before the call.
 */
int heston_path_bundle(const HestonParams *p, int n_paths,
                       int n_buckets, double price_span_sigmas,
                       HestonPathBundle *out, uint64_t rng_seed);

/* Frees the heap members of *b (density, p05/p50/p95) and zeros them. */
void heston_path_bundle_free(HestonPathBundle *b);

/*
 * Simulate one Bates/Heston price path from t=0 to t=T.
 * `path_out` must hold (p->steps + 1) doubles; path_out[0] = spot,
 * path_out[i] = price at step i * (T / steps).
 * Returns 0 on success, -1 on bad inputs.
 *
 * Uses the same Log-Euler + full-truncation + Poisson-jump scheme as
 * heston_run(). Exposes what was previously the file-static
 * simulate_path_record() so the Bates backtest can rehedge along a
 * bundle of forward paths.
 */
int heston_simulate_price_path(const HestonParams *p,
                               uint64_t seed,
                               double *path_out);

/* ── Layer-1 calibration diagnostics ──────────────────────────── */
/*
 * Sanity-check whether the calibrated Heston params are reasonable.
 * These are not a proper calibration — they answer "is the vibes-
 * calibrated model wildly wrong for this ticker?". Three checks:
 *
 *   1. Feller condition: 2*kappa*theta >= sigma_v^2
 *      If violated, variance can hit zero often; MC paths get
 *      pathological behavior near v=0.
 *
 *   2. Moment match: simulate K paths over 1y, compute (mean, std,
 *      skew, kurt_excess) of daily log returns; compare to the same
 *      moments of the historical daily log returns used for calibration.
 *
 *   3. Realized-vol stability: 21-day rolling realized vol from history.
 *      Compare mean to sqrt(theta), and report the empirical vol-of-vol
 *      so the user can see if their fixed sigma_v=0.4 is off.
 */

typedef struct {
    char   symbol[MAX_SYMBOL_LEN];

    /* Calibrated params we evaluated. */
    double v0;
    double theta;
    double kappa;
    double sigma_v;
    double rho;

    /* Historical window. */
    int    n_history_bars;
    double hist_window_years;

    /* Feller. */
    double feller_lhs;             /* 2*kappa*theta            */
    double feller_rhs;             /* sigma_v^2                */
    int    feller_ok;              /* lhs >= rhs               */

    /* Daily log return moments — historical. */
    double hist_mean_ann;          /* annualised               */
    double hist_std_ann;
    double hist_skew;
    double hist_kurt_excess;

    /* Daily log return moments — simulated (1y MC at calibrated params). */
    int    n_paths_used;
    double sim_mean_ann;
    double sim_std_ann;
    double sim_skew;
    double sim_kurt_excess;

    /* Realized-vol stability. */
    double rv21_mean_vol;          /* mean of 21d rolling vol  */
    double rv21_std_vol;           /* stdev of 21d rolling vol */
    double sqrt_theta;             /* long-run vol from theta  */
    double empirical_vol_of_vol;   /* candidate sigma_v        */

    /* Overall scores in [0, 1]. 1 = great fit, 0 = no agreement. */
    double moment_match_score;
    double mean_reversion_score;   /* how close rv21_mean is to sqrt_theta */
    double overall_score;          /* simple combine                       */
} HestonDiagnostics;

/*
 * Run the Layer-1 diagnostics. Caller passes the bar history used for
 * calibration; we re-calibrate, re-simulate, and fill `out`. Returns 0
 * on success, -1 on bad inputs / too little history.
 *   n_paths: e.g. 4000. Higher = more stable moment estimates.
 */
int heston_diagnostics(const OHLCBar *bars, int n_bars,
                       const char *symbol,
                       int n_paths,
                       uint64_t rng_seed,
                       HestonDiagnostics *out);

#endif /* HESTON_H */
