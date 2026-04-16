#ifndef CONVERGENCE_H
#define CONVERGENCE_H

#include "aggregation.h"

/*
 * convergence  –  Decides whether the bot ensemble has "stabilized" so
 *                 the orchestrator can stop spawning more bots and save
 *                 API budget.
 *
 * Idea: every K bots, snapshot the current top-20 from the aggregator.
 * Compare two snapshots. If the Jaccard similarity of their ticker
 * sets is above a threshold (e.g. 0.9) for several windows in a row,
 * we say the run has converged and the orchestrator should stop.
 *
 * This file is small but conceptually meaningful — when an interviewer
 * asks "how did you decide how many bots to run?" you have a real answer.
 */

/*
 * Jaccard similarity of two top-K result lists, ignoring counts.
 * Returns a value in [0.0, 1.0]. Empty inputs return 0.0.
 *
 *     |A ∩ B|
 *     ───────
 *     |A ∪ B|
 */
double convergence_jaccard(const AggResult *a, int n_a,
                           const AggResult *b, int n_b);

/*
 * Convenience: returns 1 if jaccard(a, b) >= threshold, else 0.
 */
int convergence_is_stable(const AggResult *a, int n_a,
                          const AggResult *b, int n_b,
                          double threshold);

#endif /* CONVERGENCE_H */
