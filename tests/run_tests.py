"""Runs each test in its own process; timeouts catch shutdown hangs."""
from pathlib import Path
import json
import tempfile
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
    "spsc_concurrent",
    "mpsc_concurrent",
    "multicast_ring",
    "transport_channels",
    "agent_spec",
    "engine_conservation",
    "engine_transport_mutex",
    "engine_transport_mutex_cv",
    "engine_transport_mixed",
    "engine_transport_mpsc",
    "engine_transport_mpsc_mixed",
    "engine_custom_run_loop",
    "engine_multicast",
    "engine_collar",
    "engine_fees",
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

# Verify the documented demo end to end, including shutdown flushing the log.
build = Path(binary).parent
try:
    with tempfile.TemporaryDirectory() as tmp:
        report_path = Path(tmp) / "report.json"
        commands_path = Path(tmp) / "commands.csv"
        trades_path = Path(tmp) / "trades.csv"
        subprocess.run(
            [str(build / "market_engine"), "--seconds", "0.4", "--collar", "0",
             "--quiet", "--json", str(report_path), "--cmdlog", str(commands_path),
             "--trades", str(trades_path)], check=True, timeout=30)
        report = json.loads(report_path.read_text())
        replay = subprocess.check_output(
            [str(build / "replay"), str(commands_path)], text=True, timeout=30)
        values = dict(field.split("=", 1) for field in replay.split())
        assert report["log_dropped_cmds"] == report["log_dropped_trades"] == 0
        assert report["trades"] > 0
        for key in ("commands", "trades", "volume", "open_orders"):
            assert int(values[key]) == report[key], key
        assert values["checksum"] == report["book_checksum"]
        assert len(trades_path.read_text().splitlines()) - 1 == report["trades"]
        print("PASS session_replay")
except (subprocess.SubprocessError, OSError, ValueError, KeyError, AssertionError) as exc:
    failed.append(f"session_replay: {exc}")

if failed:
    print("FAILED:\n  " + "\n  ".join(failed))
    sys.exit(1)
print(f"all {len(CASES)} tests + plugin smoke + session replay passed")
