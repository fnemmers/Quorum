#ifndef HESTON_SURFACE_H
#define HESTON_SURFACE_H

#include "heston.h"

/*
 * heston_surface  -  Heston-implied vol surface over (strike, maturity).
 *
 * The reason this is here:
 *   The Monte Carlo bundle shows what paths *look like* under the model.
 *   The vol surface shows what the model *prices*. It's the canonical
 *   finance 3D plot — strike on one axis, maturity on the other,
 *   model-implied Black-Scholes vol on Z.
 *
 * Pricing path:
 *   1. Heston European call price via the Lewis (2001) integral
 *        C = S0 - (e^{-rT} sqrt(S0*K)/pi) * integral_0^inf Re[...] du
 *      uses the characteristic function of ln(S_T) under risk-neutral.
 *      "Little Heston trap" formulation (Albrecher 2007) keeps the
 *      complex logs in the principal branch.
 *
 *   2. Invert to BS implied vol via Newton iteration on vega.
 *
 *   3. Sweep (strike, maturity) grid and fill iv[i*n_mat + j].
 *
 * Calibration note: we use the same HestonParams produced by
 * heston_calibrate_from_history(). It's not a real market-vol-surface
 * calibration; it's the *model-implied* surface for the calibrated
 * dynamics. Tells the truth about what the model believes; that's the
 * point.
 */

typedef struct {
    int     n_strikes;
    int     n_maturities;
    double *strikes;         /* length n_strikes,     absolute prices       */
    double *moneyness;       /* length n_strikes,     strike / spot         */
    double *maturities_days; /* length n_maturities,  days from now         */
    double *maturities_T;    /* length n_maturities,  years                  */
    double *iv;              /* [n_strikes * n_maturities], row-major.
                              * iv[i * n_mat + j] = vol(K_i, T_j)            */
    double  spot;
    double  iv_min;
    double  iv_max;
    int     n_failed;        /* grid points where IV solver gave up         */
} HestonSurface;

/*
 * Build a surface over n_strikes log-spaced moneyness in
 * [moneyness_lo, moneyness_hi] and n_maturities linearly-spaced in
 * (0, max_mat_days]. Risk-free rate r is used inside the pricer.
 *
 * Caller must zero `out` first. Returns 0 on success, -1 on bad inputs
 * or allocation failure. Use heston_surface_free() when done.
 */
int heston_surface_build(const HestonParams *p,
                         double spot,
                         double moneyness_lo,
                         double moneyness_hi,
                         int    n_strikes,
                         int    max_mat_days,
                         int    n_maturities,
                         double r,
                         HestonSurface *out);

void heston_surface_free(HestonSurface *s);

/*
 * Heston European call price via Lewis integral. Used internally but
 * exposed for unit-test scratch.
 *
 * Returns the undiscounted call value, or NaN on numerical failure.
 */
double heston_call_price(const HestonParams *p,
                         double spot, double strike,
                         double T, double r);

/*
 * Black-Scholes implied vol from a call price. Newton on vega with a
 * brentian safety net. Returns vol, or NaN if no root found.
 */
double bs_implied_vol_call(double price, double spot, double strike,
                           double T, double r);

#endif /* HESTON_SURFACE_H */
