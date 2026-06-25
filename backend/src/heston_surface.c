#include "heston_surface.h"
#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * heston_surface.c  -  Heston call pricing (Lewis integral) + BS IV
 *                      inversion + (strike, maturity) sweep.
 *
 * Uses C99 <complex.h> — much cleaner than rolling our own.
 */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif

/* MSVC's complex.h differs from POSIX; we're on gcc/MinGW so the
 * GNU spelling works. _Complex_I is the imaginary unit. */

/* ---------- Heston CF at complex argument (little trap form) ---------- */

/*
 * phi(omega; T) for X = ln(S_T) under risk-neutral measure:
 *
 *   phi(omega) = exp( i*omega*(ln(S0) + r*T) + C(omega) + D(omega)*v0 )
 *
 *   xi  = kappa - rho*sigma*i*omega
 *   d   = sqrt(xi^2 + sigma^2 * (i*omega + omega^2))
 *   A1  = i*omega*(i*omega - 1) * sinh(d*T/2)
 *   A2  = d*cosh(d*T/2) + xi*sinh(d*T/2)
 *   D   = A1 / A2
 *   B   = d * exp(kappa*T/2) / A2
 *   C   = (kappa*theta/sigma^2) * ((kappa - rho*sigma*i*omega)*T - 2*ln(B))
 *
 * Albrecher (2007) "little trap" keeps the complex logarithm in the
 * principal branch — no manual branch-cut tracking required.
 */
static double complex heston_cf_complex(double complex omega,
                                        double S0, double T, double r,
                                        double kappa, double theta,
                                        double sigma, double rho,
                                        double v0)
{
    double complex iom = _Complex_I * omega;
    double complex xi = kappa - rho * sigma * iom;
    double complex d = csqrt(xi * xi + sigma * sigma * (iom + omega * omega));
    double complex sinhDT = csinh(d * T / 2.0);
    double complex coshDT = ccosh(d * T / 2.0);
    double complex A2 = d * coshDT + xi * sinhDT;

    /* D(omega) = i*omega*(i*omega - 1) * sinh(d*T/2) / A2
     * C(omega) = (kappa*theta/sigma^2) * ( xi*T - 2*log(A2/d) )
     * phi(omega) = exp( i*omega*(ln S0 + r*T) + C + D*v0 )
     *
     * Sanity check: at omega = 0, xi=kappa, d=kappa, A2=kappa*exp(kT/2),
     * A2/d = exp(kT/2), 2*log(A2/d) = kappa*T, so C(0) = 0 and
     * D(0) = 0  =>  phi(0) = 1. Confirmed.
     */
    double complex Dpart = iom * (iom - 1.0) * sinhDT / A2;
    double complex G = A2 / d;
    double complex Cpart = (kappa * theta / (sigma * sigma)) *
                           (xi * T - 2.0 * clog(G));

    return cexp(iom * (log(S0) + r * T) + Cpart + Dpart * v0);
}

/* ---------- Heston (1993) original P1/P2 call pricing ----------
 *
 *   C(S, K, T) = S * P1 - K * exp(-r*T) * P2
 *
 *   P_j = 1/2 + 1/pi * integral_0^inf Re[ exp(-i*u*ln(K)) * f_j(u) / (i*u) ] du
 *
 *   f_2(u) = phi(u)
 *   f_1(u) = phi(u - i) / (S0 * exp(r*T))
 *
 * Two Simpson integrals over the same u grid; both decay as u grows
 * because the CFs do. Numerically much more forgiving than the Lewis
 * shifted-CF form for our calibration parameters.
 */
