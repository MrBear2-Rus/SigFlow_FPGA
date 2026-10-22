"""Acceptance CLI never fakes real-tool success when prerequisites are absent."""
from contextlib import redirect_stdout
import importlib,importlib.util,io,json
from pathlib import Path
import tempfile,unittest
from unittest.mock import patch


class SharedAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.owned=Path(__file__).resolve().parents[2]/'.superpowers/sdd/u1-u2-stage32-34-plan'
        self.owned.mkdir(parents=True,exist_ok=True)

    def test_missing_gateway_fails_with_fixed_report_and_no_model_call(self):
        name='sigflow_edu_agent.host.shared_acceptance'
        self.assertIsNotNone(importlib.util.find_spec(name),'formal U1/U2 acceptance missing')
        module=importlib.import_module(name)
        with tempfile.TemporaryDirectory(dir=self.owned) as directory:
            directory=Path(directory);output=directory/'report.json'
            with redirect_stdout(io.StringIO()):
                code=module.main(['--gateway',str(directory/'absent.exe'),'--work-dir',str(directory),
                                  '--output',str(output)])
            self.assertEqual(code,1)
            report=json.loads(output.read_text(encoding='utf-8'))
            self.assertEqual((report['status'],report['error'],report['model_calls']),('failed','gateway_missing',0))
            self.assertNotIn(str(directory),json.dumps(report))

    def test_cleanup_failure_cannot_publish_passed_acceptance(self):
        from contextlib import contextmanager
        module=importlib.import_module('sigflow_edu_agent.host.shared_acceptance')
        with tempfile.TemporaryDirectory(dir=self.owned) as owned:
            directory=Path(owned);binary=directory/'placeholder';binary.write_bytes(b'not executed')
            output=directory/'report.json'
            @contextmanager
            def interrupted_cleanup(**kwargs):
                yield str(directory)
                raise OSError('synthetic cleanup failure '+str(directory))
            with patch.object(module.tempfile,'TemporaryDirectory',interrupted_cleanup), \
                    patch.object(module,'exercise',return_value=dict(status='passed',model_calls=0)), \
                    redirect_stdout(io.StringIO()):
                code=module.main(['--gateway',str(binary),'--output',str(output)])
            result=json.loads(output.read_text(encoding='utf-8'))
            self.assertEqual((code,result['status'],result['error']),(1,'failed','integration_failed'))
            self.assertNotIn('cleanup',result)
            self.assertNotIn(str(directory),json.dumps(result))

    def test_report_write_failure_returns_failed_stdout_and_exit_code(self):
        module=importlib.import_module('sigflow_edu_agent.host.shared_acceptance')
        with tempfile.TemporaryDirectory(dir=self.owned) as owned:
            directory=Path(owned);stream=io.StringIO()
            with redirect_stdout(stream):
                code=module.main(['--gateway',str(directory/'absent.exe'),'--output',str(directory)])
            result=json.loads(stream.getvalue())
            self.assertEqual((code,result['status'],result['error']),(1,'failed','report_write_failed'))
            self.assertNotIn(str(directory),json.dumps(result))
