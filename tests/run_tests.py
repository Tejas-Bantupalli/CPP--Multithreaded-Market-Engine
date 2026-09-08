"""Runs each test in its own process; timeouts catch shutdown hangs."""
from pathlib import Path
import subprocess
import sys

CASES = (
    "book_price_time",
    "book_partial_and_ioc",
    "book_walk_levels",
    "book_cancel",
    "book_self_trade_prevention",
    "book_rejects",
    "histogram",
    "spsc_wraparound",
    "multicast_ring",
    "agent_spec",
    "engine_conservation",
    "engine_multicast",
    "engine_collar",
    "engine_shutdown_drain",
    "engine_matching_pair",
)

binary = str(Path(sys.argv[1]).resolve())
failed = []
for case in CASES:
    try:
        subprocess.run([binary, case], check=True, timeout=60)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        failed.append(f"{case}: {exc}")
# Plugin smoke test: the example plugin loads, trades, and the engine exits cleanly.
build = Path(binary).parent
engine = build / "market_engine"
plugin = build / "plugins" / "example_breakout.so"
if engine.exists() and plugin.exists():
    try:
        subprocess.run([str(engine), "--seconds", "0.3", "--quiet", "--agent", "mm", "--agent", "noise",
                        "--plugin", f"{plugin}:window=50,size=2"], check=True, timeout=30)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        failed.append(f"plugin_smoke: {exc}")
else:
    failed.append("plugin_smoke: build/market_engine or build/plugins/example_breakout.so missing")

if failed:
    print("FAILED:\n  " + "\n  ".join(failed))
    sys.exit(1)
print(f"all {len(CASES)} tests + plugin smoke test passed")
