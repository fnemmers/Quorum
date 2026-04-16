/*
 * convergence.c — YOU WRITE THIS (small)
 * ─────────────────────────────────────────────────────────────────────────────
 * Decides whether the bot ensemble has stabilized so the orchestrator can stop.
 *
 * Just two functions and they're both short. Don't overthink it.
 *
 * ─── DESIGN POINTER FOR JACCARD ──────────────────────────────────────────────
 *
 * Jaccard ignores counts entirely — it's a set comparison.
 *
 *   Build set A = {a[0].symbol, a[1].symbol, ..., a[n_a-1].symbol}
 *   Build set B = {b[0].symbol, ...}
 *   intersection = number of symbols present in both
 *   union        = n_a + n_b - intersection
 *   return intersection / union   (as a double)
 *
 * "Build set" can just be a nested loop since these arrays are tiny (k=20):
 *
 *   int inter = 0;
 *   for (int i = 0; i < n_a; i++)
 *       for (int j = 0; j < n_b; j++)
 *           if (strcmp(a[i].symbol, b[j].symbol) == 0) { inter++; break; }
 *
 * That's O(n_a * n_b) = 400 strcmps for k=20. Don't bother with a hash set —
 * it would take longer to write than the savings.
 *
 * Edge cases:
 *   - n_a == 0 && n_b == 0 → return 0.0 (or 1.0 — your call, but the header
 *     promises 0.0 for empty inputs, so do that).
 *   - n_a + n_b - inter == 0 → return 0.0 (avoid div by zero)
 *
 * ─── convergence_is_stable ───────────────────────────────────────────────────
 *
 * Literally one line: return convergence_jaccard(a, n_a, b, n_b) >= threshold.
 *
 * The orchestrator-side logic ("call this every K bots, stop when stable for
 * 3 consecutive checks") lives in TypeScript, not here. This file just gives
 * it the primitive.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "convergence.h"
#include <string.h>

double convergence_jaccard(const AggResult *a, int n_a,
                           const AggResult *b, int n_b) {
 
    int inter = 0;
    /* TODO(you): O(n_a * n_b) intersection, return ratio. */
    if(a == NULL || b == NULL || n_a <= 0 || n_b <= 0){
        return 0.0;
    }
    for (int i = 0; i < n_a; i++) {
        for (int j = 0; j < n_b; j++) {
            if (strcmp(a[i].symbol, b[j].symbol) == 0) {
                inter++;
                break;
            }
        }
    }
    int uni = n_a + n_b - inter;
    if (uni <= 0) {
        return 0.0;
    }

    return (double) inter / (double) uni;
}

int convergence_is_stable(const AggResult *a, int n_a,
                          const AggResult *b, int n_b,
                          double threshold) {
    return convergence_jaccard(a, n_a, b, n_b) >= threshold;
}
