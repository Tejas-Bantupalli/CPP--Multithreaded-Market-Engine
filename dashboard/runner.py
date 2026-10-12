"""Local experiment execution. Reuses the engine's interfaces and result format."""
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'agents'), str(ROOT / 'scripts')]
from codegen import interface_text
from catalogue import MARKET_DESCRIPTION
from loop import build_report
from sweep import summarise

BACKGROUND = ['mm', 'noise', 'noise', 'noise']
BUILTINS = {'mm', 'noise', 'momentum', 'meanrev'}


def save(path, data):
    temp = path.with_suffix(path.suffix + '.tmp')
    temp.write_text(json.dumps(data, indent=2, allow_nan=False))
    temp.replace(path)


def validate(data):
    """Produce a bounded config; credentials never enter the saved configuration."""
    if not isinstance(data, dict):
        raise ValueError('Expected an experiment configuration.')
    cfg = {'title': str(data.get('title', '')).strip()[:120] or 'Untitled experiment',
           'prompt': str(data.get('prompt', '')).strip()[:16000],
           'model': str(data.get('model', 'claude-opus-5')).strip()[:100],
           'info': data.get('info', 'rivals'), 'logs': data.get('logs', False),
           'background': BACKGROUND, 'dashboard': True}
    for name, lo, hi in [('generations', 1, 20), ('seeds', 1, 30), ('seconds', .1, 30)]:
        value = data.get(name, {'generations': 3, 'seeds': 4, 'seconds': 2}[name])
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not lo <= value <= hi:
            raise ValueError(f'{name} must be between {lo} and {hi}.')
        if name != 'seconds' and int(value) != value:
            raise ValueError(f'{name} must be a whole number.')
        cfg[name] = value if name == 'seconds' else int(value)
    if cfg['info'] not in {'own', 'market', 'rivals'} or not isinstance(cfg['logs'], bool):
        raise ValueError('Invalid feedback or logging option.')
    subjects = data.get('subjects')
    if not isinstance(subjects, list) or not 1 <= len(subjects) <= 8:
        raise ValueError('Add between one and eight strategies.')
    cfg['subjects'] = []
    for i, sub in enumerate(subjects):
        if not isinstance(sub, dict) or sub.get('mode') not in {'ai', 'custom', 'builtin'}:
            raise ValueError('Choose AI, custom C++, or built-in for each strategy.')
        name = str(sub.get('name', f'agent_{i+1}')).strip()
        if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_-]{0,31}', name):
            raise ValueError('Strategy names must start with a letter and use letters, numbers, - or _.')
        entry = {'name': name, 'mode': sub['mode'], 'instruction': str(sub.get('instruction', ''))[:8000],
                 'builtin': sub.get('builtin', 'mm'), 'source': str(sub.get('source', ''))}
        if entry['builtin'] not in BUILTINS:
            raise ValueError('Unknown built-in strategy.')
        if len(entry['source']) > 100000 or (entry['mode'] == 'custom' and not entry['source'].strip()):
            raise ValueError('Custom strategies need C++ source (at most 100 KB).')
        cfg['subjects'].append(entry)
    if len({s['name'] for s in cfg['subjects']}) != len(subjects):
        raise ValueError('Strategy names must be unique.')
    if any(s['mode'] == 'ai' for s in cfg['subjects']) and (not cfg['prompt'] or not cfg['model']):
        raise ValueError('AI strategies need an experiment brief and model ID.')
    return cfg


class Cancelled(Exception):
    pass


