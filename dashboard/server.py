"""Dependency-free local dashboard: python3 dashboard/server.py."""
import argparse
import io
import json
import mimetypes
import os
from pathlib import Path
import re
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlsplit
import zipfile

from runner import Job, ROOT, save, validate

RESULTS = ROOT / 'results'
STATIC = Path(__file__).resolve().parent
TOKEN = secrets.token_urlsafe(32)
LOCK = threading.Lock()
ACTIVE = None
ALLOWED = {'.json', '.jsonl', '.csv', '.cpp', '.md', '.log', '.txt'}
LABELS = {'exp1_info': 'Information & adaptation', 'exp2_persona': 'The persona experiment',
          'exp3_code': 'From prompts to C++', 'exp4_model': 'Model comparison',
          'exp5_freecode': 'Open-ended strategy design', 'exp6_latency': 'The latency pilot',
          'exp7_hft': 'Inside the execution loop', 'exp8_hft_scratch': 'HFT from a blank page',
          'exp9_field_a': 'Cheap Seats · field A', 'exp9_field_b': 'Cheap Seats · field B',
          'exp9_field_c': 'Cheap Seats · mixed field', 'ctrl_bandit': 'Bandit control',
          'ctrl_hillclimb': 'Hill-climbing control', 'ctrl_imitate': 'Imitation control'}


def read_json(path, default=None):
    if not path.exists():
        return default
    return json.loads(path.read_text())


def directory(name):
    if not re.fullmatch(r'[A-Za-z0-9_-]+', name):
        raise ValueError('Invalid experiment ID.')
    path = (RESULTS / name).resolve()
    if path.parent != RESULTS.resolve() or not path.is_dir():
        raise FileNotFoundError('Experiment not found.')
    return path


def artifact(folder, relative):
    path = (folder / relative).resolve()
    if not path.is_relative_to(folder.resolve()) or not path.is_file() or path.suffix not in ALLOWED:
        raise FileNotFoundError('Artifact not found.')
    return path


def trajectory(folder):
    path = folder / 'trajectory.jsonl'
    rows = []
    if path.exists():
        for line in path.read_text().splitlines():
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                pass  # A polling read can meet the writer halfway through a line.
    return rows


def overview(folder):
    cfg = read_json(folder / 'config.json', {})
    rows = trajectory(folder)
    state = read_json(folder / 'dashboard.json', {})
    evaluated = len({r['generation'] for r in rows})
    status = state.get('status')
    if not status:
        historical_state = read_json(folder / 'state.json', {})
        status = 'completed' if historical_state.get('done') or evaluated >= cfg.get('generations', 1) else 'partial'
    last = [r for r in rows if r['generation'] == max((x['generation'] for x in rows), default=-1)]
    leader = max(last, key=lambda r: r['score']) if last else None
    curve = [r['score'] for r in rows if leader and r['subject'] == leader['subject']]
    return {'curve': curve, 'leader': leader['subject'] if leader else None, 'id': folder.name, 'title': cfg.get('title') or LABELS.get(folder.name, folder.name.replace('_', ' ')),
            'status': status, 'generations': cfg.get('generations', evaluated), 'evaluated': evaluated,
            'subjects': len(cfg.get('subjects', [])) or len({r['subject'] for r in rows}),
            'seeds': cfg.get('seeds', 0), 'seconds': cfg.get('seconds', 0),
            'created': state.get('created', folder.stat().st_mtime), 'local': bool(cfg.get('dashboard')),
            'kind': 'Control' if folder.name.startswith('ctrl_') else 'AI research', 'state': state}


