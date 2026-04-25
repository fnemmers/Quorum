"""
bot_runner.py — 500 Claude Haiku bots pick S&P 500 stocks.

Usage:
    ANTHROPIC_API_KEY=... python bots/bot_runner.py

Requires the C backend running on localhost:8765.
"""

import asyncio
import datetime
import itertools
import json
import os
import random
import re
import sys
import time

import anthropic

from budget import BudgetTracker, BudgetExceeded, estimate_run_cost

BACKEND_HOST = "127.0.0.1"
BACKEND_PORT = 8765
N_BOTS = 5
HOLD_DAYS = 30
# Rolling 30-day windows. Each is a separate backtest of the SAME consensus
# picks against a different start date. We don't re-run the bots per window
# because Polygon news is "now" only — re-running would feed identical context
# and produce ~identical picks. Reusing one set of picks tests robustness of
# the consensus across market regimes.
BACKTEST_WINDOWS = [
    "2025-07-01",
    "2025-08-01",
    "2025-09-01",
    "2025-10-01",
    "2025-11-01",
]
TOP_K = 20
MAX_CONCURRENT = 30
MAX_RETRIES = 3
CLAUDE_MODEL = "claude-haiku-4-5-20251001"
MAX_TOKENS = 300

SYSTEM_INSTRUCTIONS = """\
You are a stock market analyst participating in an ensemble forecasting experiment.
You will be given an investing persona and a list of valid S&P 500 tickers.
Your task: select approximately 10 stocks that you believe will outperform over the next 30 days.

RULES:
- Pick ONLY from the provided ticker list.
- Return EXACTLY a JSON array of ticker strings, nothing else.
- Pick between 8 and 12 tickers.
- Example response: ["AAPL", "MSFT", "NVDA", "JPM", "UNH", "LLY", "AMZN", "GOOGL", "MA", "V"]"""

ARCHETYPES = [
    ("value", "You are a deep value investor. You hunt for stocks trading below intrinsic value — low P/E, low P/B, high free cash flow yield. You avoid hype and prefer boring, undervalued companies with margin of safety."),
    ("growth", "You are a growth investor. You seek companies with accelerating revenue growth, expanding TAM, and strong competitive positioning. You're willing to pay a premium for compounding potential."),
    ("momentum", "You are a momentum trader. You follow price trends — stocks making new highs, breaking out of bases, with strong relative strength. You believe winners keep winning in the near term."),
    ("contrarian", "You are a contrarian mean-reversion investor. You look for oversold stocks, beaten-down names with negative sentiment that you believe the market has over-punished. You buy fear."),
    ("dividend", "You are an income/dividend investor. You favor companies with high and sustainable dividend yields, long payout histories, and strong cash generation to support distributions."),
    ("quality", "You are a quality/moat investor. You seek companies with durable competitive advantages — high ROIC, strong brands, network effects, switching costs. Quality compounders only."),
    ("garp", "You are a GARP investor (Growth at a Reasonable Price). You want growth but not at any price — you use PEG ratios and look for reasonable valuations relative to earnings growth."),
    ("smallcap", "You focus on the smaller constituents within the S&P 500. You believe the less-covered, lower-market-cap names in the index offer more upside and less analyst crowding."),
    ("technical", "You are a quantitative/technical analyst. You rely on chart patterns, moving averages, RSI, MACD, and volume analysis. Fundamentals are secondary to price action signals."),
    ("macro", "You are a top-down macro investor. You start with the economic cycle, interest rate outlook, sector rotation patterns, and geopolitical risks, then pick stocks that benefit from your macro thesis."),
]

SECTORS = [
    ("tech", "with a focus on the Technology sector"),
    ("healthcare", "with a focus on the Healthcare sector"),
    ("financials", "with a focus on the Financials sector"),
    ("consumer_disc", "with a focus on the Consumer Discretionary sector"),
    ("consumer_staples", "with a focus on the Consumer Staples sector"),
    ("industrials", "with a focus on the Industrials sector"),
    ("energy", "with a focus on the Energy sector"),
    ("utilities", "with a focus on the Utilities sector"),
    ("real_estate", "with a focus on the Real Estate sector"),
    ("materials", "with a focus on the Materials sector"),
    ("comm_services", "with a focus on the Communication Services sector"),
    ("any", "across all sectors"),
]

