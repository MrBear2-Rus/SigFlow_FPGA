"""Runner safety tests; real EDA acceptance is the explicit CLI, never a fixture."""
from contextlib import redirect_stdout,redirect_stderr
import io
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


class ClosedLoopRunnerTests(unittest.TestCase):
    def test_wave_cli_requires_simulation_and_forwards_explicit_flag(self):
        runner=self.module()
        with tempfile.TemporaryDirectory() as owned:
            gateway=Path(owned)/'gateway.exe';gateway.touch();seen=[]
            def execute(*args,**kwargs):
                seen.append(kwargs);return dict(status='passed',wave_observation=dict(exact_window_verified=True))
            with patch.object(runner,'exercise',side_effect=execute),redirect_stdout(io.StringIO()):
                result=runner.main(['--gateway',str(gateway),'--scenario','simulation','--waves','--output',str(Path(owned)/'r.json')])
            self.assertEqual(result,0);self.assertEqual(seen,[dict(scenario='simulation',waves=True)])
        with self.assertRaises(SystemExit):runner.main(['--gateway','missing','--waves','--output','unused'])

    def module(self):
        self.assertIsNotNone(importlib.util.find_spec('sigflow_edu_agent.host.closed_loop'),
                             'closed-loop runner must exist')
        from sigflow_edu_agent.host import closed_loop
        return closed_loop

    def test_rule_environment_cannot_inherit_model_or_anchor_configuration(self):
        runner = self.module()
        inherited = {'PATH': 'kept', 'PYTHONPATH': 'legacy', 'SIGFLOW_U2_ANCHORS': 'untrusted',
                     'DEEPSEEK_API_KEY': 'secret', 'DEEPSEEK_BASE_URL': 'private', 'DEEPSEEK_MODEL': 'other'}
        env = runner.sidecar_environment(Path('C:/owned'), 'rule', inherited)
        self.assertEqual(env['PATH'], 'kept')
        self.assertNotIn('SIGFLOW_U2_ANCHORS', env)
        self.assertFalse(any(k.startswith('DEEPSEEK_') for k in env))
        self.assertNotEqual(env['PYTHONPATH'], 'legacy')
        self.assertTrue(env['SIGFLOW_DEEPSEEK_CONFIG'].endswith('missing-model.json'))

    def test_student_edit_cli_selects_revision_scenario_and_preserves_facts(self):
        runner = self.module()
        with tempfile.TemporaryDirectory() as owned:
            directory = Path(owned)
            gateway = directory / 'gateway.exe'
            gateway.touch()
            seen = []
            # The external Core/model run is the slow boundary. Exercise main's
            # real option selection, cleanup and sanitized report publication.
            def execute(path, mode, timeout, temporary, *, scenario='same-revision'):
                seen.append((scenario, mode, path))
                return dict(status='passed', real_core=True, real_yosys=True,
                            job_count=4, student_edit=dict(new_approval='verified'))
            capture = io.StringIO()
            with patch.object(runner, 'exercise', side_effect=execute), redirect_stdout(capture):
                result = runner.main(['--gateway', str(gateway), '--scenario', 'student-edit',
                                      '--output', str(directory / 'report.json')])
            report = json.loads(capture.getvalue())
            self.assertEqual(result, 0)
            self.assertEqual(seen, [('student-edit', 'rule', gateway.resolve())])
            self.assertEqual(report['student_edit']['new_approval'], 'verified')
            self.assertEqual((report['job_count'], report['cleanup']), (4, 'completed'))

    def test_course_cli_requires_explicit_enable_and_forwards_trusted_database(self):
        runner = self.module()
        with tempfile.TemporaryDirectory() as owned:
            directory = Path(owned)
            gateway = directory / 'gateway.exe'; gateway.touch()
            database = directory / 'course.sqlite'
            seen = []
            def execute(*args, **kwargs):
                seen.append(kwargs)
                return dict(status='passed', real_core=True, real_yosys=True,
                            course=dict(metadata_verified=True, reference_count=1))
            capture = io.StringIO()
            with patch.object(runner, 'exercise', side_effect=execute), redirect_stdout(capture):
                result = runner.main(['--gateway', str(gateway), '--courses', '--course-db', str(database),
                                      '--output', str(directory / 'report.json')])
            self.assertEqual(result, 0)
            self.assertEqual(seen[0]['course_db'], database.resolve())
            self.assertIs(seen[0]['courses'], True)
            self.assertTrue(json.loads(capture.getvalue())['course']['metadata_verified'])

    def test_missing_gateway_fails_sanitized_and_never_counts_as_success(self):
        runner = self.module()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'report.json'
            result = runner.main(['--gateway', str(Path(directory) / 'secret-missing.exe'),
                                  '--output', str(output), '--timeout', '1'])
            report = json.loads(output.read_text())
            self.assertEqual(result, 1)
            self.assertEqual(report['status'], 'failed')
            self.assertEqual(report['error'], 'gateway_missing')
            self.assertNotIn(directory, output.read_text())
            self.assertNotIn('secret-missing', output.read_text())

    def test_simulation_cli_selects_explicit_rule_scenario(self):
        runner = self.module()
        with tempfile.TemporaryDirectory() as owned:
            directory = Path(owned)
            gateway = directory/'gateway.exe'; gateway.touch()
            seen = []
            def execute(path, mode, timeout, temporary, **options):
                seen.append((mode, options))
                return dict(status='passed', real_core=True, real_verilator=True, job_count=3)
            with patch.object(runner, 'exercise', side_effect=execute), redirect_stdout(io.StringIO()):
                result = runner.main(['--gateway',str(gateway),'--scenario','simulation',
                                      '--output',str(directory/'result.json')])
            self.assertEqual((result, seen), (0, [('rule', {'scenario':'simulation'})]))

    def test_simulation_rejects_model_or_course_combinations_before_execution(self):
        runner = self.module()
        for extra in (['--mode','deepseek'], ['--courses']):
            with self.subTest(extra=extra), patch.object(runner, 'exercise') as execute:
                with self.assertRaises(SystemExit) as error:
                    runner.main(['--gateway','missing','--scenario','simulation','--output','unused',*extra])
                self.assertEqual(error.exception.code, 2)
                execute.assert_not_called()

    def test_stalled_child_ready_is_bounded_and_cleanup_joins_own_process(self):
        runner = self.module()
        child = runner.Child([sys.executable, '-c', 'import time; time.sleep(30)'], os.environ.copy())
        start = time.monotonic()
        try:
            with self.assertRaisesRegex(runner.RunFailure, 'ready_timeout'):
                child.ready(time.monotonic() + .15)
        finally:
            child.close(grace=.1)
        self.assertIsNotNone(child.process.poll())
        self.assertLess(time.monotonic() - start, 3)

    def test_temp_cleanup_fault_cannot_leave_passed_acceptance(self):
        runner = self.module()
        with tempfile.TemporaryDirectory() as owned:
            directory = Path(owned)
            gateway = directory / 'gateway.exe'
            gateway.touch()
            class BrokenCleanup:
                def __enter__(self):
                    return owned
                def __exit__(self, *args):
                    raise PermissionError('private cleanup location')
            capture = io.StringIO()
            facts = dict(status='passed', real_core=True, real_yosys=True, job_count=2)
            with patch.object(runner, 'exercise', return_value=facts), patch.object(
                    runner.tempfile, 'TemporaryDirectory', return_value=BrokenCleanup()), redirect_stdout(capture):
                result = runner.main(['--gateway', str(gateway), '--output', str(directory / 'report.json')])
            report = json.loads(capture.getvalue())
            self.assertEqual(result, 1)
            self.assertEqual(report['status'], 'failed')
            self.assertNotEqual(report.get('cleanup'), 'completed')
            self.assertTrue(report['execution']['real_core'])
            self.assertEqual(report['execution']['job_count'], 2)
            self.assertNotIn('private cleanup', capture.getvalue())

    def test_invalid_output_sink_emits_only_sanitized_failed_json(self):
        runner = self.module()
        with tempfile.TemporaryDirectory() as owned:
            capture = io.StringIO()
            with redirect_stdout(capture):
                result = runner.main(['--gateway', str(Path(owned)/'missing'), '--output', owned])
            report = json.loads(capture.getvalue())
            self.assertEqual(result, 1)
            self.assertEqual((report['status'], report['error']), ('failed', 'report_write_failed'))
            self.assertNotIn(owned, capture.getvalue())
            self.assertNotIn('Traceback', capture.getvalue())

    def test_descendant_pipes_cannot_extend_close_and_other_child_is_still_cleaned(self):
        self.module()
        with tempfile.TemporaryDirectory() as owned:
            result_path, done = Path(owned)/'result.json', Path(owned)/'descendant-done'
            script = r"""
import json,os,sys,time
from pathlib import Path
from sigflow_edu_agent.host.closed_loop import Child, RunFailure, close_children
result_path,done = map(Path,sys.argv[1:])
leaf = 'import time; from pathlib import Path; time.sleep(5); Path('+repr(str(done))+').touch()'
program = 'import subprocess,sys; subprocess.Popen([sys.executable,"-c",'+repr(leaf)+']); print("{}",flush=True); sys.stdin.read()'
first=Child([sys.executable,'-c',program],os.environ.copy())
second=Child([sys.executable,'-c','import sys; print("{}",flush=True); sys.stdin.read()'],os.environ.copy())
first.ready(time.monotonic()+3)
second.ready(time.monotonic()+3)
start=time.monotonic()
try:
    close_children(((first,.1),(second,.1)))
    code='unexpected_success'
except RunFailure as error:
    code=str(error)
result_path.write_text(json.dumps(dict(code=code,seconds=time.monotonic()-start,
    first_exit=first.process.poll(),second_exit=second.process.poll())))
# Finite descendant exits normally; never leave a process after the test.
limit=time.monotonic()+7
while not done.exists() and time.monotonic()<limit:
    time.sleep(.05)
for thread in first.threads+second.threads:
    thread.join(timeout=1)
assert done.exists()
"""
            result = subprocess.run([sys.executable,'-c',script,str(result_path),str(done)],
                                    capture_output=True,timeout=15,env=os.environ.copy(),
                                    creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
            self.assertEqual(result.returncode, 0, 'isolated child regression did not complete')
            report = json.loads(result_path.read_text())
            self.assertLess(report['seconds'], 2.5)
            self.assertEqual(report['code'], 'child_drain_timeout')
            self.assertEqual((report['first_exit'],report['second_exit']), (0,0))
            self.assertTrue(done.exists())

    def test_wrong_protocol_instance_and_nonce_are_rejected(self):
        runner = self.module()
        valid = dict(type='ready', protocol='edu.api.v1', instance_id='inst-test', nonce='a'*64,
                     project_id='project-test', port=12345)
        runner.check_ready(valid, 'inst-test', 'a'*64, gateway=True)
        for key, value in [('protocol','foreign'), ('instance_id','foreign'), ('nonce','wrong'), ('port',True)]:
            with self.subTest(key=key), self.assertRaises(runner.RunFailure):
                runner.check_ready(valid | {key:value}, 'inst-test', 'a'*64, gateway=True)


    def test_prediction_requires_explicit_simulation_wave_mode(self):
        runner=self.module()
        with tempfile.TemporaryDirectory() as owned:
            output=str(Path(owned)/'report.json')
            for extra in (['--predictions'],['--predictions','--scenario','simulation']):
                with redirect_stderr(io.StringIO()),self.assertRaises(SystemExit) as raised:
                    runner.main(['--gateway',owned,'--output',output,*extra])
                self.assertEqual(raised.exception.code,2)
            gateway=Path(owned)/'gateway';gateway.touch();seen=[]
            with patch.object(runner,'exercise',side_effect=lambda *a,**kw:seen.append(kw) or dict(status='passed')),redirect_stdout(io.StringIO()):
                self.assertEqual(runner.main(['--gateway',str(gateway),'--output',output,'--scenario','simulation','--waves','--predictions']),0)
            self.assertEqual(seen,[dict(scenario='simulation',waves=True,predictions=True)])

if __name__ == '__main__':
    unittest.main()
