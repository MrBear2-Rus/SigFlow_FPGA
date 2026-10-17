import http.client
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from queue import Queue
from threading import Thread
import unittest
from unittest.mock import patch
from _model_isolation import isolate_model_settings


class HostProcessTests(unittest.TestCase):
    def setUp(self):
        isolate_model_settings(self)

    def bootstrap(self, directory):
        return dict(type='sigflow-bootstrap', protocol='edu.api.v1', instance_id='inst-process',
                    nonce='a' * 64, gateway_url='http://127.0.0.1:12345',
                    gateway_token='gateway-private', ui_token='ui-private', data_directory=directory)

    def run_process(self, *, launcher=False, extra=()):
        package = str(Path(__file__).resolve().parents[1])
        env = os.environ | {'PYTHONPATH': package}
        env.pop('DEEPSEEK_API_KEY', None)
        command = [sys.executable, '-m', 'sigflow_edu_agent.host', '--bootstrap-stdin']
        cwd = package
        if launcher:
            env['SIGFLOW_EDU_AGENT_PYTHON'] = sys.executable
            command = ['cmd.exe', '/d', '/c', str(Path(package) / 'bin/sigflow-u2-host.cmd'), '--bootstrap-stdin']
            shadow = tempfile.TemporaryDirectory()
            self.addCleanup(shadow.cleanup)
            conflicting_package = Path(shadow.name) / 'sigflow_edu_agent'
            conflicting_package.mkdir()
            (conflicting_package / '__init__.py').write_text("raise SystemExit('wrong_package_selected')\n", encoding='utf-8')
            cwd = shadow.name
        command.extend(extra)
        return subprocess.Popen(command,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                cwd=cwd, env=env)

    def ready_line(self, process):
        ready = Queue()
        Thread(target=lambda: ready.put(process.stdout.readline()), daemon=True).start()
        return ready.get(timeout=5)

    def test_process_ready_health_shutdown_and_single_stdout_line(self):
        self.exercise_process()

    @unittest.skipUnless(os.name == 'nt', 'Windows launcher exercised only on Windows')
    def test_windows_launcher_avoids_other_package_in_working_directory(self):
        self.exercise_process(launcher=True)

    def exercise_process(self, *, launcher=False):
        self.assertIsNotNone(importlib.util.find_spec('sigflow_edu_agent.host.__main__'), 'host process missing')
        with tempfile.TemporaryDirectory() as directory:
            process = self.run_process(launcher=launcher)
            self.addCleanup(lambda: process.kill() if process.poll() is None else None)
            process.stdin.write(json.dumps(self.bootstrap(directory)).encode() + b'\n')
            process.stdin.flush()
            ready_raw = self.ready_line(process)
            self.assertTrue(ready_raw, 'launcher selected conflicting working-directory package')
            ready = json.loads(ready_raw)
            self.assertEqual((ready['type'], ready['protocol'], ready['nonce']), ('ready', 'edu.api.v1', 'a' * 64))
            self.assertGreater(ready['port'], 0)
            conn = http.client.HTTPConnection('127.0.0.1', ready['port'], timeout=3)
            conn.request('GET', '/api/v1/health')
            response = conn.getresponse()
            self.assertEqual(response.status, 200)
            self.assertEqual(json.loads(response.read())['data']['instance_id'], 'inst-process')
            conn.close()
            conn = http.client.HTTPConnection('127.0.0.1', ready['port'], timeout=3)
            conn.request('POST', '/api/v1/shutdown', b'{}', {'Authorization': 'Bearer ui-private', 'Content-Type': 'application/json'})
            response = conn.getresponse()
            self.assertEqual(response.status, 202)
            response.read()
            conn.close()
            output, errors = process.communicate(timeout=6)
            self.assertEqual(process.returncode, 0, errors)
            self.assertEqual(output, b'')
            self.assertNotIn(b'ui-private', errors)
            self.assertNotIn(b'gateway-private', errors)
            files = [p for p in (Path(directory) / 'u2-host').glob('*.sqlite3') if '-verification' not in p.name]
            self.assertEqual(len(files), 1)
            self.assertEqual(len(list((Path(directory) / 'u2-host').glob('*-verification.sqlite3'))), 1)
            for item in Path(directory).rglob('*'):
                if item.is_file():
                    raw = item.read_bytes()
                    self.assertNotIn(b'ui-private', raw)
                    self.assertNotIn(b'gateway-private', raw)

    def test_bad_bootstrap_fails_sanitized_with_no_ready(self):
        self.assertIsNotNone(importlib.util.find_spec('sigflow_edu_agent.host.__main__'), 'host process missing')
        with tempfile.TemporaryDirectory() as directory:
            payload = self.bootstrap(directory)
            cases = [payload | {'gateway_url': 'http://example.com:80'},
                     payload | {'gateway_url': 'http://user:password@127.0.0.1:80'},
                     payload | {'gateway_url': 'http://127.0.0.1:80/x'},
                     payload | {'nonce': 'short'}, payload | {'extra': 'secret'},
                     payload | {'ui_token': True}, payload | {'protocol': 'wrong'}]
            for record in cases:
                process = self.run_process()
                output, errors = process.communicate(json.dumps(record).encode() + b'\n', timeout=5)
                self.assertEqual(process.returncode, 2)
                self.assertEqual(output, b'')
                self.assertNotIn(b'private', errors)
                self.assertNotIn(directory.encode(), errors)

    def test_course_configuration_is_trusted_startup_only_and_path_free(self):
        for extra, enabled in (((), True), (('--no-course',), False)):
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as directory:
                process = self.run_process(extra=extra)
                self.addCleanup(lambda p=process: p.kill() if p.poll() is None else None)
                process.stdin.write(json.dumps(self.bootstrap(directory)).encode() + b'\n'); process.stdin.flush()
                ready = json.loads(self.ready_line(process))
                connection = http.client.HTTPConnection('127.0.0.1', ready['port'], timeout=3)
                connection.request('GET', '/api/v1/capabilities', headers={'Authorization': 'Bearer ui-private'})
                response = connection.getresponse(); raw = response.read(); connection.close()
                self.assertEqual(response.status, 200)
                self.assertEqual(json.loads(raw)['data']['course'],
                                 dict(available=enabled, reason='configured' if enabled else 'disabled'))
                self.assertNotIn(directory.encode(), raw)
                connection = http.client.HTTPConnection('127.0.0.1', ready['port'], timeout=3)
                connection.request('POST', '/api/v1/shutdown', b'{}',
                    {'Authorization': 'Bearer ui-private', 'Content-Type': 'application/json'})
                response = connection.getresponse(); response.read(); connection.close()
                self.assertEqual(response.status, 202)
                output, errors = process.communicate(timeout=6)
                self.assertEqual((process.returncode, output), (0, b''), errors)

    def test_environment_anchor_catalog_is_checked_before_ready(self):
        with tempfile.TemporaryDirectory() as directory:
            catalog = Path(directory) / 'anchors.json'
            catalog.write_text('{"anchors":[{"bad":"field"}]}', encoding='utf-8')
            with patch.dict(os.environ, {'SIGFLOW_U2_ANCHORS': str(catalog)}):
                process = self.run_process()
            try:
                output, errors = process.communicate(json.dumps(self.bootstrap(directory)).encode() + b'\n', timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                self.fail('environment anchor catalog was ignored; host incorrectly became ready')
            self.assertEqual(process.returncode, 2)
            self.assertEqual(output, b'')
            self.assertNotIn(directory.encode(), errors)


if __name__ == '__main__':
    unittest.main()
