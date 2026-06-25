#include "rebalance.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * rebalance.c  -  Compute obscurity, pick action, format debrief.
 */

static double clip01(double x) {
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

RebalanceParams rebalance_params_default(void) {
    RebalanceParams p;
    p.w_gap              = 0.35;
    p.w_llm              = 0.25;
    p.w_breach           = 0.25;
    p.w_horizon          = 0.15;
    p.auto_threshold     = 0.25;
    p.escalate_threshold = 0.60;
    p.es_risk_limit      = -0.10;
    p.sigma_blend        = 1.0;
    return p;
}

/* Identify which component is the biggest drag on clarity (lowest
 * weighted value). Surfaced in the debrief so the user sees the cause. */
static const char *primary_drag(const ObscurityBreakdown *b, RebalanceParams p) {
    double contrib_gap     = p.w_gap     * b->score_gap_clarity;
    double contrib_llm     = p.w_llm     * b->llm_agreement;
    double contrib_breach  = p.w_breach  * b->heston_breach;
    double contrib_horizon = p.w_horizon * b->horizon_maturity;

    const char *names[4]  = {"score_gap", "llm_agreement",
                             "heston_breach", "horizon_maturity"};
    double      contrib[4] = {contrib_gap, contrib_llm,
                              contrib_breach, contrib_horizon};

    int    lo_idx = 0;
    double lo_val = contrib[0];
    for (int i = 1; i < 4; i++) {
        if (contrib[i] < lo_val) { lo_val = contrib[i]; lo_idx = i; }
    }
    return names[lo_idx];
}

static int find_symbol(const ScoredResult *ranked, int n, const char *sym) {
    for (int i = 0; i < n; i++) {
        if (strcmp(ranked[i].symbol, sym) == 0) return i;
    }
    return -1;
}

int rebalance_evaluate(const char            *sym,
                       const ScoredResult    *ranked,
                       int                    n,
                       double                 old_blend,
                       double                 exit_threshold,
                       int                    days_held,
                       int                    hold_target_days,
                       RebalanceParams        p,
                       RebalanceEvent        *out) {
    if (!sym || !ranked || !out || n <= 0) return -1;

    int idx = find_symbol(ranked, n, sym);
    if (idx < 0) return -1;

    const ScoredResult *pos = &ranked[idx];
    double new_blend = pos->blended_score;

    /* If we're still above the exit threshold, no rebalance needed.
     * The whole obscurity layer is moot in that case. */
    if (new_blend >= exit_threshold) {
        memset(out, 0, sizeof(*out));
        strncpy(out->symbol, sym, MAX_SYMBOL_LEN - 1);
        out->decision          = REBAL_HOLD;
        out->suggested_action  = REBAL_ACTION_NONE;
        out->old_blended_score = old_blend;
        out->new_blended_score = new_blend;
        out->exit_threshold    = exit_threshold;
        out->days_held         = days_held;
        out->intended_hold_days = hold_target_days;
        snprintf(out->debrief, sizeof(out->debrief),
                 "HOLD %s: score %.2f still above exit %.2f.",
                 sym, new_blend, exit_threshold);
        return 0;
    }

    /* ----- Compute the four obscurity components. ----- */

    double sigma = p.sigma_blend > 1e-9 ? p.sigma_blend : 1.0;
    double gap_clarity = clip01(fabs(new_blend - exit_threshold) / sigma);

    /* bot_disagreement is in [0, 1]. Higher = noisier = lower clarity. */
    double llm_agreement = clip01(1.0 - pos->bot_disagreement);

    double heston_breach = (pos->heston_es_95 <= p.es_risk_limit) ? 1.0 : 0.0;

    double horizon_maturity = 0.0;
    if (hold_target_days > 0) {
        horizon_maturity = clip01((double)days_held / (double)hold_target_days);
    }

    double clarity = p.w_gap     * gap_clarity
                   + p.w_llm     * llm_agreement
                   + p.w_breach  * heston_breach
                   + p.w_horizon * horizon_maturity;

    double obscurity = 1.0 - clarity;

    /* ----- Decision routing. ----- */

    RebalanceDecision decision;
    if (obscurity < p.auto_threshold)            decision = REBAL_AUTO_EXECUTE;
    else if (obscurity < p.escalate_threshold)   decision = REBAL_AUTO_NOTIFY;
    else                                         decision = REBAL_ESCALATE;

    /* Action: trim if score still positive (mildly bearish signal),
     * sell if score has flipped meaningfully negative. Flip only on
     * a hard Heston breach paired with a deep score drop. */
    RebalanceAction action;
    if (heston_breach > 0.5 && new_blend < exit_threshold - 2.0 * sigma) {
        action = REBAL_ACTION_FLIP;
    } else if (new_blend < exit_threshold - sigma) {
        action = REBAL_ACTION_SELL;
    } else {
        action = REBAL_ACTION_TRIM;
    }

    /* ----- Fill the event. ----- */

    memset(out, 0, sizeof(*out));
    strncpy(out->symbol, sym, MAX_SYMBOL_LEN - 1);
    out->decision           = decision;
    out->suggested_action   = action;
    out->old_blended_score  = old_blend;
    out->new_blended_score  = new_blend;
    out->exit_threshold     = exit_threshold;
    out->days_held          = days_held;
    out->intended_hold_days = hold_target_days;

    out->obscurity.score_gap_clarity = gap_clarity;
    out->obscurity.llm_agreement     = llm_agreement;
    out->obscurity.heston_breach     = heston_breach;
    out->obscurity.horizon_maturity  = horizon_maturity;
    out->obscurity.clarity           = clarity;
    out->obscurity.obscurity         = obscurity;
    out->obscurity.primary_driver    = primary_drag(&out->obscurity, p);

    /* Human-readable debrief. */
    const char *dec_str = (decision == REBAL_AUTO_EXECUTE) ? "AUTO"
                       : (decision == REBAL_AUTO_NOTIFY)   ? "NOTIFY"
                       :                                     "ESCALATE";
    const char *act_str = (action == REBAL_ACTION_SELL) ? "SELL"
                       : (action == REBAL_ACTION_TRIM) ? "TRIM"
                       : (action == REBAL_ACTION_FLIP) ? "FLIP"
                       :                                 "NONE";

    snprintf(out->debrief, sizeof(out->debrief),
        "%s %s %s: score %.2f -> %.2f (exit %.2f). "
        "Obscurity %.2f (driver: %s). Day %d/%d. "
        "ES_95 %.2f%%. Bot disagreement %.2f.",
        dec_str, act_str, sym,
        old_blend, new_blend, exit_threshold,
        obscurity, out->obscurity.primary_driver,
        days_held, hold_target_days,
        pos->heston_es_95 * 100.0,
        pos->bot_disagreement);

    return 0;
}