double heston_call_price(const HestonParams *p,
                         double S, double K,
                         double T, double r)
{
    if (!p || S <= 0 || K <= 0 || T <= 0)
        return NAN;

    double logK = log(K);

    /* Vol-adaptive upper limit. */
    double sigma_h = sqrt((p->v0 > 0 ? p->v0 : 0.04) * T);
    double u_max = 50.0 / (sigma_h + 0.05);
    if (u_max < 50.0)
        u_max = 50.0;
    if (u_max > 400.0)
        u_max = 400.0;

    const int N = 512;
    double du = u_max / N;

    /* Tiny u floor avoids the 1/(i*u) singularity at u=0. We start the
     * trapezoidal-style sweep at u = du and use Simpson on [du, u_max].
     * The CF -> 1 as u -> 0 so the integrand near 0 is bounded after
     * the regularised 1/(iu) term — we just skip u=0 explicitly. */

    double sum1 = 0.0, sum2 = 0.0;
    for (int i = 1; i <= N; i++)
    {
        double u = i * du;
        double complex iu = _Complex_I * u;

        double complex phi_u = heston_cf_complex(u, S, T, r,
                                                 p->kappa, p->theta,
                                                 p->sigma_v, p->rho, p->v0);
        double complex phi_um1 = heston_cf_complex(u - _Complex_I * 1.0,
                                                   S, T, r,
                                                   p->kappa, p->theta,
                                                   p->sigma_v, p->rho, p->v0);

        /* f_1(u) = phi(u-i) / (S * exp(r*T))  — the stock-numeraire CF */
        double complex f1 = phi_um1 / (S * exp(r * T));
        double complex f2 = phi_u;

        double complex e_iuk = cexp(-_Complex_I * u * logK);
        double complex t1 = e_iuk * f1 / iu;
        double complex t2 = e_iuk * f2 / iu;

        /* Simpson weights: 4 on odd, 2 on even, 1 on endpoints. */
        double w = (i == N) ? 1.0 : (i % 2 == 1) ? 4.0
                                                 : 2.0;
        sum1 += w * creal(t1);
        sum2 += w * creal(t2);
    }
    sum1 *= du / 3.0;
    sum2 *= du / 3.0;

    double P1 = 0.5 + sum1 / M_PI;
    double P2 = 0.5 + sum2 / M_PI;

    /* Probabilities should be in [0, 1] in a healthy calibration —
     * clamp to keep one rogue grid point from poisoning the surface. */
    if (P1 < 0)
        P1 = 0;
    else if (P1 > 1)
        P1 = 1;
    if (P2 < 0)
        P2 = 0;
    else if (P2 > 1)
        P2 = 1;

    double price = S * P1 - K * exp(-r * T) * P2;
    if (!isfinite(price))
        return NAN;
    if (price < 0)
        price = 0.0;
    if (price > S)
        price = S;
    return price;
}

/* ---------- Black-Scholes call + Newton inversion ---------- */

static double bs_norm_cdf(double x)
{
    /* Abramowitz & Stegun approximation, plenty accurate for IV root-find. */
    return 0.5 * erfc(-x * M_SQRT1_2);
}

static double bs_norm_pdf(double x)
{
    return (1.0 / sqrt(2.0 * M_PI)) * exp(-0.5 * x * x);
}

static double bs_call_price(double S, double K, double T, double r, double vol)
{
    if (vol <= 0.0 || T <= 0.0)
    {
        double intr = S - K * exp(-r * T);
        return intr > 0 ? intr : 0.0;
    }
    double sigT = vol * sqrt(T);
    double d1 = (log(S / K) + (r + 0.5 * vol * vol) * T) / sigT;
    double d2 = d1 - sigT;
    return S * bs_norm_cdf(d1) - K * exp(-r * T) * bs_norm_cdf(d2);
}

static double bs_vega(double S, double K, double T, double r, double vol)
{
    if (vol <= 0.0 || T <= 0.0)
        return 0.0;
    double sigT = vol * sqrt(T);
    double d1 = (log(S / K) + (r + 0.5 * vol * vol) * T) / sigT;
    return S * bs_norm_pdf(d1) * sqrt(T);
}

double bs_implied_vol_call(double price, double S, double K, double T, double r)
{
    if (price <= 0 || S <= 0 || K <= 0 || T <= 0)
        return NAN;

    /* Bound checks: price must lie in [max(S - Ke^{-rT}, 0), S]. */
    double intr = S - K * exp(-r * T);
    double lo_p = intr > 0 ? intr : 0.0;
    if (price < lo_p - 1e-8)
        return NAN;
    if (price >= S - 1e-8)
        return NAN;

    double vol = 0.3; /* warm start */
    for (int it = 0; it < 60; it++)
    {
        double f = bs_call_price(S, K, T, r, vol) - price;
        double v = bs_vega(S, K, T, r, vol);
        if (fabs(f) < 1e-7)
            return vol;
        if (v < 1e-10)
        {
            /* Vega collapsed; fall back to bisection */
            double a = 1e-4, b = 5.0;
            for (int j = 0; j < 100; j++)
            {
                double m = 0.5 * (a + b);
                double fm = bs_call_price(S, K, T, r, m) - price;
                if (fabs(fm) < 1e-7)
                    return m;
                if (fm > 0)
                    b = m;
                else
                    a = m;
            }
            return 0.5 * (a + b);
        }
        double step = f / v;
        if (step > 0.5)
            step = 0.5;
        if (step < -0.5)
            step = -0.5;
        vol -= step;
        if (vol < 1e-4)
            vol = 1e-4;
        if (vol > 5.0)
            vol = 5.0;
    }
    return vol;
}

/* ---------- Surface sweep ---------- */

