"""
backfill.py — prime the price_cache with daily bars for all S&P 500 tickers
plus SPY over a chosen date window.

Usage:
    python bots/backfill.py 2025-09-01 2025-11-08
"""

import asyncio
import json
import sys


BACKEND_HOST = "127.0.0.1"
BACKEND_PORT = 8765
CONCURRENCY = 8


async def send_command(reader, writer, lock, cmd):
    async with lock:
        writer.write((json.dumps(cmd) + "\n").encode())
        await writer.drain()
        line = await reader.readline()
        if not line:
            raise RuntimeError("connection closed")
        return json.loads(line.decode())


async def fetch_history(reader, writer, lock, semaphore, symbol, start, end):
    async with semaphore:
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
            return symbol, n, None
        except Exception as e:
            return symbol, 0, str(e)


async def main():
    if len(sys.argv) != 3:
        print("Usage: python backfill.py YYYY-MM-DD YYYY-MM-DD", file=sys.stderr)
        sys.exit(1)

    start, end = sys.argv[1], sys.argv[2]

    reader, writer = await asyncio.open_connection(BACKEND_HOST, BACKEND_PORT)
    lock = asyncio.Lock()

    resp = await send_command(reader, writer, lock, {"cmd": "sp500_list"})
    tickers = resp["tickers"]
    print(f"[backfill] Fetched {len(tickers)} S&P 500 tickers")

    # Include SPY for benchmark
    all_symbols = tickers + ["SPY"]
    semaphore = asyncio.Semaphore(CONCURRENCY)

    print(f"[backfill] Fetching {start} → {end} for {len(all_symbols)} symbols...")
    results = await asyncio.gather(*[
        fetch_history(reader, writer, lock, semaphore, s, start, end)
        for s in all_symbols
    ])

    ok = sum(1 for _, n, _ in results if n > 0)
    empty = sum(1 for _, n, e in results if n == 0 and e is None)
    failed = [(s, e) for s, n, e in results if e is not None]

    print(f"[backfill] {ok} ok, {empty} empty, {len(failed)} failed")
    for s, e in failed[:10]:
        print(f"  FAIL {s}: {e}")

    writer.close()
    await writer.wait_closed()


if __name__ == "__main__":
    asyncio.run(main())
