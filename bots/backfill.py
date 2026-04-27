"""
backfill.py — prime the price_cache with daily bars for all S&P 500 tickers
plus SPY over a chosen date window.

Throttled to respect Polygon's 5 req/min free tier (one request every 13s by
default). Override with --rps if you have a paid plan. Empty responses are
retried because Polygon's rate-limiter sometimes returns 200 with no bars
instead of 429.

Usage:
    python bots/backfill.py 2025-09-01 2025-11-08
    python bots/backfill.py 2025-06-15 2025-12-15 --rps 10   # paid tier
"""

import argparse
import asyncio
import json
import sys
import time


BACKEND_HOST = "127.0.0.1"
BACKEND_PORT = 8765

# Free-tier safe default: 5 requests/min ⇒ 1 every 13s (slack for clock skew).
DEFAULT_GAP_SEC = 13.0
EMPTY_RETRIES = 2
EMPTY_BACKOFF_SEC = 30.0


async def send_command(reader, writer, lock, cmd):
    async with lock:
        writer.write((json.dumps(cmd) + "\n").encode())
        await writer.drain()
        line = await reader.readline()
        if not line:
            raise RuntimeError("connection closed")
        return json.loads(line.decode())


async def fetch_history(reader, writer, lock, symbol, start, end):
    """Returns (n_bars, error_str_or_None). Retries empty responses."""
    for attempt in range(EMPTY_RETRIES + 1):
        try:
            resp = await send_command(reader, writer, lock, {
                "cmd": "history",
                "symbol": symbol,
                "multiplier": 1,
                "timespan": "day",
                "from": start,
                "to": end,
            })
            n = len(resp.get("bars", [])) if resp.get("type") == "history" else 0
            if n > 0 or attempt == EMPTY_RETRIES:
                return n, None
            # Empty + retries left → wait and retry (likely 200 w/ rate-limit body)
            await asyncio.sleep(EMPTY_BACKOFF_SEC)
        except Exception as e:
            return 0, str(e)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("start", help="YYYY-MM-DD")
    ap.add_argument("end",   help="YYYY-MM-DD")
    ap.add_argument("--rps", type=float, default=None,
                    help="Requests per second (default: ~5/min for free tier)")
    args = ap.parse_args()

    gap_sec = (1.0 / args.rps) if args.rps else DEFAULT_GAP_SEC

    reader, writer = await asyncio.open_connection(BACKEND_HOST, BACKEND_PORT)
    lock = asyncio.Lock()

    resp = await send_command(reader, writer, lock, {"cmd": "sp500_list"})
    tickers = resp["tickers"]
    print(f"[backfill] Fetched {len(tickers)} S&P 500 tickers", flush=True)

    all_symbols = tickers + ["SPY"]
    total = len(all_symbols)
    eta_min = total * gap_sec / 60
    print(f"[backfill] Fetching {args.start} → {args.end} for {total} symbols "
          f"({gap_sec:.1f}s/req, ETA ~{eta_min:.0f} min)...", flush=True)

    ok = empty = failed = 0
    t0 = time.time()
    for i, sym in enumerate(all_symbols, 1):
        n, err = await fetch_history(reader, writer, lock, sym, args.start, args.end)
        if err is not None:
            failed += 1
            tag = f"FAIL ({err})"
        elif n == 0:
            empty += 1
            tag = "empty"
        else:
            ok += 1
            tag = f"{n} bars"
        elapsed = time.time() - t0
        rate = i / elapsed if elapsed > 0 else 0
        print(f"  [{i:3d}/{total}] {sym:<6s} {tag:<14s}  "
              f"({rate*60:.1f}/min, ok={ok} empty={empty} fail={failed})",
              flush=True)
        # Throttle (skip after the last call)
        if i < total:
            await asyncio.sleep(gap_sec)

    print(f"[backfill] DONE — {ok} ok, {empty} empty, {failed} failed "
          f"in {(time.time()-t0)/60:.1f} min", flush=True)

    writer.close()
    await writer.wait_closed()


if __name__ == "__main__":
    asyncio.run(main())
