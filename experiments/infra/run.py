#!/usr/bin/env python3
"""One-subject infrastructure experiment. Sessions run serially for latency measurement.

init --out DIR; evaluate --out DIR --candidate SOURCE --rationale TEXT;
finalize --out DIR --generation N. The LLM reads feedback.json between evaluations.
No model API is called: an interactive LLM supplies each candidate source.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import shutil
import statistics
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import sweep

REGIMES = {
    'ordinary': ['mm', 'noise', 'noise', 'noise', 'momentum'],
    'busy': ['mm:interval_us=50'] + ['noise:interval_us=50'] * 6 + ['momentum'],
}

def dump(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def fixed_hashes(binary):
    paths = [HERE / f for f in ('api.h', 'host.cpp', 'check.cpp', 'host_check.cpp', 'run.py')]
    paths += sorted((ROOT / 'include').glob('*.h')) + sorted((ROOT / 'src').glob('*.cpp'))
    return {str(p.relative_to(ROOT)): sha(p) for p in paths} | {'engine_binary': sha(binary)}

def build(source, dest):
    dest.mkdir(parents=True)
    shutil.copyfile(source, dest / 'candidate.cpp')
    common = [os.environ.get('CXX', 'c++'), '-std=c++17', '-O2', '-pthread', '-Wall', '-Wextra',
              '-I', str(HERE)]
    commands = [common + ['-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                         str(HERE / 'check.cpp'), str(dest / 'candidate.cpp'), '-o', str(dest / 'check')],
                common + ['-I', str(ROOT / 'include'), '-fsanitize=address,undefined',
                          str(HERE / 'host_check.cpp'), str(dest / 'candidate.cpp'), '-o', str(dest / 'host_check')],
                common + ['-I', str(ROOT / 'include'), '-shared', '-fPIC', str(HERE / 'host.cpp'),
                          str(dest / 'candidate.cpp'), '-o', str(dest / 'subject.so')]]
    for i, cmd in enumerate(commands):
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        (dest / f'compile_{i}.log').write_text(p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError(f'Compilation failed: {dest}/compile_{i}.log')
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    for test in ('check', 'host_check'):
        p = subprocess.run([str(dest / test)], capture_output=True, text=True, timeout=20, env=env)
        (dest / f'{test}.log').write_text(p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError(f'Candidate failed reference/sanitizer checks: {dest}/{test}.log')
    dump(dest / 'build.json', {'commands': commands, 'candidate_sha256': sha(dest / 'candidate.cpp'),
                             'plugin_sha256': sha(dest / 'subject.so'), 'checks_passed': True})

def brief(out, history):
    (out / 'brief.md').write_text('''# Single LLM infrastructure experiment
You are a C++ engineer specializing in low-latency trading systems.
Use measurements to guide changes; the role does not imply any technique must win.
Change only a candidate .cpp implementing api.h. Return source plus a rationale.
Keep the exact rolling sum of the most recent 128 trade prices, including warmup.
You may change storage, incremental calculations, batch size and bounded idle policy.
No extra threads, engine headers/context, file/network I/O, market seed, rival state,
order submission, global shared state or changes to the frozen host are permitted.
Use at most 64 KiB of candidate-owned storage. Native code is reviewed, not sandboxed.
The fixed host evaluates EVERY trade: momentum relative to the 128-trade average,
strict threshold 2 ticks, IOC quantity 1, position cap 20, one outstanding order.
SPSC private/order queues and multicast public feed remain fixed. Lost data invalidates a run.
Objective: reduce publish-to-submit latency without losing correctness or starving peers.
Inspect own feedback.json, your previous source and api.h; explain the next hypothesis.
P&L is a secondary outcome. Do not select on held-out results. Sessions are nondeterministic.
''' + '\nCompleted training generations: ' + str(history) + '\n')

def init(args):
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    binary = Path(args.binary).resolve()
    cfg = {'binary': str(binary), 'seconds': args.seconds, 'seeds': list(range(1, args.seeds + 1)),
           'heldout_seeds': list(range(10001, 10001 + args.seeds)), 'regimes': REGIMES,
           'flags': ['--transport', 'spsc', '--fanout', 'multicast'],
           'fixed_hashes': fixed_hashes(binary), 'platform': platform.platform(),
           'machine': platform.machine(), 'cpu_count': os.cpu_count(),
           'compiler': subprocess.check_output([os.environ.get('CXX', 'c++'), '--version'], text=True),
           'proposer': 'single interactive LLM in the parent conversation; no autonomous model API',
           'memory_budget_bytes': 65536, 'thread_budget': 1}
    dump(out / 'config.json', cfg)
    build(HERE / 'candidates/baseline.cpp', out / 'baseline')
    brief(out, [])

def summarize(rows):
    result = {}
    for regime in REGIMES:
        result[regime] = {}
        for label in ('baseline', 'candidate'):
            rs = [r for r in rows if r['regime'] == regime and r['variant'] == label]
            aa = [r['report']['agents'][-1] for r in rs]
            result[regime][label] = {
                'sessions': len(aa), 'valid_sessions': sum(r['valid'] for r in rs),
                'p99_event_age_ns_median': statistics.median(a['latency']['event_age']['p99'] for a in aa),
                'p50_event_age_ns_median': statistics.median(a['latency']['event_age']['p50'] for a in aa),
                'p99_delivery_ns_median': statistics.median(a['latency']['delivery']['p99'] for a in aa),
                'p99_react_ns_median': statistics.median(a['latency']['react']['p99'] for a in aa),
                'p99_submit_to_pop_ns_median': statistics.median(a['latency']['submit_to_pop']['p99'] for a in aa),
                'latency_samples': [a['latency']['event_age']['count'] for a in aa],
                'pnl_mean': statistics.fmean(a['pnl'] for a in aa),
                'pnl_per_session': [a['pnl'] for a in aa],
                'fills_per_session': [a['fills'] for a in aa],
                'subject_drops': sum(a['events_dropped'] for a in aa),
                'subject_queue_full': sum(a['queue_full'] for a in aa),
                'process_cpu_seconds_mean': statistics.fmean(r['process_cpu_seconds'] for r in rs),
            }
    return result

def evaluate(args):
    out = Path(args.out).resolve()
    cfg = json.loads((out / 'config.json').read_text())
    if (out / 'selection.json').exists():
        raise RuntimeError('Selection is frozen; no more evaluations in this experiment')
    if fixed_hashes(cfg['binary']) != cfg['fixed_hashes']:
        raise RuntimeError('Frozen engine/host/interface/runner changed; start a new experiment')
    if sha(out / 'baseline/subject.so') != json.loads((out / 'baseline/build.json').read_text())['plugin_sha256']:
        raise RuntimeError('Baseline binary changed')
    final = args.command == 'finalize'
    if final:
        source_dir = out / f'gen_{args.generation}'
        if not (source_dir / 'feedback.json').exists():
            raise RuntimeError('Selected generation has no completed training evaluation')
        d = out / 'heldout'
        d.mkdir()
        dump(out / 'selection.json', {'generation': args.generation, 'reason': args.rationale,
                                     'source_sha256': sha(source_dir / 'candidate.cpp')})
        candidate = source_dir / 'subject.so'
        if sha(candidate) != json.loads((source_dir / 'build.json').read_text())['plugin_sha256']:
            raise RuntimeError('Selected plugin changed')
    else:
        gen = len(list(out.glob('gen_*')))
        d = out / f'gen_{gen}'
        build(Path(args.candidate).resolve(), d)
        dump(d / 'proposal.json', {'rationale': args.rationale, 'generation': gen,
                                  'source_sha256': sha(d / 'candidate.cpp')})
        candidate = d / 'subject.so'
    seeds = cfg['heldout_seeds'] if final else cfg['seeds']
    rows = []
    for ri, (regime, background) in enumerate(cfg['regimes'].items()):
        for si, seed in enumerate(seeds):
            variants = [('baseline', out / 'baseline/subject.so'), ('candidate', candidate)]
            if (ri + si) % 2:
                variants.reverse()
            for label, plugin in variants:
                before = resource.getrusage(resource.RUSAGE_CHILDREN)
                report = sweep.run_one(cfg['binary'], seed, cfg['seconds'],
                                      background + [f'plugin:{plugin}'], extra=cfg['flags'])
                after = resource.getrusage(resource.RUSAGE_CHILDREN)
                subject = report['agents'][-1]
                reasons = []
                if report['fanout'] != 'multicast' or any(a['transport'] != 'spsc' for a in report['agents']):
                    reasons.append('transport mismatch')
                if report['events_dropped'] or any(a['queue_full'] for a in report['agents']):
                    reasons.append('lost events or full order queue')
                if abs(subject['position']) > 20:
                    reasons.append('position limit exceeded')
                if subject['latency']['event_age']['count'] < 20:
                    reasons.append('fewer than 20 order-latency samples')
                row = {'regime': regime, 'variant': label, 'seed': seed, 'report': report,
                       'valid': not reasons, 'invalid_reasons': reasons,
                       'process_cpu_seconds': after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime}
                rows.append(row)
                with (d / 'raw.jsonl').open('a') as f:
                    f.write(json.dumps(row) + '\n')
                print(regime, seed, label, 'valid' if not reasons else reasons, flush=True)
    feedback = summarize(rows)
    dump(d / 'feedback.json', feedback)
    if not final:
        dump(out / 'feedback.json', feedback)
        brief(out, sorted(p.name for p in out.glob('gen_*')))
    print(json.dumps(feedback, indent=2))

def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    a = sub.add_parser('init')
    a.add_argument('--out', required=True)
    a.add_argument('--binary', default=str(ROOT / 'build/market_engine'))
    a.add_argument('--seconds', type=float, default=2)
    a.add_argument('--seeds', type=int, default=3)
    a = sub.add_parser('evaluate')
    a.add_argument('--out', required=True)
    a.add_argument('--candidate', required=True)
    a.add_argument('--rationale', required=True)
    a = sub.add_parser('finalize')
    a.add_argument('--out', required=True)
    a.add_argument('--generation', type=int, required=True)
    a.add_argument('--rationale', required=True)
    args = p.parse_args()
    if args.command == 'init':
        if args.seconds <= 0 or args.seeds < 1:
            p.error('seconds and seeds must be positive')
        init(args)
    else:
        evaluate(args)

if __name__ == '__main__':
    main()
