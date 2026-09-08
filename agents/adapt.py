"""Algorithmic adaptation rules. No network, no model: an agent updates its
strategy from the performance information the loop gives it.

  HillClimb  keep the best parameters seen so far, perturb them (uses own history only)
  Imitate    HillClimb, plus with --info rivals it may copy the best rival's strategy and
             parameters with a mutation (social learning vs solo learning)
  Bandit     chooses the strategy class by UCB1 over past scores, then perturbs the best
             parameters seen for that class (lets an agent switch between mm, momentum, meanrev)

All three return the same Proposal shape as the other proposers, so the loop,
the --info modes, and the trajectory files are identical across rules.
"""
import math
import random

from catalogue import CATALOGUE, default_params, parse_spec
from proposer import normalise


def best_entry(history):
    return max(history, key=lambda h: h["score"]) if history else None


class HillClimb:
    def __init__(self, seed=0, start_strategy="momentum", sigma=0.3):
        self.rng = random.Random(seed)
        self.start_strategy = start_strategy
        self.sigma = sigma

    def name(self):
        return "hillclimb"

    def perturb(self, params):
        out = {}
        for k, v in params.items():
            if v == 0:
                out[k] = self.rng.random() * 0.1
            else:
                out[k] = v * math.exp(self.rng.gauss(0, self.sigma))
        return out

    def propose(self, subject, history, report):
        if not history:
            return normalise(self.start_strategy, {}, f"{self.name()}: start with {self.start_strategy} defaults")
        best = best_entry(history)
        base = best["proposal"]
        return normalise(base["strategy"], self.perturb(base["params"]),
                         f"{self.name()}: perturb best so far (gen {history.index(best)}, score {best['score']:.2f})")


class Imitate(HillClimb):
    """Copies the best rival when it is doing clearly better; otherwise climbs alone."""

    def __init__(self, seed=0, start_strategy="momentum", sigma=0.3, p_imitate=0.5, margin=1.0):
        super().__init__(seed, start_strategy, sigma)
        self.p_imitate = p_imitate
        self.margin = margin

    def name(self):
        return "imitate"

    def propose(self, subject, history, report):
        # Only rivals whose strategy is in the catalogue can be copied (not plugins, not noise traders).
        others = [o for o in ((report or {}).get("others") or []) if parse_spec(o["spec"])[0] in CATALOGUE]
        if history and others and self.rng.random() < self.p_imitate:
            my_best = best_entry(history)["score"]
            rival = max(others, key=lambda o: o["mean_pnl"])
            name, params = parse_spec(rival["spec"])
            if rival["mean_pnl"] > my_best + self.margin:
                merged = default_params(name)
                merged.update(params)
                return normalise(name, self.perturb(merged),
                                 f"imitate: copy agent {rival['id']} ({name}, mean {rival['mean_pnl']:.2f} "
                                 f"vs my best {my_best:.2f}) with mutation")
        return super().propose(subject, history, report)


class Bandit(HillClimb):
    """UCB1 over strategy classes; parameters climb within the chosen class."""

    def __init__(self, seed=0, start_strategy="momentum", sigma=0.3, c=2.0):
        super().__init__(seed, start_strategy, sigma)
        self.c = c

    def name(self):
        return "bandit"

    def propose(self, subject, history, report):
        arms = list(CATALOGUE.keys())
        by_arm = {a: [h for h in history if h["proposal"]["strategy"] == a] for a in arms}
        untried = [a for a in arms if not by_arm[a]]
        if untried:
            arm = untried[0] if not history else self.rng.choice(untried)
            return normalise(arm, {}, f"bandit: try untried class {arm}")
        n = len(history)
        scale = max(1.0, max(abs(h["score"]) for h in history))  # UCB needs a bounded-ish reward
        def ucb(a):
            xs = [h["score"] for h in by_arm[a]]
            return sum(xs) / len(xs) / scale + self.c * math.sqrt(math.log(n) / len(xs))
        arm = max(arms, key=ucb)
        best = best_entry(by_arm[arm])
        return normalise(arm, self.perturb(best["proposal"]["params"]),
                         f"bandit: {arm} by UCB1 ({', '.join(f'{a}={ucb(a):.2f}' for a in arms)}), "
                         f"perturb its best (score {best['score']:.2f})")


RULES = {"hillclimb": HillClimb, "imitate": Imitate, "bandit": Bandit}
