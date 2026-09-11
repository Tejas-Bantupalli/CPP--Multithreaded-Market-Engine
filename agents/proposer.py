"""Proposers turn a performance report into the next strategy to try.

Two implementations share one interface:

  ClaudeProposer  asks Claude for a strategy and parameters, structured-output JSON
  MockProposer    random perturbation with a keep-the-best rule; for testing the loop offline

A proposal is {"strategy": name, "params": {k: v}, "rationale": text}.
"""
import json
import math
import random

from catalogue import CATALOGUE, catalogue_text, clamp_params, default_params, MARKET_DESCRIPTION

PERSONAS = {
    "neutral": "You are a quantitative trader evaluating strategies on a simulated market.",
    "cautious": "You are a risk-averse quantitative trader. You value consistency over peak returns, "
                "prefer small positions and wide safety margins, and change one thing at a time.",
    "aggressive": "You are an aggressive proprietary trader. You are willing to take large positions and "
                  "accept variance in pursuit of the highest mean PnL. You make bold parameter changes.",
    "market_maker": "You are a market-making specialist. You think in terms of spread capture, inventory risk, "
                    "and adverse selection. You prefer to provide liquidity rather than take it.",
    "trend": "You are a systematic trend follower. You believe price moves persist and look for "
             "parameters that catch and ride them while limiting whipsaw.",
    "hft": "You are a high-frequency trader at a proprietary trading firm, trading in this market. "
           "You build your own strategy from scratch.",
}

PROPOSAL_SCHEMA = {
    "type": "object",
    "properties": {
        "strategy": {"type": "string", "enum": list(CATALOGUE.keys())},
        "params": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {"name": {"type": "string"}, "value": {"type": "number"}},
                "required": ["name", "value"],
                "additionalProperties": False,
            },
        },
        "rationale": {"type": "string"},
    },
    "required": ["strategy", "params", "rationale"],
    "additionalProperties": False,
}


class Proposal(dict):
    @property
    def strategy(self):
        return self["strategy"]

    @property
    def params(self):
        return self["params"]

    @property
    def rationale(self):
        return self["rationale"]


def normalise(strategy, raw_params, rationale):
    """Validate and clamp a proposal from any source."""
    if strategy not in CATALOGUE:
        raise ValueError(f"unknown strategy {strategy}")
    if isinstance(raw_params, list):
        raw_params = {p["name"]: p["value"] for p in raw_params}
    merged = default_params(strategy)
    merged.update(raw_params)
    params, warnings = clamp_params(strategy, merged)
    return Proposal(strategy=strategy, params=params, rationale=rationale), warnings


class MockProposer:
    """Keeps the best-scoring proposal seen and perturbs it. No network."""

    def __init__(self, seed=0, start_strategy="momentum"):
        self.rng = random.Random(seed)
        self.start_strategy = start_strategy
        self.best = None
        self.best_score = -math.inf

    def propose(self, subject, history, report):
        if not history:
            return normalise(self.start_strategy, {}, "initial defaults (mock)")
        last = history[-1]
        if last["score"] > self.best_score:
            self.best_score, self.best = last["score"], last["proposal"]
        base = self.best or last["proposal"]
        params = {}
        for k, v in base["params"].items():
            params[k] = v * math.exp(self.rng.gauss(0, 0.3)) if v != 0 else self.rng.random() * 0.1
        return normalise(base["strategy"], params, f"perturb best (score {self.best_score:.2f}) (mock)")


class ClaudeProposer:
    def __init__(self, model="claude-opus-5", persona="neutral", effort="high"):
        import anthropic  # imported here so the mock path needs no SDK
        self.anthropic = anthropic
        self.client = anthropic.Anthropic()
        self.model = model
        self.effort = effort
        self.system = (
            PERSONAS[persona] + "\n\n"
            "You control one agent in a multi-agent market simulation. Each generation you choose a strategy from "
            "the catalogue and its parameters. The simulation then runs your choice over many random seeds against the "
            "same background market and reports the distribution of your PnL. Your objective is the highest mean PnL "
            "with a hit rate above one half. You are free to switch strategies. Reason from the evidence you are given; "
            "say what you expect a change to do and why.\n\n"
            "MARKET\n" + MARKET_DESCRIPTION + "\n\n"
            "STRATEGY CATALOGUE\n" + catalogue_text()
        )

    def propose(self, subject, history, report):
        user = build_user_message(subject, history, report)
        response = self.client.messages.create(
            model=self.model,
            max_tokens=16000,
            system=[{"type": "text", "text": self.system, "cache_control": {"type": "ephemeral"}}],
            messages=[{"role": "user", "content": user}],
            output_config={"effort": self.effort, "format": {"type": "json_schema", "schema": PROPOSAL_SCHEMA}},
        )
        if response.stop_reason == "refusal":
            raise RuntimeError(f"model refused: {response.stop_details}")
        text = next(b.text for b in response.content if b.type == "text")
        data = json.loads(text)
        proposal, warnings = normalise(data["strategy"], data["params"], data["rationale"])
        proposal["usage"] = {
            "input_tokens": response.usage.input_tokens,
            "output_tokens": response.usage.output_tokens,
            "cache_read_input_tokens": response.usage.cache_read_input_tokens,
        }
        return proposal, warnings


def build_user_message(subject, history, report):
    parts = [f"You are agent '{subject}'."]
    if not history:
        parts.append("This is generation 0. There is no performance history yet. Choose a starting strategy "
                     "and parameters and explain your reasoning.")
    else:
        parts.append(f"Generation {len(history)}. Your history, oldest first (score = mean PnL over the seeds):")
        for i, h in enumerate(history):
            p = h["proposal"]
            parts.append(f"  gen {i}: {p['strategy']} {json.dumps(p['params'], sort_keys=True)} -> "
                         f"mean {h['score']:.2f}, std {h['std']:.2f}, hit {h['hit_rate']:.2f}, fills {h['fills']:.0f}")
        parts.append("Latest report:")
        parts.append(json.dumps(report, indent=1, sort_keys=True))
        parts.append("Propose the strategy and parameters for the next generation.")
    return "\n".join(parts)
