"""
kseed_runner.py — Parallel multi-ensemble bot runner with per-ensemble
convergence and cross-ensemble disagreement scoring.

Two layers of variance reduction working together:

  1. *Within* an ensemble — wave-based firing with a Jaccard convergence
     stop-rule. After each wave of W bots, the orchestrator asks the C
     backend for the current top-K and compares it (Jaccard) to the
     previous wave's top-K. If similarity >= threshold for `stable_waves`
     consecutive checks, the ensemble stops spawning more bots. No
     wasted compute on a consensus that's already settled.

  2. *Across* ensembles — K ensembles run in parallel against one vLLM
     server, each with a different persona seed and sampling temperature.
     Final per-symbol pick rates across the K give us the per-stock
     disagreement variance feeding the obscurity score.

A single global semaphore caps total in-flight vLLM requests so K=4
ensembles with W=32 don't overrun the GPU. The semaphore size matches
bot_runner.MAX_CONCURRENT; vLLM's --max-num-seqs is the real ceiling.

Usage:
    python bots/kseed_runner.py --k 4 --wave-size 32 --n-bots-max 200 \\
        --apply-to-run-idx 0

  --apply-to-run-idx N    POST the cross-ensemble disagreement to
                          ranking_blend against the Nth ensemble's run_id.
"""

import argparse
import asyncio
import datetime
import json
import os
import statistics
import sys

from openai import AsyncOpenAI

from bot_runner import (
    BackendClient, BackendError, ProgressTracker,
    SYSTEM_INSTRUCTIONS, generate_personas, run_single_bot,
    DEFAULT_MODEL, VLLM_BASE_URL, VLLM_API_KEY, MAX_CONCURRENT,
    BACKEND_HOST, BACKEND_PORT,
)


DEFAULT_K                  = 4
DEFAULT_WAVE_SIZE          = 32
DEFAULT_N_BOTS_MAX         = 200
DEFAULT_TOP_K              = 20
DEFAULT_AGG_K              = 100    # how wide to pull per-run picks for disagreement
DEFAULT_JACCARD_THRESHOLD  = 0.9
DEFAULT_STABLE_WAVES       = 2
DEFAULT_TEMP_SCHEDULE      = [0.85, 0.95, 1.0, 1.05, 0.9, 1.0, 0.92, 1.02]
DEFAULT_OUT                = "kseed_disagreement.json"


def temp_for_idx(i):
    if i < len(DEFAULT_TEMP_SCHEDULE):
        return DEFAULT_TEMP_SCHEDULE[i]
    return 1.0


async def fetch_news_digest(host, port, news_as_of):
    """Crawl + build the digest once; all ensembles reuse it."""
    backend = BackendClient()
    await backend.connect(host, port)
    try:
        crawl = {"cmd": "crawl_news", "limit": 50}
        if news_as_of:
            crawl["cutoff_date"] = news_as_of
        await backend.send_command(crawl)
        digest_cmd = {"cmd": "get_news_digest", "max_chars": 32000, "days": 7}
        if news_as_of:
            digest_cmd["as_of"] = news_as_of
        resp = await backend.send_command(digest_cmd)
        return resp.get("digest", "") or ""
    finally:
        await backend.close()


async def fetch_sp500(host, port):
    backend = BackendClient()
    await backend.connect(host, port)
    try:
        resp = await backend.send_command({"cmd": "sp500_list"})
        return resp["tickers"]
    finally:
        await backend.close()