RISK_LEVELS = [
    ("conservative", "You are conservative and risk-averse — you prefer stability and downside protection over maximum upside."),
    ("moderate", "You have a moderate risk tolerance — you balance upside potential with reasonable downside protection."),
    ("aggressive", "You are aggressive and high-conviction — you concentrate bets on your best ideas and tolerate volatility."),
    ("balanced", "You are a balanced allocator — you diversify across risk levels, mixing safe havens with higher-beta names."),
]

HORIZONS = [
    ("short", "You focus on short-term catalysts: upcoming earnings, product launches, regulatory decisions, and news flow."),
    ("medium", "You focus on medium-term trends: multi-month sector rotation, business cycle positioning, and emerging narratives."),
    ("long", "You focus on long-term fundamentals: durable business quality, secular growth tailwinds, and multi-year compounding."),
]


def generate_personas(n):
    all_combos = list(itertools.product(ARCHETYPES, SECTORS, RISK_LEVELS, HORIZONS))
    random.seed(42)
    random.shuffle(all_combos)
    selected = all_combos[:n]

    personas = []
    for (arch_key, arch_desc), (sect_key, sect_desc), (risk_key, risk_desc), (hor_key, hor_desc) in selected:
        name = f"{arch_key}-{sect_key}-{risk_key}-{hor_key}"
        description = f"{arch_desc} {sect_desc}. {risk_desc} {hor_desc}"
        personas.append({"name": name, "description": description})
    return personas


def extract_tickers(response_text, valid_tickers):
    text = response_text.strip()

    try:
        parsed = json.loads(text)
        if isinstance(parsed, list):
            return [t for t in parsed if isinstance(t, str) and t in valid_tickers]
    except json.JSONDecodeError:
        pass

    match = re.search(r"\[([^\]]+)\]", text)
    if match:
        try:
            parsed = json.loads(match.group(0))
            if isinstance(parsed, list):
                return [t for t in parsed if isinstance(t, str) and t in valid_tickers]
        except json.JSONDecodeError:
            pass

    candidates = re.findall(r"\b([A-Z]{1,5})\b", text)
    seen = set()
    deduped = []
    for t in candidates:
        if t in valid_tickers and t not in seen:
            seen.add(t)
            deduped.append(t)
    return deduped


class BackendError(Exception):
    pass


class BackendClient:
    def __init__(self):
        self._reader = None
        self._writer = None
        self._lock = asyncio.Lock()

    async def connect(self, host=BACKEND_HOST, port=BACKEND_PORT):
        self._reader, self._writer = await asyncio.open_connection(host, port)

    async def send_command(self, cmd):
        async with self._lock:
            data = json.dumps(cmd) + "\n"
            self._writer.write(data.encode())
            await self._writer.drain()
            line = await self._reader.readline()
            if not line:
                raise BackendError("connection closed")
            resp = json.loads(line.decode())
            if resp.get("type") == "error":
                raise BackendError(resp.get("message", "unknown error"))
            return resp

    async def close(self):
        if self._writer:
            self._writer.close()
            await self._writer.wait_closed()


class ProgressTracker:
    def __init__(self, total):
        self.total = total
        self.done = 0
        self.success = 0
        self.failed = 0
        self._lock = asyncio.Lock()

    async def record(self, ok):
        async with self._lock:
            self.done += 1
            if ok:
                self.success += 1
            else:
                self.failed += 1
            if self.done % 50 == 0 or self.done == self.total:
                pct = self.done / self.total * 100
                print(f"[bot_runner] Progress: {self.done}/{self.total} ({pct:.0f}%) "
                      f"— {self.success} success, {self.failed} failed")