int heston_surface_build(const HestonParams *p,
                         double spot,
                         double moneyness_lo,
                         double moneyness_hi,
                         int n_strikes,
                         int max_mat_days,
                         int n_maturities,
                         double r,
                         HestonSurface *out)
{
    if (!p || !out)
        return -1;
    if (spot <= 0)
        return -1;
    if (n_strikes < 2 || n_maturities < 2)
        return -1;
    if (moneyness_lo <= 0)
        moneyness_lo = 0.7;
    if (moneyness_hi <= moneyness_lo)
        moneyness_hi = 1.3;
    if (max_mat_days < 7)
        max_mat_days = 90;

    double *strikes = malloc((size_t)n_strikes * sizeof(double));
    double *moneyness = malloc((size_t)n_strikes * sizeof(double));
    double *mat_days = malloc((size_t)n_maturities * sizeof(double));
    double *mat_T = malloc((size_t)n_maturities * sizeof(double));
    double *iv = malloc((size_t)n_strikes * (size_t)n_maturities * sizeof(double));
    if (!strikes || !moneyness || !mat_days || !mat_T || !iv)
    {
        free(strikes);
        free(moneyness);
        free(mat_days);
        free(mat_T);
        free(iv);
        return -1;
    }

    /* Log-spaced strikes for symmetric coverage either side of ATM. */
    double log_lo = log(moneyness_lo);
    double log_hi = log(moneyness_hi);
    for (int i = 0; i < n_strikes; i++)
    {
        double t = (double)i / (double)(n_strikes - 1);
        double m = exp(log_lo + t * (log_hi - log_lo));
        moneyness[i] = m;
        strikes[i] = spot * m;
    }

    /* Linearly-spaced maturities; skip 0. */
    for (int j = 0; j < n_maturities; j++)
    {
        double days = (double)(j + 1) / (double)n_maturities * (double)max_mat_days;
        mat_days[j] = days;
        mat_T[j] = days / 365.0;
    }

    double iv_lo = 1e9, iv_hi = -1e9;
    int n_fail = 0;

    /* Sweep the grid. We use a fresh HestonParams with adjusted T per
     * call; only T is used by the pricer for the maturity slice. */
    HestonParams p_local = *p;

    for (int i = 0; i < n_strikes; i++)
    {
        for (int j = 0; j < n_maturities; j++)
        {
            p_local.T = mat_T[j];
            p_local.steps = 1; /* unused by the CF pricer */
            double price = heston_call_price(&p_local, spot, strikes[i],
                                             mat_T[j], r);
            double vol = isfinite(price)
                             ? bs_implied_vol_call(price, spot, strikes[i], mat_T[j], r)
                             : NAN;
            if (!isfinite(vol))
            {
                n_fail++;
                /* Use neighbour fallback later; mark NaN for now and
                 * fill in a second pass. */
                iv[i * n_maturities + j] = NAN;
            }
            else
            {
                iv[i * n_maturities + j] = vol;
                if (vol < iv_lo)
                    iv_lo = vol;
                if (vol > iv_hi)
                    iv_hi = vol;
            }
        }
    }

    /* Fill NaN cells from the closest valid neighbour so the surface
     * stays continuous when the frontend renders it. */
    for (int i = 0; i < n_strikes; i++)
    {
        for (int j = 0; j < n_maturities; j++)
        {
            if (isfinite(iv[i * n_maturities + j]))
                continue;
            double sum = 0.0;
            int cnt = 0;
            for (int di = -1; di <= 1; di++)
            {
                for (int dj = -1; dj <= 1; dj++)
                {
                    int ii = i + di, jj = j + dj;
                    if (ii < 0 || ii >= n_strikes)
                        continue;
                    if (jj < 0 || jj >= n_maturities)
                        continue;
                    double v = iv[ii * n_maturities + jj];
                    if (isfinite(v))
                    {
                        sum += v;
                        cnt++;
                    }
                }
            }
            iv[i * n_maturities + j] = cnt > 0 ? sum / cnt : sqrt(p->v0);
        }
    }

    if (iv_lo > iv_hi)
    {
        iv_lo = sqrt(p->v0);
        iv_hi = sqrt(p->v0) * 1.5;
    }

    out->n_strikes = n_strikes;
    out->n_maturities = n_maturities;
    out->strikes = strikes;
    out->moneyness = moneyness;
    out->maturities_days = mat_days;
    out->maturities_T = mat_T;
    out->iv = iv;
    out->spot = spot;
    out->iv_min = iv_lo;
    out->iv_max = iv_hi;
    out->n_failed = n_fail;
    return 0;
}

void heston_surface_free(HestonSurface *s)
{
    if (!s)
        return;
    free(s->strikes);
    s->strikes = NULL;
    free(s->moneyness);
    s->moneyness = NULL;
    free(s->maturities_days);
    s->maturities_days = NULL;
    free(s->maturities_T);
    s->maturities_T = NULL;
    free(s->iv);
    s->iv = NULL;
    s->n_strikes = s->n_maturities = 0;
}