async def run_one_ensemble(*, idx, model, n_bots_max, wave_size,
                           top_k, jaccard_threshold, stable_waves,
                           tickers, system_prompt,
                           llm_client, semaphore,
                           seed, temperature, label_prefix,
                           host, port, hold_days):
    """
    One ensemble: fires bots in waves, checks convergence after each,
    stops early if the top-K settles. Returns:
        {"idx": ..., "run_id": ..., "n_bots": ..., "top_counts": {sym: count}}
    """
    backend = BackendClient()
    await backend.connect(host, port)

    label = f"{label_prefix}-{idx}-{datetime.date.today().isoformat()}"
    resp = await backend.send_command({
        "cmd": "bot_run_create",
        "label": label,
        "n_bots_target": n_bots_max,
        "hold_days": hold_days,
    })
    run_id = int(resp["run_id"])
    print(f"[ensemble {idx}] run_id={run_id}  seed={seed}  temp={temperature:.2f}")

    valid_set = set(tickers)
    personas = generate_personas(n_bots_max, seed=seed)
    progress = ProgressTracker(n_bots_max)

    bots_fired       = 0
    prev_top_symbols = []
    stable_count     = 0
    converged        = False

    try:
        while bots_fired < n_bots_max:
            wave_n = min(wave_size, n_bots_max - bots_fired)
            await asyncio.gather(*[
                run_single_bot(
                    backend, llm_client, semaphore, run_id,
                    bots_fired + j, personas[bots_fired + j],
                    valid_set, system_prompt, progress,
                    model=model, temperature=temperature,
                )
                for j in range(wave_n)
            ])
            bots_fired += wave_n

            check = await backend.send_command({
                "cmd": "convergence_check",
                "run_id": run_id,
                "k": top_k,
                "prev": prev_top_symbols,
                "threshold": jaccard_threshold,
            })
            cur_top = [e["symbol"] for e in check.get("top", [])]
            jac = check.get("jaccard", 0.0)

            if prev_top_symbols and check.get("stable"):
                stable_count += 1
            else:
                stable_count = 0
            prev_top_symbols = cur_top

            print(f"[ensemble {idx}] bots {bots_fired}/{n_bots_max}  "
                  f"jaccard={jac:.3f}  stable_in_row={stable_count}/"
                  f"{stable_waves}")

            if stable_count >= stable_waves:
                converged = True
                print(f"[ensemble {idx}] CONVERGED after {bots_fired} bots "
                      f"(saved {n_bots_max - bots_fired} calls)")
                break

        await backend.send_command({
            "cmd": "bot_run_finish",
            "run_id": run_id,
            "n_bots_actual": progress.success,
        })

        agg = await backend.send_command({
            "cmd": "aggregate_run",
            "run_id": run_id,
            "k": DEFAULT_AGG_K,
        })
        top_counts = {e["symbol"]: int(e["count"]) for e in agg.get("top", [])}

        return {
            "idx":         idx,
            "run_id":      run_id,
            "n_bots":      progress.success,
            "converged":   converged,
            "top_counts":  top_counts,
        }
    finally:
        await backend.close()


def compute_disagreement(per_run_counts, per_run_totals):
    """
    per_run_counts: list[dict[str, int]] - picks for each of K ensembles
    per_run_totals: list[int]            - n_bots per ensemble (denominator)
    Returns {symbol: coefficient-of-variation in [0, 1]}.
    """
    all_symbols = set()
    for d in per_run_counts:
        all_symbols.update(d.keys())

    eps = 1e-6
    disagreement = {}
    for sym in all_symbols:
        rates = []
        for counts, total in zip(per_run_counts, per_run_totals):
            denom = total if total > 0 else 1
            rates.append(counts.get(sym, 0) / denom)
        if len(rates) < 2:
            disagreement[sym] = 0.0
            continue
        mean = statistics.fmean(rates)
        stdev = statistics.pstdev(rates)
        cv = stdev / (mean + eps)
        disagreement[sym] = max(0.0, min(1.0, cv))
    return disagreement


async def post_ranking_blend(host, port, run_id, k, disagreement):
    backend = BackendClient()
    await backend.connect(host, port)
    try:
        return await backend.send_command({
            "cmd":          "ranking_blend",
            "run_id":       run_id,
            "k":            k,
            "horizon_days": 21,
            "disagreement": disagreement,
        })
    finally:
        await backend.close()


