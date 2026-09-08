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
    "agent_spec",
    "engine_conservation",
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
if failed:
    print("FAILED:\n  " + "\n  ".join(failed))
    sys.exit(1)
print(f"all {len(CASES)} tests passed")
