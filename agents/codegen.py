"""Code-writing proposers: the agent writes a Strategy subclass in C++, the loop
compiles it into a plugin, smoke-tests it, and the compiled library becomes the
agent for the next generation.

  ClaudeCodeProposer  Claude writes the source; compiler errors are fed back for up to 3 attempts
  MockCodeProposer    perturbs the example plugin's default window; offline control

Generated code is compiled and executed on this machine with your privileges.
Treat the output directory as untrusted and read the source before reusing it.
"""
import json
import os
import random
import re
import subprocess
from pathlib import Path

from proposer import PERSONAS, Proposal
from catalogue import MARKET_DESCRIPTION

ROOT = Path(__file__).resolve().parent.parent
INTERFACE_FILES = ["include/types.h", "include/transport.h", "include/strategy.h",
                   "include/strategies.h", "include/plugin.h", "plugins/example_breakout.cpp"]

CODE_SCHEMA = {
    "type": "object",
    "properties": {
        "class_name": {"type": "string"},
        "source": {"type": "string"},
        "rationale": {"type": "string"},
    },
    "required": ["class_name", "source", "rationale"],
    "additionalProperties": False,
}


def compile_plugin(src, out_dir):
    """Compile one plugin source. Returns (path_to_so or None, compiler_output)."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    so = out_dir / (Path(src).stem + ".so")
    cmd = [os.environ.get("CXX", "g++"), "-O2", "-std=c++17", "-pthread", "-Wall", "-Wextra",
           "-I", str(ROOT / "include"), "-shared", "-fPIC", str(src), "-o", str(so)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if proc.returncode != 0:
        return None, proc.stderr[-6000:]
    return so, proc.stderr[-2000:]


def smoke_test(so, binary=None, seconds=0.3):
    """Run the plugin briefly in a small market. Catches crashes and hangs. Returns (ok, output)."""
    binary = binary or ROOT / "build" / "market_engine"
    cmd = [str(binary), "--seconds", str(seconds), "--quiet", "--agent", "mm", "--agent", "noise",
           "--plugin", str(so)]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=seconds + 20)
    except subprocess.TimeoutExpired:
        return False, "engine did not exit: the strategy probably blocks or spins inside a callback"
    if proc.returncode != 0:
        return False, f"exit {proc.returncode}\n{proc.stderr[-3000:]}"
    return True, proc.stdout.strip()


def interface_text():
    parts = []
    for rel in INTERFACE_FILES:
        parts.append(f"===== {rel} =====\n" + (ROOT / rel).read_text())
    return "\n\n".join(parts)


def build_code_message(subject, history, report):
    parts = [f"You are agent '{subject}'."]
    if not history:
        parts.append("Generation 0. No performance history yet. Write your first strategy.")
    else:
        parts.append(f"Generation {len(history)}. History, oldest first (score = mean PnL over the seeds):")
        for i, h in enumerate(history):
            p = h["proposal"]
            parts.append(f"  gen {i}: {p.get('class_name', '?')} -> mean {h['score']:.2f}, std {h['std']:.2f}, "
                         f"hit {h['hit_rate']:.2f}, fills {h['fills']:.0f}")
        last_src = history[-1]["proposal"].get("source_path")
        if last_src and Path(last_src).exists():
            parts.append("Your most recent source:\n```cpp\n" + Path(last_src).read_text() + "\n```")
        parts.append("Latest report:\n" + json.dumps(report, indent=1, sort_keys=True))
    parts.append("Return the complete C++ source of your plugin for the next generation.")
    return "\n".join(parts)


class ClaudeCodeProposer:
    def __init__(self, workdir, model="claude-opus-5", persona="neutral", effort="high", binary=None):
        import anthropic
        self.anthropic = anthropic
        self.client = anthropic.Anthropic()
        self.model = model
        self.effort = effort
        self.workdir = Path(workdir)
        self.binary = binary
        self.system = (
            PERSONAS[persona] + "\n\n"
            "You control one agent in a multi-agent market simulation by writing its strategy in C++17. "
            "Each generation you return one complete source file that defines a Strategy subclass and ends with "
            "MARKET_PLUGIN(YourClass). It is compiled as a shared library and run over many random seeds against "
            "the same background market; you then see the distribution of your PnL. Your objective is the highest "
            "mean PnL with a hit rate above one half.\n\n"
            "Rules for the code:\n"
            "- Include only \"plugin.h\" plus standard headers. Everything you need is in the interface below.\n"
            "- on_event and on_idle run on your own thread and must return quickly: no sleeping, no blocking, "
            "no I/O, no unbounded loops. Throttle yourself with ctx.now().\n"
            "- Read parameters with the Params::get(key, default) pattern so they can be tuned from the command line.\n"
            "- Keep positions bounded. Cash is $100,000; PnL is marked to the final mid.\n"
            "- The compiler runs with -Wall -Wextra; warnings are fine, errors are returned to you to fix.\n\n"
            "MARKET\n" + MARKET_DESCRIPTION + "\n\n"
            "INTERFACE (verbatim)\n" + interface_text()
        )

    def propose(self, subject, history, report):
        self.workdir.mkdir(parents=True, exist_ok=True)
        messages = [{"role": "user", "content": build_code_message(subject, history, report)}]
        gen = len(history)
        last_error = ""
        usage = {"input_tokens": 0, "output_tokens": 0, "cache_read_input_tokens": 0}
        for attempt in range(3):
            response = self.client.messages.create(
                model=self.model,
                max_tokens=32000,
                system=[{"type": "text", "text": self.system, "cache_control": {"type": "ephemeral"}}],
                messages=messages,
                output_config={"effort": self.effort, "format": {"type": "json_schema", "schema": CODE_SCHEMA}},
            )
            usage["input_tokens"] += response.usage.input_tokens
            usage["output_tokens"] += response.usage.output_tokens
            usage["cache_read_input_tokens"] += response.usage.cache_read_input_tokens or 0
            if response.stop_reason == "refusal":
                raise RuntimeError(f"model refused: {response.stop_details}")
            text = next(b.text for b in response.content if b.type == "text")
            data = json.loads(text)
            class_name = re.sub(r"[^A-Za-z0-9_]", "", data["class_name"]) or "Strategy"
            src = self.workdir / f"{subject}_gen{gen}_try{attempt}.cpp"
            src.write_text(data["source"])

            so, out = compile_plugin(src, self.workdir / "lib")
            if so is None:
                last_error = out
                messages += [{"role": "assistant", "content": text},
                             {"role": "user", "content": "Compilation failed. Fix it and return the complete source again.\n\n" + out}]
                continue
            ok, out = smoke_test(so, self.binary)
            if not ok:
                last_error = out
                messages += [{"role": "assistant", "content": text},
                             {"role": "user", "content": "The plugin compiled but failed a short test run. Fix it and return the complete source again.\n\n" + out}]
                continue
            proposal = Proposal(strategy="plugin", params={}, rationale=data["rationale"],
                                spec=f"plugin:{so}", source_path=str(src), class_name=class_name, usage=usage)
            return proposal, ([] if attempt == 0 else [f"needed {attempt + 1} attempts"])
        raise RuntimeError(f"no working plugin after 3 attempts; last error:\n{last_error}")


class MockCodeProposer:
    """Copies the example breakout plugin with a perturbed window. Exercises the compile and load path."""

    def __init__(self, workdir, seed=0, binary=None):
        self.workdir = Path(workdir)
        self.rng = random.Random(seed)
        self.binary = binary
        self.best_window, self.best_score = 300, float("-inf")
        self.template = (ROOT / "plugins" / "example_breakout.cpp").read_text()

    def propose(self, subject, history, report):
        self.workdir.mkdir(parents=True, exist_ok=True)
        if history:
            last = history[-1]
            if last["score"] > self.best_score:
                self.best_score, self.best_window = last["score"], last["proposal"].get("window", 300)
        window = 300 if not history else max(20, int(self.best_window * self.rng.uniform(0.6, 1.5)))
        source = self.template.replace('p.get("window", 300)', f'p.get("window", {window})')
        src = self.workdir / f"{subject}_gen{len(history)}.cpp"
        src.write_text(source)
        so, out = compile_plugin(src, self.workdir / "lib")
        if so is None:
            raise RuntimeError(out)
        ok, out = smoke_test(so, self.binary)
        if not ok:
            raise RuntimeError(out)
        proposal = Proposal(strategy="plugin", params={}, rationale=f"breakout window {window} (mock code)",
                            spec=f"plugin:{so}", source_path=str(src), class_name="Breakout", window=window)
        return proposal, []
