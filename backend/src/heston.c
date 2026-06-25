#include "heston.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * heston.c  -  Heston Monte Carlo with convergence-based stopping.
 *
 * RNG: xorshift128+ (fast, statistically fine for MC).
 * Normals: Box-Muller, two normals per call.
 * Discretization: log-Euler on S, full-truncation on v.
 * Convergence: relative tolerance on ES_95 between consecutive batches.
 */

/* ---------- xorshift128+ ---------- */

typedef struct {
    uint64_t s[2];
    int have_spare;
    double spare;
} Rng;

static void rng_init(Rng *r, uint64_t seed) {
    /* SplitMix64 to fan a single seed out to a 128-bit state. */
    uint64_t z = seed + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    r->s[0] = z ^ (z >> 31);

    z = (seed + 0x9e3779b97f4a7c15ULL) + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    r->s[1] = z ^ (z >> 31);

    if (r->s[0] == 0 && r->s[1] == 0) r->s[0] = 1;
    r->have_spare = 0;
    r->spare = 0.0;
}

static uint64_t rng_next_u64(Rng *r) {
    uint64_t x = r->s[0];
    uint64_t y = r->s[1];
    r->s[0] = y;
    x ^= x << 23;
    r->s[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
    return r->s[1] + y;
}

/* Uniform in (0, 1). */
static double rng_uniform(Rng *r) {
    /* Top 53 bits, then scale. Avoid exact 0. */
    uint64_t u = rng_next_u64(r) >> 11;
    double d = (double)u * (1.0 / 9007199254740992.0);
    if (d <= 0.0) d = 1e-300;
    return d;
}

/* Standard normal via Box-Muller, with a cached spare. */
static double rng_normal(Rng *r) {
    if (r->have_spare) {
        r->have_spare = 0;
        return r->spare;
    }
    double u1 = rng_uniform(r);
    double u2 = rng_uniform(r);
    double mag = sqrt(-2.0 * log(u1));
    double z1 = mag * cos(2.0 * M_PI * u2);
    double z2 = mag * sin(2.0 * M_PI * u2);
    r->spare = z2;
    r->have_spare = 1;
    return z1;
}

/* ---------- ES_95 over a sorted return array ---------- */

static int cmp_double_asc(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/*
 * Expected shortfall at 95% confidence: mean of the worst 5% of returns.
 * Returns 0 if n < 20 (not enough tail to be meaningful).
 */
static double expected_shortfall_95(double *returns, int n) {
    if (n < 20) return 0.0;
    qsort(returns, n, sizeof(double), cmp_double_asc);
    int tail_n = n / 20;          /* 5% of n */
    if (tail_n < 1) tail_n = 1;
    double sum = 0.0;
    for (int i = 0; i < tail_n; i++) sum += returns[i];
    return sum / (double)tail_n;
}

/* ---------- Single-path Heston simulation, returns the terminal return. ---------- */

static double simulate_one_path(const HestonParams *p, Rng *rng) {
    double dt = p->T / (double)p->steps;
    double sqrt_dt = sqrt(dt);
    double rho_comp = sqrt(1.0 - p->rho * p->rho);

    double v = p->v0;
    double log_s = log(p->s0);

    for (int i = 0; i < p->steps; i++) {
        double z1 = rng_normal(rng);
        double eps = rng_normal(rng);
        double z2 = p->rho * z1 + rho_comp * eps;

        double v_pos = v > 0.0 ? v : 0.0;
        double sqrt_v = sqrt(v_pos);

        log_s += (p->mu - 0.5 * v_pos) * dt + sqrt_v * sqrt_dt * z1;
        v     += p->kappa * (p->theta - v_pos) * dt
                 + p->sigma_v * sqrt_v * sqrt_dt * z2;
    }

    double s_t = exp(log_s);
    return (s_t - p->s0) / p->s0;
}

/* ---------- Public API ---------- */

int heston_run(const HestonParams *p,
               HestonScore *out,
               int paths_per_batch,
               int max_paths,
               double es_tol_rel,
               uint64_t rng_seed) {
    if (!p || !out)                          return -1;
    if (p->s0 <= 0.0 || p->T <= 0.0)         return -1;
    if (p->steps <= 0)                       return -1;
    if (paths_per_batch <= 0)                paths_per_batch = 5000;
    if (max_paths < paths_per_batch)         max_paths = paths_per_batch * 4;
    if (es_tol_rel <= 0.0)                   es_tol_rel = 0.005;

    /* Working buffers - capped at max_paths. */
    double *returns = malloc((size_t)max_paths * sizeof(double));
    if (!returns) return -1;

    Rng rng;
    rng_init(&rng, rng_seed ? rng_seed : 0xDEADBEEFCAFEBABEULL);

    int    n_total = 0;
    double prev_es = 0.0;
    int    converged = 0;
    int    n_loss_5 = 0;

    while (n_total < max_paths) {
        int batch = paths_per_batch;
        if (n_total + batch > max_paths) batch = max_paths - n_total;

        for (int i = 0; i < batch; i++) {
            double r = simulate_one_path(p, &rng);
            returns[n_total + i] = r;
            if (r < -0.05) n_loss_5++;
        }
        n_total += batch;

        /* Convergence check: ES_95 stable batch-over-batch.
         * We copy the buffer to avoid mutating the running sample
         * (qsort would reorder it and bias subsequent batches). */
        double *copy = malloc((size_t)n_total * sizeof(double));
        if (!copy) { free(returns); return -1; }
        memcpy(copy, returns, (size_t)n_total * sizeof(double));
        double es = expected_shortfall_95(copy, n_total);
        free(copy);

        if (n_total >= paths_per_batch * 2) {
            double denom = fabs(prev_es) > 1e-9 ? fabs(prev_es) : 1e-9;
            if (fabs(es - prev_es) / denom < es_tol_rel) {
                converged = 1;
                prev_es = es;
                break;
            }
        }
        prev_es = es;
    }

    /* Final stats over the un-sorted buffer. */
    double mean = 0.0;
    for (int i = 0; i < n_total; i++) mean += returns[i];
    mean /= (double)n_total;

    double var = 0.0;
    for (int i = 0; i < n_total; i++) {
        double d = returns[i] - mean;
        var += d * d;
    }
    var /= (double)(n_total > 1 ? n_total - 1 : 1);
    double horizon_vol = sqrt(var);
    /* Annualize: scale by sqrt(1/T). */
    double annualized_vol = horizon_vol * sqrt(1.0 / p->T);

    out->expected_return = mean;
    out->forward_vol     = annualized_vol;
    out->es_95           = prev_es;
    out->prob_loss_5     = (double)n_loss_5 / (double)n_total;
    out->n_paths_used    = n_total;
    out->converged       = converged;

    free(returns);
    return 0;
}

/* ---------- Calibration from history ---------- */

int heston_calibrate_from_history(const OHLCBar *bars, int n_bars,
                                  double horizon_years, int steps,
                                  HestonParams *out) {
    if (!bars || !out || n_bars < 30) return -1;
    if (horizon_years <= 0.0)         horizon_years = 21.0 / 252.0;
    if (steps <= 0)                   steps = 21;

    /* Log returns from close-to-close. */
    int n_ret = n_bars - 1;
    double *log_ret = malloc((size_t)n_ret * sizeof(double));
    if (!log_ret) return -1;

    for (int i = 0; i < n_ret; i++) {
        double c0 = bars[i].close;
        double c1 = bars[i + 1].close;
        if (c0 <= 0.0 || c1 <= 0.0) { free(log_ret); return -1; }
        log_ret[i] = log(c1 / c0);
    }

    double mean = 0.0;
    for (int i = 0; i < n_ret; i++) mean += log_ret[i];
    mean /= (double)n_ret;

    double var = 0.0;
    for (int i = 0; i < n_ret; i++) {
        double d = log_ret[i] - mean;
        var += d * d;
    }
    var /= (double)(n_ret > 1 ? n_ret - 1 : 1);

    /* Annualize using 252 trading days. */
    double ann_mean = mean * 252.0;
    double ann_var  = var * 252.0;

    out->s0      = bars[n_bars - 1].close;
    out->mu      = ann_mean;
    out->v0      = ann_var;
    out->theta   = ann_var;
    out->kappa   = 3.0;     /* literature default for US equities */
    out->sigma_v = 0.4;
    out->rho     = -0.7;
    out->T       = horizon_years;
    out->steps   = steps;

    free(log_ret);
    return 0;
}

/* ---------- Risk-adjusted scalar ---------- */

double heston_risk_adjusted_score(const HestonScore *s,
                                  double lambda1, double lambda2) {
    if (!s) return 0.0;
    return s->expected_return
         - lambda1 * fabs(s->es_95)
         - lambda2 * s->prob_loss_5;
}

/* ---------- Path bundle (density + quantiles) ---------- */

/* Same single-path engine but writes the full trajectory of prices to
 * out_prices[0..steps] (length steps+1, including the initial spot). */
static void simulate_path_record(const HestonParams *p, Rng *rng,
                                 double *out_prices) {
    double dt      = p->T / (double)p->steps;
    double sqrt_dt = sqrt(dt);
    double rho_comp = sqrt(1.0 - p->rho * p->rho);

    double v     = p->v0;
    double log_s = log(p->s0);
    out_prices[0] = p->s0;

    for (int i = 0; i < p->steps; i++) {
        double z1  = rng_normal(rng);
        double eps = rng_normal(rng);
        double z2  = p->rho * z1 + rho_comp * eps;

        double v_pos = v > 0.0 ? v : 0.0;
        double sqrt_v = sqrt(v_pos);

        log_s += (p->mu - 0.5 * v_pos) * dt + sqrt_v * sqrt_dt * z1;
        v     += p->kappa * (p->theta - v_pos) * dt
                 + p->sigma_v * sqrt_v * sqrt_dt * z2;

        out_prices[i + 1] = exp(log_s);
    }
}

int heston_path_bundle(const HestonParams *p, int n_paths,
                       int n_buckets, double price_span_sigmas,
                       HestonPathBundle *out, uint64_t rng_seed) {
    if (!p || !out)               return -1;
    if (p->s0 <= 0.0 || p->T <= 0.0) return -1;
    if (p->steps <= 0)            return -1;
    if (n_paths <= 0)             n_paths = 5000;
    if (n_buckets <= 0)           n_buckets = 100;
    if (price_span_sigmas <= 0.0) price_span_sigmas = 3.5;

    int n_steps = p->steps;
    int n_cols  = n_steps + 1;

    /* Price-range bracket using v0 as the working variance estimate.
     *   sigma_horizon = sqrt(v0 * T)
     *   price_lo = s0 * exp(-K * sigma_horizon)
     *   price_hi = s0 * exp( K * sigma_horizon)
     * Anything outside the bracket gets clamped into the edge buckets. */
    double sigma_h = sqrt((p->v0 > 0.0 ? p->v0 : 0.0) * p->T);
    double k_sd    = price_span_sigmas;
    double lo      = p->s0 * exp(-k_sd * sigma_h);
    double hi      = p->s0 * exp( k_sd * sigma_h);
    if (hi <= lo)  { lo = p->s0 * 0.7; hi = p->s0 * 1.3; }

    out->n_steps   = n_steps;
    out->n_buckets = n_buckets;
    out->price_min = lo;
    out->price_max = hi;
    out->s0        = p->s0;
    out->T         = p->T;
    out->density   = NULL;
    out->p05 = out->p50 = out->p95 = NULL;

    int    *density = calloc((size_t)n_buckets * (size_t)n_cols, sizeof(int));
    double *p05     = calloc((size_t)n_cols,                       sizeof(double));
    double *p50     = calloc((size_t)n_cols,                       sizeof(double));
    double *p95     = calloc((size_t)n_cols,                       sizeof(double));
    double *path    = malloc((size_t)n_cols * sizeof(double));
    /* column buffer for sorting one time-slice at a time */
    double *col_buf = malloc((size_t)n_paths * sizeof(double));
    /* terminal returns for ES_95 */
    double *terms   = malloc((size_t)n_paths * sizeof(double));
    /* prices_all[step][path] = price[path][step] -- needed for column sort.
     * Stored column-major (step varies slowest, path varies fastest). */
    double *prices_all =
        malloc((size_t)n_cols * (size_t)n_paths * sizeof(double));

    if (!density || !p05 || !p50 || !p95 ||
        !path || !col_buf || !terms || !prices_all) {
        free(density); free(p05); free(p50); free(p95);
        free(path);    free(col_buf); free(terms); free(prices_all);
        return -1;
    }

    Rng rng;
    rng_init(&rng, rng_seed ? rng_seed : 0xA17EB00B0FFEC0DEULL);

    double inv_span = 1.0 / (hi - lo);

    for (int i = 0; i < n_paths; i++) {
        simulate_path_record(p, &rng, path);

        /* Bucket and store each step's price. */
        for (int s = 0; s < n_cols; s++) {
            double price = path[s];
            /* density bucketing -- clamp to grid edges */
            double frac = (price - lo) * inv_span;
            int b = (int)(frac * n_buckets);
            if (b < 0)             b = 0;
            if (b >= n_buckets)    b = n_buckets - 1;
            density[b * n_cols + s]++;
            /* store column-major for later sort */
            prices_all[(size_t)s * (size_t)n_paths + (size_t)i] = price;
        }

        terms[i] = (path[n_steps] - p->s0) / p->s0;
    }

    /* Per-step quantiles. Sort each column and pick indices. */
    int idx05 = (int)(0.05 * n_paths);
    int idx50 = (int)(0.50 * n_paths);
    int idx95 = (int)(0.95 * n_paths);
    if (idx95 >= n_paths) idx95 = n_paths - 1;

    for (int s = 0; s < n_cols; s++) {
        memcpy(col_buf, &prices_all[(size_t)s * (size_t)n_paths],
               (size_t)n_paths * sizeof(double));
        qsort(col_buf, (size_t)n_paths, sizeof(double), cmp_double_asc);
        p05[s] = col_buf[idx05];
        p50[s] = col_buf[idx50];
        p95[s] = col_buf[idx95];
    }

    /* Expected return + ES_95 of terminal returns. */
    double mean = 0.0;
    for (int i = 0; i < n_paths; i++) mean += terms[i];
    mean /= (double)n_paths;

    qsort(terms, (size_t)n_paths, sizeof(double), cmp_double_asc);
    int tail_n = n_paths / 20;
    if (tail_n < 1) tail_n = 1;
    double sum_tail = 0.0;
    for (int i = 0; i < tail_n; i++) sum_tail += terms[i];
    double es_95 = sum_tail / (double)tail_n;

    out->density         = density;
    out->p05             = p05;
    out->p50             = p50;
    out->p95             = p95;
    out->es_95           = es_95;
    out->expected_return = mean;
    out->n_paths_used    = n_paths;

    free(path);
    free(col_buf);
    free(terms);
    free(prices_all);
    return 0;
}

void heston_path_bundle_free(HestonPathBundle *b) {
    if (!b) return;
    free(b->density); b->density = NULL;
    free(b->p05);     b->p05     = NULL;
    free(b->p50);     b->p50     = NULL;
    free(b->p95);     b->p95     = NULL;
    b->n_steps = 0;
    b->n_buckets = 0;
}

/* ---------- Layer-1 diagnostics ---------- */

/* Mean, sample std, skew, excess kurtosis from a contiguous double array. */
static void moments4(const double *x, int n,
                     double *mean_out, double *std_out,
                     double *skew_out, double *kurt_ex_out) {
    if (n < 2) {
        *mean_out = 0; *std_out = 0; *skew_out = 0; *kurt_ex_out = 0;
        return;
    }
    double m = 0;
    for (int i = 0; i < n; i++) m += x[i];
    m /= n;

    double m2 = 0, m3 = 0, m4 = 0;
    for (int i = 0; i < n; i++) {
        double d = x[i] - m;
        double d2 = d * d;
        m2 += d2;
        m3 += d2 * d;
        m4 += d2 * d2;
    }
    m2 /= n;
    m3 /= n;
    m4 /= n;

    double var_sample = m2 * (double)n / (double)(n - 1);  /* unbiased  */
    double sd = sqrt(var_sample);
    double skew = (m2 > 0) ? m3 / pow(m2, 1.5) : 0;
    double kurt_ex = (m2 > 0) ? (m4 / (m2 * m2)) - 3.0 : 0;

    *mean_out    = m;
    *std_out     = sd;
    *skew_out    = skew;
    *kurt_ex_out = kurt_ex;
}

/* Score in [0,1] for how close `model` is to `target` on a moment.
 * Uses relative tolerance — defaults to 25% relative error mapped to 0. */
static double moment_score(double model, double target, double rel_tol) {
    double denom = fabs(target);
    if (denom < 1e-9) denom = 1e-9;
    double rel = fabs(model - target) / denom;
    double s   = 1.0 - rel / rel_tol;
    if (s < 0) s = 0;
    if (s > 1) s = 1;
    return s;
}

int heston_diagnostics(const OHLCBar *bars, int n_bars,
                       const char *symbol,
                       int n_paths,
                       uint64_t rng_seed,
                       HestonDiagnostics *out) {
    if (!bars || !out || n_bars < 60) return -1;
    if (n_paths <= 0) n_paths = 4000;

    memset(out, 0, sizeof(*out));
    if (symbol) {
        strncpy(out->symbol, symbol, MAX_SYMBOL_LEN - 1);
        out->symbol[MAX_SYMBOL_LEN - 1] = '\0';
    }

    /* Calibrate from history (existing function). */
    HestonParams p;
    if (heston_calibrate_from_history(bars, n_bars, 1.0, 252, &p) != 0)
        return -1;

    out->v0      = p.v0;
    out->theta   = p.theta;
    out->kappa   = p.kappa;
    out->sigma_v = p.sigma_v;
    out->rho     = p.rho;
    out->n_history_bars   = n_bars;
    out->hist_window_years = (double)n_bars / 252.0;

    /* ── 1. Feller condition ───────────────────────────────── */
    out->feller_lhs = 2.0 * p.kappa * p.theta;
    out->feller_rhs = p.sigma_v * p.sigma_v;
    out->feller_ok  = out->feller_lhs >= out->feller_rhs;

    /* ── 2. Historical daily log returns + moments ─────────── */
    int n_ret = n_bars - 1;
    double *log_ret = malloc((size_t)n_ret * sizeof(double));
    if (!log_ret) return -1;
    for (int i = 0; i < n_ret; i++) {
        double c0 = bars[i].close, c1 = bars[i + 1].close;
        if (c0 <= 0 || c1 <= 0) { free(log_ret); return -1; }
        log_ret[i] = log(c1 / c0);
    }

    double hm, hs, hsk, hkurt;
    moments4(log_ret, n_ret, &hm, &hs, &hsk, &hkurt);
    out->hist_mean_ann = hm * 252.0;
    out->hist_std_ann  = hs * sqrt(252.0);
    out->hist_skew     = hsk;
    out->hist_kurt_excess = hkurt;

    /* ── 3. Simulated daily log returns over 1 year ────────── */
    HestonParams pp = p;
    pp.T     = 1.0;
    pp.steps = 252;

    int  total = n_paths * pp.steps;
    double *sim_ret = malloc((size_t)total * sizeof(double));
    double *prices  = malloc((size_t)(pp.steps + 1) * sizeof(double));
    if (!sim_ret || !prices) {
        free(log_ret); free(sim_ret); free(prices);
        return -1;
    }

    Rng rng;
    rng_init(&rng, rng_seed ? rng_seed : 0xD1A60057ABCDEF12ULL);

    int idx = 0;
    for (int i = 0; i < n_paths; i++) {
        simulate_path_record(&pp, &rng, prices);
        for (int s = 0; s < pp.steps; s++) {
            double r = log(prices[s + 1] / prices[s]);
            sim_ret[idx++] = r;
        }
    }
    out->n_paths_used = n_paths;

    double sm, ss, ssk, skurt;
    moments4(sim_ret, idx, &sm, &ss, &ssk, &skurt);
    out->sim_mean_ann    = sm * 252.0;
    out->sim_std_ann     = ss * sqrt(252.0);
    out->sim_skew        = ssk;
    out->sim_kurt_excess = skurt;

    free(prices);
    free(sim_ret);

    /* ── 4. 21-day rolling realized vol of history ─────────── */
    /* Take a rolling 21-day window stddev of daily log returns; the
     * series gives empirical vol-of-vol and a check on theta. */
    int win = 21;
    int n_rv = n_ret - win + 1;
    if (n_rv >= 2) {
        double *rv = malloc((size_t)n_rv * sizeof(double));
        if (rv) {
            for (int i = 0; i < n_rv; i++) {
                double mean = 0;
                for (int j = 0; j < win; j++) mean += log_ret[i + j];
                mean /= win;
                double var = 0;
                for (int j = 0; j < win; j++) {
                    double d = log_ret[i + j] - mean;
                    var += d * d;
                }
                var /= (double)(win - 1);
                rv[i] = sqrt(var * 252.0);  /* annualised vol */
            }
            double rv_mean, rv_std, _sk, _k;
            moments4(rv, n_rv, &rv_mean, &rv_std, &_sk, &_k);
            out->rv21_mean_vol = rv_mean;
            out->rv21_std_vol  = rv_std;
            out->sqrt_theta    = sqrt(p.theta);
            /* Empirical vol-of-vol estimate: stdev of variance series,
             * scaled to annualised. Variance = vol^2. */
            double *dv = malloc((size_t)(n_rv - 1) * sizeof(double));
            if (dv) {
                for (int i = 1; i < n_rv; i++) {
                    double vi   = rv[i]     * rv[i];
                    double vim1 = rv[i - 1] * rv[i - 1];
                    dv[i - 1] = vi - vim1;
                }
                double dvm, dvs, _a, _b;
                moments4(dv, n_rv - 1, &dvm, &dvs, &_a, &_b);
                out->empirical_vol_of_vol = dvs * sqrt(252.0);
                free(dv);
            }
            free(rv);
        }
    }

    free(log_ret);

    /* ── 5. Scoring ───────────────────────────────────────── */
    double s_std  = moment_score(out->sim_std_ann, out->hist_std_ann, 0.25);
    double s_skew = moment_score(out->sim_skew,    out->hist_skew,    1.0);
    double s_kurt = moment_score(out->sim_kurt_excess, out->hist_kurt_excess, 1.0);
    out->moment_match_score = 0.5 * s_std + 0.25 * s_skew + 0.25 * s_kurt;
    out->mean_reversion_score =
        moment_score(out->rv21_mean_vol, out->sqrt_theta, 0.25);
    out->overall_score =
        0.5 * out->moment_match_score
      + 0.3 * out->mean_reversion_score
      + 0.2 * (out->feller_ok ? 1.0 : 0.0);
    return 0;
}