class Job:
    def __init__(self, directory, cfg, api_key='', generate=None):
        self.directory, self.cfg, self.api_key = directory, cfg, api_key
        self.cancelled = threading.Event()
        self.generate = generate or self.generate_source
        self.state = {'status': 'queued', 'phase': 'Queued', 'generation': 0, 'sessions_done': 0,
                      'sessions_total': cfg['generations'] * cfg['seeds'], 'created': time.time()}
        self.write_state()

    def write_state(self, **values):
        self.state.update(values)
        save(self.directory / 'dashboard.json', self.state)

    def log(self, message):
        # Never log request headers or credentials.
        if self.api_key:
            message = message.replace(self.api_key, '[redacted]')
        with (self.directory / 'activity.log').open('a') as f:
            f.write(time.strftime('%H:%M:%S') + '  ' + message + '\n')

    def check_cancelled(self):
        if self.cancelled.is_set():
            raise Cancelled()

    def command(self, args, timeout):
        self.check_cancelled()
        env = {k: v for k, v in os.environ.items() if not any(x in k.upper() for x in ('KEY', 'TOKEN', 'SECRET', 'PASSWORD'))}
        with subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, env=env, start_new_session=True) as proc:
            deadline = time.monotonic() + timeout
            while True:
                try:
                    output, _ = proc.communicate(timeout=.2)
                    break
                except subprocess.TimeoutExpired:
                    if self.cancelled.is_set() or time.monotonic() >= deadline:
                        try:
                            os.killpg(proc.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        proc.communicate()
                        self.check_cancelled()
                        raise RuntimeError('Process timed out: ' + Path(args[0]).name)
        if proc.returncode:
            raise RuntimeError(output[-6000:] or f'Process exited with code {proc.returncode}')
        return output

    def generate_source(self, system, messages):
        self.check_cancelled()
        body = json.dumps({'model': self.cfg['model'], 'max_tokens': 12000,
                           'system': system, 'messages': messages}).encode()
        req = urllib.request.Request('https://api.anthropic.com/v1/messages', data=body,
                                     headers={'content-type': 'application/json', 'x-api-key': self.api_key,
                                              'anthropic-version': '2023-06-01'})
        try:
            with urllib.request.urlopen(req, timeout=120) as response:
                result = json.load(response)
        except urllib.error.HTTPError as exc:
            raise RuntimeError(f'Claude API returned HTTP {exc.code}. Check the model ID, API key and account limits.') from None
        except urllib.error.URLError:
            raise RuntimeError('Could not reach Claude API. Check the network connection.') from None
        self.check_cancelled()
        if result.get('stop_reason') == 'max_tokens':
            raise RuntimeError('Model response exceeded the output limit; request a simpler strategy.')
        source = '\n'.join(b['text'] for b in result.get('content', []) if b.get('type') == 'text').strip()
        if source.startswith('```'):
            source = re.sub(r'^```[^\n]*\n', '', source)
            source = re.sub(r'\n```\s*$', '', source)
        if not source or len(source) > 100000:
            raise RuntimeError('Model returned empty or oversized source.')
        return source

    def strategy(self, sub, generation, history, feedback):
        if sub['mode'] == 'builtin':
            return sub['builtin'], {'strategy': sub['builtin'], 'params': {}, 'rationale': 'Fixed built-in control.'}
        folder = self.directory / f'gen_{generation}' / sub['name']
        folder.mkdir(parents=True)
        src, library = folder / 'strategy.cpp', folder / 'strategy.so'
        system = ('Write a C++17 trading strategy for this local market simulator. Return ONLY complete C++ source, '
                  'no markdown. Include "plugin.h" and standard headers; implement Strategy with a constructor '
                  'taking const Params&, and end with MARKET_PLUGIN(ClassName). Keep inventory bounded. '
                  'Callbacks must return promptly. No filesystem, network, process execution, or external I/O. '
                  'If overriding run(), observe the running flag and apply every consumed event. '
                  'Explain the strategy in a short source comment.\n\nMARKET\n' + MARKET_DESCRIPTION +
                  '\n\nINTERFACE\n' + interface_text())
        prompt = (f"Experiment: {self.cfg['prompt']}\nYou are {sub['name']}. Your mandate: {sub['instruction']}\n"
                  f'Generation {generation + 1}. Your previous scores: {json.dumps(history)}\n'
                  f'Permitted feedback: {json.dumps(feedback)}')
        if generation and sub['mode'] == 'ai':
            previous = self.directory / f'gen_{generation-1}' / sub['name'] / 'strategy.cpp'
            prompt += '\nYour previous source:\n' + previous.read_text()
        (folder / 'brief.md').write_text(prompt)
        messages = [{'role': 'user', 'content': prompt}]
        for attempt in range(3 if sub['mode'] == 'ai' else 1):
            self.write_state(phase=f"{'Writing' if sub['mode'] == 'ai' else 'Compiling'} {sub['name']} · attempt {attempt+1}")
            self.log(self.state['phase'])
            source = self.generate(system, messages) if sub['mode'] == 'ai' else sub['source']
            self.check_cancelled()
            src.write_text(source)
            (folder / f'attempt_{attempt+1}.cpp').write_text(source)
            try:
                self.write_state(phase=f"Compiling {sub['name']}")
                output = self.command(['clang++', '-O2', '-std=c++17', '-pthread', '-Wall', '-Wextra',
                                       '-I', str(ROOT / 'include'), '-shared', '-fPIC', str(src), '-o', str(library)], 90)
                self.log(output.strip() or f"Compiled {sub['name']}")
                self.write_state(phase=f"Smoke-testing {sub['name']}")
                self.command([str(ROOT / 'build/market_engine'), '--seconds', '.15', '--quiet',
                              '--agent', 'mm', '--agent', 'noise', '--plugin', str(library)], 10)
                self.log(f"Passed smoke test: {sub['name']}")
                return f'plugin:{library}', {'strategy': 'plugin', 'params': {}, 'source_path': str(src),
                                            'class_name': sub['name'], 'rationale': source[:1000]}
            except RuntimeError as exc:
                self.log(str(exc))
                if attempt == (2 if sub['mode'] == 'ai' else 0):
                    raise RuntimeError(f"{sub['name']} did not pass compilation/smoke testing. See activity log.") from None
                messages.extend([{'role': 'assistant', 'content': source},
                                 {'role': 'user', 'content': f'Fix this failure and return the entire source:\n{exc}'}])

    def run(self):
        try:
            self.write_state(status='running', phase='Building engine')
            self.log('Building engine; sessions run serially for comparable latency.')
            self.command(['make', '-j2', 'all'], 120)
            cfg, history, feedback = self.cfg, {}, {}
            for generation in range(cfg['generations']):
                self.check_cancelled()
                self.write_state(generation=generation)
                specs, proposals = list(BACKGROUND), {}
                for sub in cfg['subjects']:
                    spec, proposal = self.strategy(sub, generation, history.get(sub['name'], []), feedback.get(sub['name'], {}))
                    specs.append(spec)
                    proposals[sub['name']] = proposal
                runs = []
                seeds = list(range(generation * 1000 + 1, generation * 1000 + cfg['seeds'] + 1))
                logs = self.directory / f'gen_{generation}' / 'sessions'
                logs.mkdir(parents=True, exist_ok=True)
                for i, seed in enumerate(seeds):
                    self.write_state(phase=f'Running generation {generation+1} · seed {i+1}/{len(seeds)}')
                    report_file = logs / f'seed_{seed}.json'
                    cmd = [str(ROOT / 'build/market_engine'), '--seconds', str(cfg['seconds']), '--seed', str(seed),
                           '--quiet', '--json', str(report_file)]
                    for spec in specs:
                        cmd += ['--plugin', spec[7:]] if spec.startswith('plugin:') else ['--agent', spec]
                    if cfg['logs']:
                        cmd += ['--trades', str(logs / f'seed_{seed}_trades.csv'),
                                '--cmdlog', str(logs / f'seed_{seed}_commands.csv')]
                    self.log(self.command(cmd, cfg['seconds'] + 15).strip())
                    runs.append(json.loads(report_file.read_text()))
                    self.write_state(sessions_done=generation*cfg['seeds'] + i+1)
                summary = summarise(runs)
                entries = {}
                for i, sub in enumerate(cfg['subjects']):
                    row = summary['agents'][len(BACKGROUND)+i]
                    p = row['pnl']
                    entry = {'generation': generation, 'subject': sub['name'], 'mode': sub['mode'], 'info': cfg['info'],
                             'model': cfg['model'] if sub['mode'] == 'ai' else None, 'ok': True, 'proposal': proposals[sub['name']],
                             'spec': specs[len(BACKGROUND)+i], 'score': p['mean'], 'std': p['std'], 'hit_rate': p['hit_rate'],
                             'median': p['median'], 'min': p['min'], 'max': p['max'], 'fills': row['fills_mean'], 'volume': row['volume_mean']}
                    entries[sub['name']] = entry
                    history.setdefault(sub['name'], []).append({'generation': generation, 'score': p['mean'], 'std': p['std'], 'hit_rate': p['hit_rate']})
                    feedback[sub['name']] = build_report(cfg['info'], len(BACKGROUND)+i, summary, runs, specs)
                    with (self.directory / 'trajectory.jsonl').open('a') as f:
                        f.write(json.dumps(entry) + '\n')
                save(self.directory / f'gen_{generation}.json', {'generation': generation, 'specs': specs, 'seeds': seeds,
                                                              'proposals': entries, 'summary': summary, 'runs': runs})
                self.log(f'Generation {generation+1} complete.')
            self.write_state(status='completed', phase='Experiment complete', finished=time.time())
        except Cancelled:
            self.write_state(status='cancelled', phase='Stopped by user', finished=time.time())
            self.log('Stopped. Completed generations and artifacts are preserved.')
        except Exception as exc:
            message = str(exc).replace(self.api_key, '[redacted]') if self.api_key else str(exc)
            self.write_state(status='failed', phase='Experiment failed', error=message[:2000], finished=time.time())
            self.log(message)
        finally:
            self.api_key = ''
