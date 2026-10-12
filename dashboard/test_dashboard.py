"""Tests use a deterministic stand-in for the model, but the real compiler/engine."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import runner
import server


class DashboardTests(unittest.TestCase):
    def config(self, **changes):
        data = dict(title='Test experiment', prompt='Test bounded market making', generations=1,
                    seeds=2, seconds=.1, logs=True, subjects=[{'name':'control', 'mode':'builtin', 'builtin':'mm'}])
        data.update(changes)
        return runner.validate(data)

    def test_config_validation(self):
        for changes in [{'seeds':float('nan')}, {'generations':1.5}, {'seconds':0}, {'seeds':True},
                        {'subjects':[{'name':'../escape','mode':'custom','source':'x'}]},
                        {'subjects':[{'name':'x','mode':'custom','source':''}]},
                        {'subjects':[{'name':'x','mode':'ai'},{'name':'x','mode':'ai'}]}]:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.config(**changes)
        self.assertNotIn('api_key', runner.validate({**self.config(), 'api_key':'not-saved'}))

    def test_compile_repair_feedback_and_results(self):
        cfg = self.config(generations=2, subjects=[{'name':'writer','mode':'ai'}])
        source = (runner.ROOT / 'plugins/example_breakout.cpp').read_text()
        calls = []
        def generate(system, messages):
            calls.append(json.loads(json.dumps(messages)))
            return 'invalid c++' if len(calls) == 1 else source
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            runner.save(folder/'config.json', cfg)
            job = runner.Job(folder, cfg, 'test-secret-never-persist', generate=generate)
            job.run()
            self.assertEqual(job.state['status'], 'completed', job.state)
            self.assertEqual(job.state['sessions_done'], 4)
            self.assertEqual(len(calls), 3)
            self.assertIn('Fix this failure', calls[1][-1]['content'])
            self.assertIn('Your previous source', calls[2][0]['content'])
            self.assertIn('mean_pnl', calls[2][0]['content'])
            for generation in range(2):
                record = json.loads((folder/f'gen_{generation}.json').read_text())
                self.assertEqual(len(record['runs']), 2)
                self.assertTrue(record['proposals']['writer']['ok'])
            self.assertEqual(len(list(folder.rglob('*_trades.csv'))), 4)
            self.assertEqual(len(list(folder.rglob('*_commands.csv'))), 4)
            for path in folder.rglob('*'):
                if path.is_file() and path.suffix != '.so':
                    self.assertNotIn('test-secret-never-persist', path.read_text())
            self.assertEqual(job.api_key, '')

    def test_compile_failure_stays_failed(self):
        cfg=self.config(subjects=[{'name':'bad','mode':'custom','source':'not c++'}])
        with tempfile.TemporaryDirectory() as tmp:
            job=runner.Job(Path(tmp),cfg)
            job.run()
            self.assertEqual(job.state['status'],'failed')
            self.assertFalse((Path(tmp)/'trajectory.jsonl').exists())
            self.assertIn('did not pass',job.state['error'])

    def test_cancel_preserves_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            job=runner.Job(Path(tmp),self.config())
            job.cancelled.set()
            job.run()
            self.assertEqual(job.state['status'],'cancelled')
            self.assertEqual(job.state['sessions_done'],0)

    def test_artifact_path_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);folder=root/'run';folder.mkdir()
            (root/'secret.txt').write_text('private')
            (folder/'config.json').write_text('{}')
            (folder/'escape.txt').symlink_to(root/'secret.txt')
            self.assertEqual(server.artifact(folder,'config.json'),(folder/'config.json').resolve())
            for path in ['../secret.txt','escape.txt','missing.json']:
                with self.assertRaises(FileNotFoundError):server.artifact(folder,path)

    def test_api_request_and_response(self):
        class Response:
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def read(self):return b'{"content":[{"type":"text","text":"```cpp\\nsource\\n```"}],"stop_reason":"end_turn"}'
        with tempfile.TemporaryDirectory() as tmp:
            job=runner.Job(Path(tmp),self.config(),'test-api-key')
            with patch('runner.urllib.request.urlopen',return_value=Response()) as request:
                self.assertEqual(job.generate_source('system',[{'role':'user','content':'brief'}]),'source')
                req=request.call_args.args[0]
                self.assertEqual(req.full_url,'https://api.anthropic.com/v1/messages')
                self.assertEqual(req.get_header('X-api-key'),'test-api-key')
                self.assertEqual(json.loads(req.data)['messages'][0]['content'],'brief')


if __name__=='__main__':
    unittest.main(verbosity=2)