async def main(args):
    if not os.environ.get("ANTHROPIC_API_KEY") and args.base_url.startswith("http"):
        pass  # vLLM doesn't need a key; ignore the absence

    print(f"[kseed] LLM endpoint: {args.base_url}  model: {args.model}")
    print(f"[kseed] K={args.k}  wave_size={args.wave_size}  "
          f"n_bots_max={args.n_bots_max}  jaccard>={args.jaccard_threshold}")

    # ── Shared resources across all ensembles ─────────────────────
    llm_client = AsyncOpenAI(base_url=args.base_url, api_key=args.api_key)
    semaphore  = asyncio.Semaphore(args.max_concurrent)

    # Crawl news once; everyone reuses the digest.
    print(f"[kseed] Fetching S&P 500 list + news digest...")
    tickers = await fetch_sp500(args.host, args.port)
    digest  = await fetch_news_digest(args.host, args.port, args.news_as_of)
    print(f"[kseed] {len(tickers)} tickers, news digest = {len(digest)} chars")

    system_prompt = SYSTEM_INSTRUCTIONS + "\n\nVALID S&P 500 TICKERS:\n" + ", ".join(tickers)
    if digest:
        system_prompt += "\n\n" + digest

    # ── Launch K ensembles in parallel ────────────────────────────
    t0 = asyncio.get_event_loop().time()
    ensembles = await asyncio.gather(*[
        run_one_ensemble(
            idx               = i,
            model             = args.model,
            n_bots_max        = args.n_bots_max,
            wave_size         = args.wave_size,
            top_k             = args.top_k,
            jaccard_threshold = args.jaccard_threshold,
            stable_waves      = args.stable_waves,
            tickers           = tickers,
            system_prompt     = system_prompt,
            llm_client        = llm_client,
            semaphore         = semaphore,
            seed              = 42 + i,
            temperature       = temp_for_idx(i),
            label_prefix      = args.label_prefix,
            host              = args.host,
            port              = args.port,
            hold_days         = args.hold_days,
        )
        for i in range(args.k)
    ])
    elapsed = asyncio.get_event_loop().time() - t0

    print(f"\n[kseed] All {args.k} ensembles done in {elapsed:.1f}s")
    for e in ensembles:
        flag = "✓" if e["converged"] else " "
        print(f"  ensemble {e['idx']}  run_id={e['run_id']}  "
              f"bots={e['n_bots']}  converged={flag}")

    # ── Cross-ensemble disagreement variance ─────────────────────
    per_run_counts = [e["top_counts"] for e in ensembles]
    per_run_totals = [e["n_bots"]    for e in ensembles]
    disagreement   = compute_disagreement(per_run_counts, per_run_totals)

    high = sorted(disagreement.items(), key=lambda kv: -kv[1])[:10]
    low  = sorted(disagreement.items(), key=lambda kv:  kv[1])[:10]
    print(f"\n[kseed] Disagreement scored for {len(disagreement)} symbols")
    print("[kseed] Most consistent (low disagreement):")
    for sym, d in low:
        print(f"  {sym:<6s}  {d:.3f}")
    print("[kseed] Most variable (high disagreement):")
    for sym, d in high:
        print(f"  {sym:<6s}  {d:.3f}")

    out = {
        "run_ids":       [e["run_id"]   for e in ensembles],
        "n_bots":        [e["n_bots"]   for e in ensembles],
        "converged":     [e["converged"] for e in ensembles],
        "disagreement":  disagreement,
    }
    with open(args.out, "w") as f:
        json.dump(out, f, indent=2)
    print(f"\n[kseed] Wrote {args.out}")

    # ── Optional: apply to ranking_blend on one of the run_ids ────
    if args.apply_to_run_idx is not None:
        if 0 <= args.apply_to_run_idx < len(ensembles):
            target = ensembles[args.apply_to_run_idx]["run_id"]
            print(f"\n[kseed] POSTing disagreement to ranking_blend "
                  f"for run_id={target}")
            resp = await post_ranking_blend(args.host, args.port,
                                            target, args.top_k, disagreement)
            if resp and resp.get("type") == "ranking_blended":
                print(f"[kseed] Top {min(20, len(resp['ranked']))} blended:")
                for r in resp["ranked"][:20]:
                    print(f"  {r['rank']:2d}. {r['symbol']:<6s}  "
                          f"score {r['blended_score']:+.3f}  "
                          f"z_bot {r['z_bot']:+.2f}  "
                          f"z_heston {r['z_heston']:+.2f}  "
                          f"dis {r['bot_disagreement']:.2f}")
            else:
                print(f"[kseed] ranking_blend response: {resp}")
        else:
            print(f"[kseed] --apply-to-run-idx out of range, skipping")


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="Parallel multi-ensemble bot runner with convergence.")
    p.add_argument("--k",                 type=int,   default=DEFAULT_K)
    p.add_argument("--wave-size",         type=int,   default=DEFAULT_WAVE_SIZE)
    p.add_argument("--n-bots-max",        type=int,   default=DEFAULT_N_BOTS_MAX)
    p.add_argument("--top-k",             type=int,   default=DEFAULT_TOP_K)
    p.add_argument("--hold-days",         type=int,   default=21)
    p.add_argument("--jaccard-threshold", type=float, default=DEFAULT_JACCARD_THRESHOLD)
    p.add_argument("--stable-waves",      type=int,   default=DEFAULT_STABLE_WAVES,
                   help="Consecutive stable waves required before stopping.")
    p.add_argument("--max-concurrent",    type=int,   default=MAX_CONCURRENT,
                   help="Global semaphore cap on in-flight vLLM requests.")
    p.add_argument("--model",             default=DEFAULT_MODEL)
    p.add_argument("--base-url",          default=VLLM_BASE_URL)
    p.add_argument("--api-key",           default=VLLM_API_KEY)
    p.add_argument("--news-as-of",        default=None)
    p.add_argument("--host",              default=BACKEND_HOST)
    p.add_argument("--port",              type=int,   default=BACKEND_PORT)
    p.add_argument("--out",               default=DEFAULT_OUT)
    p.add_argument("--label-prefix",      default="parallel-kseed")
    p.add_argument("--apply-to-run-idx",  type=int,   default=None,
                   help="If set, POST disagreement to ranking_blend "
                        "for the Nth ensemble's run_id.")
    return p.parse_args(argv)


if __name__ == "__main__":
    asyncio.run(main(parse_args()))
