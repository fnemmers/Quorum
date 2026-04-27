"""
bot_runner_backtest.py — backtest-mode wrapper around bot_runner.main().

Configures a date-bounded news crawl (--as-of) and rolling backtest windows
that start *after* that cutoff. The model itself defaults to Haiku 4.5 — as
of 2026 the older Claude 3.x family is retired from the API, so Haiku 4.5
(cutoff ~mid 2025) is the oldest cheap model still callable. Backtests
starting from Aug 2025 onward remain meaningfully out-of-sample.

Usage:
    ANTHROPIC_API_KEY=... python bots/bot_runner_backtest.py \\
        --as-of 2025-08-01 --rolling 3 --n-bots 5

Anything not specified falls back to bot_runner.py defaults.
"""

import argparse
import asyncio
import datetime
import sys

from bot_runner import main, DEFAULT_HOLD_DAYS, DEFAULT_N_BOTS, DEFAULT_TOP_K
from budget import PRICES_BY_MODEL


BACKTEST_DEFAULT_MODEL = "claude-haiku-4-5-20251001"


def rolling_windows(start_date, n_windows, step_days=30):
    """Generate `n_windows` start dates spaced step_days apart starting at start_date."""
    base = datetime.date.fromisoformat(start_date)
    return [(base + datetime.timedelta(days=step_days * i)).isoformat()
            for i in range(n_windows)]


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="Backtest-mode bot ensemble run.")
    p.add_argument("--model", default=BACKTEST_DEFAULT_MODEL,
                   choices=sorted(PRICES_BY_MODEL.keys()),
                   help=f"Older model with pre-cutoff knowledge "
                        f"(default: {BACKTEST_DEFAULT_MODEL})")
    p.add_argument("--as-of", required=True,
                   help="YYYY-MM-DD: simulated knowledge cutoff. News digest "
                        "is bounded to this date and the first backtest window "
                        "starts the day after if --backtest-start is omitted.")
    p.add_argument("--backtest-start", default=None,
                   help="YYYY-MM-DD start of the first backtest window "
                        "(default: day after --as-of).")
    p.add_argument("--hold-days", type=int, default=DEFAULT_HOLD_DAYS)
    p.add_argument("--rolling",   type=int, default=5,
                   help="Number of rolling backtest windows (default 5).")
    p.add_argument("--step-days", type=int, default=30,
                   help="Spacing between rolling window starts (default 30).")
    p.add_argument("--n-bots", type=int, default=DEFAULT_N_BOTS)
    p.add_argument("--top-k",  type=int, default=DEFAULT_TOP_K)
    return p.parse_args(argv)


def main_cli(argv=None):
    args = parse_args(argv)

    # Validate dates and derive backtest_start.
    try:
        as_of = datetime.date.fromisoformat(args.as_of)
    except ValueError:
        print(f"Error: --as-of must be YYYY-MM-DD, got {args.as_of!r}",
              file=sys.stderr)
        sys.exit(1)

    backtest_start = args.backtest_start or (as_of + datetime.timedelta(days=1)).isoformat()
    if backtest_start <= args.as_of:
        print(f"Error: --backtest-start ({backtest_start}) must be AFTER "
              f"--as-of ({args.as_of}) — otherwise the backtest overlaps the "
              f"news/knowledge cutoff and is biased.", file=sys.stderr)
        sys.exit(2)

    windows = rolling_windows(backtest_start, args.rolling, args.step_days)

    print(f"[backtest] model={args.model}  as_of={args.as_of}")
    print(f"[backtest] {args.rolling} rolling {args.hold_days}-day windows "
          f"starting {windows[0]} → ending {windows[-1]} + {args.hold_days}d")

    asyncio.run(main(
        model=args.model,
        n_bots=args.n_bots,
        hold_days=args.hold_days,
        top_k=args.top_k,
        news_as_of=args.as_of,
        backtest_windows=windows,
        label_prefix=f"backtest-{args.model.split('-')[1]}",
    ))


if __name__ == "__main__":
    main_cli()
