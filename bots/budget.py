"""
budget.py — persistent monthly budget tracker for Claude API spend.

Hard $50/month cap. Refuses to run if estimated cost would exceed remaining
budget. State persisted to bots/budget.json between runs.

Pricing is selected per-model from PRICES_BY_MODEL. Costs are quoted per
1M tokens.
"""

import datetime
import json
import os
from pathlib import Path
from threading import Lock

MONTHLY_CAP_USD = 50.0

# Per-model pricing (USD per 1M tokens). Keys are exact Anthropic model IDs.
# Only currently-callable models are listed — Claude 3 / 3.5 families were
# retired from the API by 2026. Run `curl https://api.anthropic.com/v1/models`
# to confirm what your account can call.
PRICES_BY_MODEL = {
    "claude-haiku-4-5-20251001": {  # cheapest available — recommended for ensembles
        "input":       1.00,
        "cache_write": 1.25,
        "cache_read":  0.10,
        "output":      5.00,
    },
    "claude-sonnet-4-5-20250929": {  # higher-quality, ~3× cost of Haiku
        "input":       3.00,
        "cache_write": 3.75,
        "cache_read":  0.30,
        "output":     15.00,
    },
}

DEFAULT_MODEL = "claude-haiku-4-5-20251001"

BUDGET_FILE = Path(__file__).parent / "budget.json"


def _prices_for(model_id):
    if model_id not in PRICES_BY_MODEL:
        raise ValueError(
            f"No pricing entry for model {model_id!r}. "
            f"Add it to PRICES_BY_MODEL in budget.py."
        )
    return PRICES_BY_MODEL[model_id]


def _current_month():
    return datetime.date.today().strftime("%Y-%m")


def compute_cost_usd(usage, model_id=DEFAULT_MODEL):
    """Convert an Anthropic Usage object (or dict) into USD spent."""
    if usage is None:
        return 0.0
    p = _prices_for(model_id)
    get = usage.get if isinstance(usage, dict) else lambda k, d=0: getattr(usage, k, d) or 0

    input_tokens   = get("input_tokens", 0)
    output_tokens  = get("output_tokens", 0)
    cache_creation = get("cache_creation_input_tokens", 0)
    cache_read     = get("cache_read_input_tokens", 0)

    cost = (
        input_tokens   * p["input"]
        + cache_creation * p["cache_write"]
        + cache_read     * p["cache_read"]
        + output_tokens  * p["output"]
    ) / 1_000_000.0
    return cost


class BudgetExceeded(Exception):
    pass


class BudgetTracker:
    """Thread/async-safe persistent budget ledger."""

    def __init__(self, cap_usd=MONTHLY_CAP_USD, path=BUDGET_FILE,
                 model_id=DEFAULT_MODEL):
        self.cap_usd = cap_usd
        self.path = Path(path)
        self.model_id = model_id
        _prices_for(model_id)  # validates early
        self._lock = Lock()
        self._load()

    def _load(self):
        month = _current_month()
        if self.path.exists():
            try:
                data = json.loads(self.path.read_text())
                if data.get("month") == month:
                    self.month = month
                    self.spent_usd = float(data.get("spent_usd", 0.0))
                    self.n_calls = int(data.get("n_calls", 0))
                    return
            except (json.JSONDecodeError, ValueError):
                pass
        self.month = month
        self.spent_usd = 0.0
        self.n_calls = 0
        self._save()

    def _save(self):
        tmp = self.path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps({
            "month": self.month,
            "spent_usd": round(self.spent_usd, 6),
            "n_calls": self.n_calls,
            "cap_usd": self.cap_usd,
            "updated_at": datetime.datetime.now().isoformat(timespec="seconds"),
        }, indent=2))
        os.replace(tmp, self.path)

    @property
    def remaining(self):
        return max(0.0, self.cap_usd - self.spent_usd)

    def preflight(self, estimated_cost_usd):
        """Raise BudgetExceeded if this run would blow the cap."""
        with self._lock:
            self._load()  # re-read in case another process wrote
            if self.spent_usd + estimated_cost_usd > self.cap_usd:
                raise BudgetExceeded(
                    f"Would exceed ${self.cap_usd:.2f}/month cap: "
                    f"spent ${self.spent_usd:.4f}, estimated ${estimated_cost_usd:.4f}, "
                    f"remaining ${self.remaining:.4f}."
                )

    def record(self, usage):
        """Add one API call's usage to the ledger. Returns cost_usd."""
        cost = compute_cost_usd(usage, self.model_id)
        with self._lock:
            self.spent_usd += cost
            self.n_calls += 1
            self._save()
        return cost

    def summary(self):
        return (f"Budget: ${self.spent_usd:.4f} / ${self.cap_usd:.2f} spent "
                f"this month ({self.month}), {self.n_calls} calls, "
                f"${self.remaining:.4f} remaining "
                f"[{self.model_id}].")


def estimate_run_cost(n_bots, model_id=DEFAULT_MODEL,
                      avg_input_tokens=3500, avg_output_tokens=100,
                      cache_hit_fraction=0.95):
    """
    Rough pre-flight estimate for a bot_runner run.

    Most of the input is the S&P 500 ticker list in the cached system prompt.
    After the first call writes the cache, subsequent calls pay cache_read
    rate for ~95% of input tokens.
    """
    p = _prices_for(model_id)
    first_call_input = avg_input_tokens * p["cache_write"] / 1_000_000
    cached_input = (
        avg_input_tokens * cache_hit_fraction * p["cache_read"]
        + avg_input_tokens * (1 - cache_hit_fraction) * p["input"]
    ) / 1_000_000
    per_call_output = avg_output_tokens * p["output"] / 1_000_000

    total = first_call_input + (n_bots - 1) * cached_input + n_bots * per_call_output
    return total