async def call_claude(client, persona, system_prompt, semaphore, budget=None):
    user_msg = f"{persona['description']}\n\nSelect ~10 tickers. Respond with ONLY a JSON array."

    for attempt in range(MAX_RETRIES):
        try:
            async with semaphore:
                response = await client.messages.create(
                    model=CLAUDE_MODEL,
                    max_tokens=MAX_TOKENS,
                    system=[
                        {
                            "type": "text",
                            "text": system_prompt,
                            "cache_control": {"type": "ephemeral"},
                        }
                    ],
                    messages=[{"role": "user", "content": user_msg}],
                )
            if budget is not None:
                budget.record(getattr(response, "usage", None))
            return response.content[0].text
        except anthropic.RateLimitError:
            if attempt < MAX_RETRIES - 1:
                wait = 2 ** (attempt + 1)
                print(f"[bot_runner] Rate limited, retrying in {wait}s...")
                await asyncio.sleep(wait)
            else:
                raise
        except anthropic.APIStatusError as e:
            if e.status_code >= 500 and attempt < MAX_RETRIES - 1:
                wait = 2 ** (attempt + 1)
                print(f"[bot_runner] Server error {e.status_code}, retrying in {wait}s...")
                await asyncio.sleep(wait)
            else:
                raise


async def run_single_bot(backend, claude_client, semaphore, run_id,
                         bot_index, persona, valid_tickers, system_prompt,
                         progress, budget=None):
    try:
        raw = await call_claude(claude_client, persona, system_prompt,
                                semaphore, budget=budget)
        picks = extract_tickers(raw, valid_tickers)
        if not picks:
            await progress.record(False)
            return

        await backend.send_command({
            "cmd": "bot_picks_ingest",
            "run_id": run_id,
            "bot_index": bot_index,
            "persona": persona["name"],
            "picks": picks,
        })
        await progress.record(True)
    except Exception as e:
        print(f"[bot_runner] Bot {bot_index} failed: {e}")
        await progress.record(False)


