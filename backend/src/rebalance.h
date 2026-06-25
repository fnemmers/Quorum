#ifndef REBALANCE_H
#define REBALANCE_H

#include "market_data.h"   /* MAX_SYMBOL_LEN */
#include "risk_score.h"    /* ScoredResult */

/*
 * rebalance  -  Obscurity-scored rebalance routing for held positions.
 *
 * The obscurity score collapses four signals into one scalar saying
 * "how dicey is this rebalance decision".
 *
 *     score_gap_clarity  = clip(|new_blend - exit_threshold| / sigma, 0, 1)
 *     llm_agreement      = 1 - bot_disagreement      (0..1)
 *     heston_breach      = 1 if ES_95 past risk limit, else 0
 *     horizon_maturity   = clip(days_held / intended_hold_days, 0, 1)
 *
 *     clarity   = 0.35*gap + 0.25*llm + 0.25*breach + 0.15*horizon
 *     obscurity = 1 - clarity
 *
 * Routing:
 *     obscurity < 0.25         -> AUTO_EXECUTE  (act, log it)
 *     0.25 <= obscurity < 0.6  -> AUTO_NOTIFY   (act, ping the user)
 *     obscurity >= 0.6         -> ESCALATE      (block, ask the user)
 *
 * The thresholds and weights live in RebalanceParams so they can be
 * tuned from overrides logged in rebalance_events. Calibration loop:
 *   if the user keeps overriding AUTO_NOTIFY in one direction, the
 *   thresholds are too loose (or too tight) and should shift.
 */

typedef enum {
    REBAL_HOLD          = 0,   /* new score still above exit threshold */
    REBAL_AUTO_EXECUTE  = 1,   /* obscurity < auto_threshold           */
    REBAL_AUTO_NOTIFY   = 2,   /* between auto_threshold and escalate  */
    REBAL_ESCALATE      = 3    /* obscurity >= escalate_threshold       */
} RebalanceDecision;

typedef enum {
    REBAL_ACTION_NONE     = 0,
    REBAL_ACTION_SELL     = 1,
    REBAL_ACTION_TRIM     = 2,    /* reduce - not full exit             */
    REBAL_ACTION_FLIP     = 3     /* close long, open short (or vice)   */
} RebalanceAction;

typedef struct {
    /* Components, each in [0, 1]. */
    double score_gap_clarity;
    double llm_agreement;
    double heston_breach;
    double horizon_maturity;

    /* Weighted sum -> clarity, then 1 - clarity. */
    double clarity;
    double obscurity;

    /* Which single component was the largest *drag* (clarity contributor
     * with lowest value, weight-adjusted). Surfaced in the debrief so
     * the user sees the reason at a glance: "horizon_maturity = 0.13". */
    const char *primary_driver;
} ObscurityBreakdown;

typedef struct {
    char              symbol[MAX_SYMBOL_LEN];
    RebalanceDecision decision;
    RebalanceAction   suggested_action;

    double            old_blended_score;
    double            new_blended_score;
    double            exit_threshold;

    int               days_held;
    int               intended_hold_days;

    ObscurityBreakdown obscurity;

    /* Human-readable summary the IPC layer can ship straight to the UI. */
    char              debrief[512];
} RebalanceEvent;

typedef struct {
    /* Component weights, sum to 1.0. */
    double w_gap;          /* 0.35 default */
    double w_llm;          /* 0.25 default */
    double w_breach;       /* 0.25 default */
    double w_horizon;      /* 0.15 default */

    /* Routing thresholds on obscurity. */
    double auto_threshold;     /* 0.25 default */
    double escalate_threshold; /* 0.60 default */

    /* What counts as a hard Heston breach: ES_95 worse (more negative)
     * than this triggers heston_breach = 1.0. */
    double es_risk_limit;      /* e.g. -0.10 (10% tail loss)        */

    /* Sigma of the blended score across the universe at scoring time.
     * Used to normalize the score gap. Cache this with the run. */
    double sigma_blend;
} RebalanceParams;

RebalanceParams rebalance_params_default(void);

/*
 * Compute the obscurity-routed decision for one held position.
 *
 *   sym              - symbol the position is in
 *   current_idx      - index of `sym` in `ranked` (ranked is sorted desc)
 *   ranked           - the universe sorted by blended_score desc
 *   n                - length of ranked
 *   old_blend        - blended score at entry (cached from prior run)
 *   exit_threshold   - the score below which we want out (e.g. score
 *                      at rank=2K, beyond the top-K trade band)
 *   days_held        - calendar days since entry
 *   hold_target_days - intended hold horizon (e.g. 21 for 3 weeks)
 *   params           - tuning knobs
 *   out              - filled on success
 *
 * Returns 0 on success, -1 if `sym` is not present in `ranked`.
 */
int rebalance_evaluate(const char            *sym,
                       const ScoredResult    *ranked,
                       int                    n,
                       double                 old_blend,
                       double                 exit_threshold,
                       int                    days_held,
                       int                    hold_target_days,
                       RebalanceParams        params,
                       RebalanceEvent        *out);

#endif /* REBALANCE_H */