def detail(folder):
    info = overview(folder)
    info['config'] = read_json(folder / 'config.json', {})
    info['rows'] = trajectory(folder)
    gens = sorted(folder.glob('gen_*.json'), key=lambda p: int(p.stem.split('_')[-1]))
    info['latest'] = read_json(gens[-1], {}) if gens else {}
    # Raw per-seed reports are downloaded separately, keeping UI responses small.
    info['latest'].pop('runs', None)
    info['files'] = [str(p.relative_to(folder)) for p in sorted(folder.rglob('*'))
                     if p.is_file() and not p.is_symlink() and p.suffix in ALLOWED]
    log = folder / 'activity.log'
    info['log'] = log.read_text()[-24000:] if log.exists() else 'Historical run. Open generation reports and source files for the recorded evidence.'
    return info


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def respond(self, data, status=200, content_type='application/json', filename=None):
        if content_type == 'application/json':
            data = json.dumps(data, allow_nan=False).encode()
        elif isinstance(data, str):
            data = data.encode()
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'")
        if filename:
            self.send_header('Content-Disposition', f'attachment; filename="{filename}"')
        self.end_headers()
        self.wfile.write(data)

    def trusted(self, mutation=False):
        host = self.headers.get('Host', '')
        expected = {f'127.0.0.1:{self.server.server_port}', f'localhost:{self.server.server_port}'}
        if host not in expected:
            raise PermissionError('Local access only.')
        origin = self.headers.get('Origin')
        if origin and origin != f'http://{host}':
            raise PermissionError('Cross-origin requests are not allowed.')
        if mutation and not secrets.compare_digest(self.headers.get('X-Dashboard-Token', ''), TOKEN):
            raise PermissionError('Reload the dashboard before starting a run.')

    def do_GET(self):
        try:
            self.trusted()
            url = urlsplit(self.path)
            parts = unquote(url.path).strip('/').split('/')
            query = parse_qs(url.query)
            if url.path == '/api/config':
                return self.respond({'token': TOKEN, 'key_configured': bool(os.getenv('ANTHROPIC_API_KEY')),
                                     'model': os.getenv('MARKET_MODEL', 'claude-opus-5'),
                                     'example': (ROOT / 'plugins/example_breakout.cpp').read_text()})
            if url.path == '/api/runs':
                runs = []
                for folder in RESULTS.iterdir():
                    if folder.is_dir() and not folder.is_symlink() and ((folder / 'trajectory.jsonl').exists() or (folder / 'dashboard.json').exists()):
                        try:
                            runs.append(overview(folder))
                        except (ValueError, OSError, KeyError):
                            continue
                return self.respond(sorted(runs, key=lambda x: (x['local'], x['created']), reverse=True))
            if len(parts) >= 3 and parts[:2] == ['api', 'runs']:
                folder = directory(parts[2])
                if len(parts) == 3:
                    return self.respond(detail(folder))
                if len(parts) == 4 and parts[3] == 'file':
                    path = artifact(folder, query.get('path', [''])[0])
                    if path.stat().st_size > 2_000_000:
                        raise ValueError('This file is too large to preview. Download the result bundle.')
                    return self.respond({'path': str(path.relative_to(folder)), 'text': path.read_text(errors='replace')})
                if len(parts) == 4 and parts[3] == 'download':
                    kind = query.get('kind', ['all'])[0]
                    if kind not in {'all', 'logs', 'sources', 'results'}:
                        raise ValueError('Unknown download type.')
                    buffer = io.BytesIO()
                    with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as archive:
                        for path in sorted(folder.rglob('*')):
                            if not path.is_file() or path.is_symlink() or not path.resolve().is_relative_to(folder.resolve()) or path.suffix not in ALLOWED:
                                continue
                            if kind == 'logs' and path.suffix not in {'.csv', '.log'}:
                                continue
                            if kind == 'sources' and path.suffix not in {'.cpp', '.md'}:
                                continue
                            if kind == 'results' and path.suffix not in {'.json', '.jsonl'}:
                                continue
                            archive.write(path, str(path.relative_to(folder)))
                    return self.respond(buffer.getvalue(), content_type='application/zip', filename=f'{folder.name}-{kind}.zip')
            paths = {'/': 'index.html', '/app.js': 'app.js', '/style.css': 'style.css'}
            if url.path in paths:
                p = STATIC / paths[url.path]
                return self.respond(p.read_bytes(), content_type=mimetypes.guess_type(p.name)[0] or 'text/plain')
            raise FileNotFoundError('Not found.')
        except (ValueError, FileNotFoundError, PermissionError) as exc:
            self.respond({'error': str(exc)}, 403 if isinstance(exc, PermissionError) else 404 if isinstance(exc, FileNotFoundError) else 400)
        except Exception:
            self.respond({'error': 'Could not read experiment data.'}, 500)

    def do_POST(self):
        global ACTIVE
        try:
            self.trusted(mutation=True)
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= 1_000_000:
                raise ValueError('Invalid request size.')
            data = json.loads(self.rfile.read(length))
            parts = urlsplit(self.path).path.strip('/').split('/')
            if parts == ['api', 'runs']:
                cfg = validate(data)
                key = str(data.get('api_key') or os.getenv('ANTHROPIC_API_KEY', '')).strip()
                if any(s['mode'] == 'ai' for s in cfg['subjects']) and not key:
                    raise ValueError('Add an Anthropic API key in Connection, or use built-in/custom strategies.')
                with LOCK:
                    if ACTIVE and ACTIVE.state['status'] in {'queued', 'running'}:
                        raise ValueError('An experiment is already running. Stop it or wait for completion.')
                    name = time.strftime('lab-%Y%m%d-%H%M%S-') + secrets.token_hex(3)
                    folder = RESULTS / name
                    folder.mkdir()
                    save(folder / 'config.json', cfg)
                    ACTIVE = Job(folder, cfg, key)
                    threading.Thread(target=ACTIVE.run, daemon=True).start()
                return self.respond({'id': name}, 201)
            if len(parts) == 4 and parts[:2] == ['api', 'runs'] and parts[3] == 'cancel':
                with LOCK:
                    if not ACTIVE or ACTIVE.directory.name != parts[2] or ACTIVE.state['status'] not in {'queued', 'running'}:
                        raise ValueError('This experiment is not running.')
                    ACTIVE.cancelled.set()
                return self.respond({'ok': True})
            raise FileNotFoundError('Not found.')
        except (ValueError, FileNotFoundError, PermissionError) as exc:
            self.respond({'error': str(exc)}, 403 if isinstance(exc, PermissionError) else 404 if isinstance(exc, FileNotFoundError) else 400)
        except Exception:
            self.respond({'error': 'Could not start experiment.'}, 500)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8765)
    args = parser.parse_args()
    RESULTS.mkdir(exist_ok=True)
    for path in RESULTS.glob('lab-*/dashboard.json'):
        state = read_json(path)
        if state.get('status') in {'queued', 'running'}:
            state.update(status='interrupted', phase='Server stopped before completion')
            save(path, state)
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    print(f'Market Lab: http://127.0.0.1:{server.server_port}', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        if ACTIVE:
            ACTIVE.cancelled.set()
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