async def main():
    api_key = os.environ.get("ANTHROPIC_API_KEY")
    if not api_key:
        print("Error: ANTHROPIC_API_KEY environment variable not set.", file=sys.stderr)
        sys.exit(1)

    budget = BudgetTracker()
    print(f"[bot_runner] {budget.summary()}")
    estimated = estimate_run_cost(N_BOTS)
    print(f"[bot_runner] Estimated cost for this run: ${estimated:.4f}")
    try:
        budget.preflight(estimated)
    except BudgetExceeded as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(2)

    backend = BackendClient()
    try:
        await backend.connect()
    except ConnectionRefusedError:
        print(f"Error: cannot connect to backend on {BACKEND_HOST}:{BACKEND_PORT}. "
              "Is the C backend running?", file=sys.stderr)
        sys.exit(1)
    print(f"[bot_runner] Connected to backend on {BACKEND_HOST}:{BACKEND_PORT}")

    resp = await backend.send_command({"cmd": "sp500_list"})
    tickers = resp["tickers"]
    valid_set = set(tickers)
    print(f"[bot_runner] Fetched {len(tickers)} S&P 500 tickers")

    label = f"haiku-run-{datetime.date.today().isoformat()}"
    resp = await backend.send_command({
        "cmd": "bot_run_create",
        "label": label,
        "n_bots_target": N_BOTS,
        "hold_days": HOLD_DAYS,
    })
    run_id = int(resp["run_id"])
    print(f"[bot_runner] Created run #{run_id} (label: {label})")

    # Crawl fresh news and build a market briefing digest
    print("[bot_runner] Crawling recent news...")
    try:
        await backend.send_command({"cmd": "crawl_news", "limit": 50})
        digest_resp = await backend.send_command({
            "cmd": "get_news_digest",
            "max_chars": 32000,
            "days": 7,
        })
        news_digest = digest_resp.get("digest", "")
        print(f"[bot_runner] Got news digest: {len(news_digest)} chars")
    except Exception as e:
        print(f"[bot_runner] News crawl failed ({e}), continuing without news context")
        news_digest = ""

    system_prompt = SYSTEM_INSTRUCTIONS + "\n\nVALID S&P 500 TICKERS:\n" + ", ".join(tickers)
    if news_digest:
        system_prompt += "\n\n" + news_digest

    personas = generate_personas(N_BOTS)
    claude_client = anthropic.AsyncAnthropic()
    semaphore = asyncio.Semaphore(MAX_CONCURRENT)
    progress = ProgressTracker(N_BOTS)

    print(f"[bot_runner] Starting {N_BOTS} bot calls (concurrency: {MAX_CONCURRENT})...")
    t0 = time.time()

    await asyncio.gather(*[
        run_single_bot(backend, claude_client, semaphore, run_id,
                       i, personas[i], valid_set, system_prompt, progress,
                       budget=budget)
        for i in range(N_BOTS)
    ])

    elapsed = time.time() - t0
    print(f"[bot_runner] All bots complete in {elapsed:.1f}s: "
          f"{progress.success}/{N_BOTS} succeeded")
    print(f"[bot_runner] {budget.summary()}")

    await backend.send_command({
        "cmd": "bot_run_finish",
        "run_id": run_id,
        "n_bots_actual": progress.success,
    })

    agg = await backend.send_command({
        "cmd": "aggregate_run",
        "run_id": run_id,
        "k": TOP_K,
    })

    print(f"\n{'=' * 50}")
    print(f" TOP {TOP_K} CONSENSUS PICKS ({agg['n_picks_total']} total picks)")
    print(f"{'=' * 50}")
    for i, entry in enumerate(agg["top"], 1):
        pct = entry["count"] / progress.success * 100 if progress.success else 0
        print(f"  {i:2d}. {entry['symbol']:<6s} — {entry['count']} bots ({pct:.1f}%)")

    print(f"\n[bot_runner] Running rolling {HOLD_DAYS}-day backtests "
          f"across {len(BACKTEST_WINDOWS)} windows...")

    results = []
    for start in BACKTEST_WINDOWS:
        end = (datetime.date.fromisoformat(start)
               + datetime.timedelta(days=HOLD_DAYS)).isoformat()
        try:
            bt = await backend.send_command({
                "cmd": "backtest_run",
                "run_id": run_id,
                "k": TOP_K,
                "start_date": start,
                "hold_days": HOLD_DAYS,
            })
            results.append(bt)
            print(f"  {start} → {end}: "
                  f"port {bt['port_return']:+6.2f}%  "
                  f"SPY {bt['bench_return']:+6.2f}%  "
                  f"α {bt['alpha']:+6.2f}%  "
                  f"Sharpe {bt['sharpe']:+5.2f}")
        except BackendError as e:
            print(f"  {start} → {end}: failed ({e})")

    if results:
        wins  = sum(1 for r in results if r["alpha"] > 0)
        avg_a = sum(r["alpha"]        for r in results) / len(results)
        avg_p = sum(r["port_return"]  for r in results) / len(results)
        avg_b = sum(r["bench_return"] for r in results) / len(results)
        avg_s = sum(r["sharpe"]       for r in results) / len(results)
        print(f"\n{'=' * 60}")
        print(f" ROLLING BACKTEST SUMMARY ({len(results)} windows)")
        print(f"{'=' * 60}")
        print(f"  Avg portfolio return: {avg_p:+.2f}%")
        print(f"  Avg benchmark (SPY):  {avg_b:+.2f}%")
        print(f"  Avg alpha:            {avg_a:+.2f}%")
        print(f"  Avg Sharpe:           {avg_s:+.2f}")
        print(f"  Beat SPY:             {wins}/{len(results)} windows "
              f"({wins / len(results) * 100:.0f}%)")

    await backend.close()
    print("\n[bot_runner] Done.")


if __name__ == "__main__":
    asyncio.run(main())
