#ifndef SP500_UNIVERSE_H
#define SP500_UNIVERSE_H

#include <stddef.h>

/*
 * sp500_universe  –  Static list of S&P 500 tickers used as the bot
 *                    pick universe and the backtest candidate pool.
 *
 * The list is hard-coded in sp500_universe.c. To update it, regenerate
 * the array from a current S&P 500 constituents source.
 *
 * Note: this is a *current* snapshot, which introduces survivorship bias
 * for any backtest that crosses constituent changes. Acknowledge this
 * limitation in NOTES.md and your project writeup.
 */

/* Returns pointer to internal array of NUL-terminated ticker strings. */
const char *const *sp500_tickers(void);

/* Number of tickers in the array. */
size_t sp500_count(void);

/*
 * Returns 1 if `symbol` is in the S&P 500 list, else 0.
 * Case-sensitive (use uppercase tickers).
 */
int sp500_contains(const char *symbol);

#endif /* SP500_UNIVERSE_H */
