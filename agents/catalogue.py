"""The strategy catalogue an agent may choose from, with parameter bounds.

This is the agent's action space for the parameters-only loop. It mirrors the
defaults in src/strategies.cpp; keep the two in sync when adding parameters.
"""

INT_PARAMS = {"size", "maxpos", "fast", "slow", "period", "interval_us", "sample_us", "cooldown_us"}

# name -> {param: (default, low, high, description)}
CATALOGUE = {
    "mm": {
        "description": "Market maker. Quotes both sides around fair value, skews quotes by inventory, "
                       "requotes on a timer and on every book change. Earns the spread, loses on jumps.",
        "params": {
            "spread": (2, 1, 20, "half spread in ticks (1 tick = $0.01)"),
            "size": (5, 1, 50, "quote size per side"),
            "skew": (0.05, 0.0, 1.0, "ticks of quote shift per unit of inventory"),
            "maxpos": (50, 5, 500, "stop quoting the side that would grow inventory past this"),
            "interval_us": (100, 20, 5000, "minimum microseconds between requotes"),
        },
    },
    "momentum": {
        "description": "Trend follower. Samples the mid on a clock, compares a fast and a slow moving "
                       "average, holds a position while the fast stays on one side of the slow, "
                       "flattens when it crosses back. Takes liquidity with IOC orders.",
        "params": {
            "sample_us": (1000, 100, 20000, "microseconds between mid samples"),
            "fast": (20, 2, 500, "fast window in samples"),
            "slow": (200, 5, 5000, "slow window in samples; must exceed fast"),
            "thr": (2.0, 0.1, 50.0, "ticks the fast average must lead the slow by before entering"),
            "size": (5, 1, 50, "units per order"),
            "maxpos": (30, 1, 300, "target absolute position when the signal is on"),
            "cooldown_us": (10000, 100, 500000, "microseconds between orders"),
        },
    },
    "meanrev": {
        "description": "Mean reversion. Samples the mid on a clock, fades deviations from a moving "
                       "average, flattens once the mid has come most of the way back. "
                       "Takes liquidity with IOC orders.",
        "params": {
            "sample_us": (1000, 100, 20000, "microseconds between mid samples"),
            "period": (200, 5, 5000, "moving-average window in samples"),
            "thr": (2.0, 0.1, 50.0, "ticks of deviation before entering"),
            "exit": (0.25, 0.0, 1.0, "flatten when |deviation| < thr * exit"),
            "size": (5, 1, 50, "units per order"),
            "maxpos": (30, 1, 300, "target absolute position when the signal is on"),
            "cooldown_us": (10000, 100, 500000, "microseconds between orders"),
        },
    },
}

MARKET_DESCRIPTION = (
    "Single instrument, price in ticks of $0.01 starting at $100.00. One session lasts a few seconds "
    "of wall-clock time and every agent runs in its own thread against a single-threaded matching engine. "
    "Background agents: one market maker and three noise traders. The noise traders share a latent true value "
    "that random-walks and occasionally jumps by tens of ticks; they post limit orders around their private view "
    "of it and take liquidity when their view crosses the touch. Trends appear after jumps; between jumps the "
    "mid wanders around the true value. Crossing the spread costs roughly the market maker's half spread per unit. "
    "Each agent starts with $100,000 cash and zero inventory; PnL is marked to the final mid."
)


def default_params(name):
    return {k: v[0] for k, v in CATALOGUE[name]["params"].items()}


def clamp_params(name, params):
    """Drop unknown keys and clamp values into the catalogue bounds. Returns (params, warnings)."""
    out, warnings = {}, []
    spec = CATALOGUE[name]["params"]
    for k, v in params.items():
        if k not in spec:
            warnings.append(f"unknown parameter {k} for {name}, dropped")
            continue
        default, lo, hi, _ = spec[k]
        try:
            v = float(v)
        except (TypeError, ValueError):
            warnings.append(f"non-numeric {k}={v!r}, using default {default}")
            v = default
        if v < lo or v > hi:
            warnings.append(f"{k}={v} outside [{lo}, {hi}], clamped")
            v = min(hi, max(lo, v))
        if k in INT_PARAMS:
            v = float(int(round(v)))
        out[k] = v
    if name == "momentum" and "fast" in out and "slow" in out and out["slow"] <= out["fast"]:
        warnings.append("slow <= fast, set slow = fast * 5")
        out["slow"] = out["fast"] * 5
    return out, warnings


def to_spec(name, params):
    """Format as the engine's --agent spec."""
    def fmt(v):
        return str(int(v)) if float(v).is_integer() else f"{v:.4f}".rstrip("0")
    if not params:
        return name
    return name + ":" + ",".join(f"{k}={fmt(v)}" for k, v in sorted(params.items()))


def catalogue_text():
    lines = []
    for name, entry in CATALOGUE.items():
        lines.append(f"{name}: {entry['description']}")
        for k, (default, lo, hi, desc) in entry["params"].items():
            lines.append(f"  {k}: {desc}. default {default}, range [{lo}, {hi}]")
    return "\n".join(lines)
