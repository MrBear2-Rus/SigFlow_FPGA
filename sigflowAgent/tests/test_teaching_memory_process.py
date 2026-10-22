"""Formal host process/HTTP boundary with synthetic external contract inputs."""
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import http.client
import json
import os
from pathlib import Path
from queue import Queue
import sqlite3
import subprocess
import sys
import tempfile
from threading import Thread
import time
import unittest

from _model_isolation import isolate_model_settings


class TeachingMemoryProcessTests(unittest.TestCase):
    def setUp(self):
        isolate_model_settings(self)
        self.package = Path(__file__).resolve().parents[1]
        self.directory = tempfile.TemporaryDirectory(dir=self.package)
        self.addCleanup(self.directory.cleanup)
        self.log = Path(self.directory.name)/'boundary.jsonl'
        self.process = None
        self.addCleanup(self.stop_process)
        self.gateway_requests = []
        owner = self
        report = dict(schema_version='edu.jobreport.v1', project_id='p', revision='r', snapshot_id='s',
            job_id='j', origin='core', capability='eda.synth', plugin_id='eda-synth-yosys',
            plugin_version='1', state='Succeeded', exit_code=0, completeness='complete',
            input_fingerprint='input-hash', diagnostics=[], metrics={},
            artifacts=[dict(artifact_id='a', sha256='a'*64, schema='netlist.v1', role='primary')])
        metadata = dict(artifact_id='a', project_id='p', revision='r', job_id='j', sha256='a'*64,
            schema='netlist.v1', role='netlist', size='123', media_type='application/json', inline_readable=True)

        class GatewayHandler(BaseHTTPRequestHandler):
            def do_GET(self):
                owner.gateway_requests.append((self.path, self.headers.get('Authorization')))
                if self.path == '/api/v1/health':
                    data = dict(protocol_version='edu.api.v1', instance_id='inst', edition='edu')
                elif self.headers.get('Authorization') != 'Bearer gateway-probe':
                    self.send_error(401)
                    return
                elif self.path == '/api/v1/jobs/j/report':
                    data = report
                elif self.path == '/api/v1/artifacts/a':
                    data = metadata
                else:
                    self.send_error(404)
                    return
                raw = json.dumps(dict(schema_version='edu.api.v1', request_id='req', trace_id='trace', data=data)).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def log_message(self, *args):
                pass

        self.gateway = ThreadingHTTPServer(('127.0.0.1', 0), GatewayHandler)
        Thread(target=self.gateway.serve_forever, kwargs={'poll_interval': .01}, daemon=True).start()
        self.addCleanup(self.gateway.server_close)
        self.addCleanup(self.gateway.shutdown)

    def stop_process(self):
        if self.process is not None and self.process.poll() is None:
            self.process.kill()
            self.process.communicate(timeout=5)

    def start(self, mode):
        environment = dict(os.environ, PYTHONPATH=str(self.package))
        environment.pop('DEEPSEEK_API_KEY', None)
        environment.pop('SIGFLOW_U2_ANCHORS', None)
        self.process = subprocess.Popen([sys.executable, '-P', str(self.package/'tests/helpers/teaching_memory_host.py'),
            mode, str(self.log)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            cwd=self.package, env=environment)
        bootstrap = dict(type='sigflow-bootstrap', protocol='edu.api.v1', instance_id='inst', nonce='a'*64,
            gateway_url=f'http://127.0.0.1:{self.gateway.server_port}', gateway_token='gateway-probe',
            ui_token='ui-probe', data_directory=self.directory.name)
        self.process.stdin.write(json.dumps(bootstrap).encode()+b'\n')
        self.process.stdin.flush()
        ready = Queue()
        Thread(target=lambda: ready.put(self.process.stdout.readline()), daemon=True).start()
        raw = ready.get(timeout=5)
        self.assertTrue(raw, 'trusted adapter did not reach the formal ready boundary')
        ready = json.loads(raw)
        self.assertEqual((ready['type'], ready['protocol'], ready['nonce']), ('ready', 'edu.api.v1', 'a'*64))
        self.port = ready['port']

    def request(self, method, path, body=None, **headers):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=3)
        headers = {'Authorization': 'Bearer ui-probe', 'X-Learner-ID': 'learner'} | headers
        if body is not None:
            body = json.dumps(body).encode()
            headers['Content-Type'] = 'application/json'
        try:
            connection.request(method, path, body, headers)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def poll(self, read, accept):
        deadline = time.monotonic()+5
        while time.monotonic() < deadline:
            value = read()
            if accept(value):
                return value
            time.sleep(.02)
        self.fail('host boundary did not reach expected committed outcome')

    def records(self, kind):
        if not self.log.exists():
            return []
        # A writer may be appending its final line. Never accept incomplete data.
        lines = self.log.read_bytes().splitlines(keepends=True)
        values = [json.loads(line) for line in lines if line.endswith(b'\n')]
        return [v['data'] for v in values if v['kind'] == kind]

    def state(self, sid):
        code, result = self.request('GET', f'/api/v1/teaching/sessions/{sid}/state')
        self.assertEqual(code, 200, result)
        return result['data']

    def exercise(self, mode):
        self.start(mode)
        code, caps = self.request('GET', '/api/v1/capabilities')
        self.assertEqual(code, 200)
        self.assertTrue(caps['data']['teaching_memory']['configured'])
        body = dict(learner_id='learner', project_id='p', revision='r', snapshot_id='s', job_id='j',
            diagnostic_id='job-report', issue_id='issue', question='private-student-question', mode='deepseek')
        code, result = self.request('POST', '/api/v1/teaching/sessions', body | {'teaching_memory': 'injected'},
                                    **{'Idempotency-Key': 'reject'})
        self.assertEqual(code, 400, result)
        code, result = self.request('POST', '/api/v1/teaching/sessions', body, **{'Idempotency-Key': 'create'})
        self.assertEqual(code, 202, result)
        sid = result['data']['session_id']
        first = self.poll(lambda: self.state(sid), lambda value: value['status'] == 'WaitingStudent')
        action = dict(kind='hint_next', action_id='hint', expected_version=first['state_version'])
        path = f'/api/v1/teaching/sessions/{sid}/actions'
        code, result = self.request('POST', path, action)
        self.assertEqual(code, 202, result)
        second = self.poll(lambda: self.state(sid), lambda value: value['level'] == 2)
        code, result = self.request('POST', path, action)
        self.assertEqual(code, 202, result)
        code, result = self.request('POST', f'/api/v1/teaching/sessions/{sid}/actions',
            dict(kind='finish', action_id='finish', expected_version=second['state_version']))
        self.assertEqual(code, 202, result)
        final = self.poll(lambda: self.state(sid), lambda value: value['status'] == 'Completed')
        code, _ = self.request('GET', f'/api/v1/teaching/sessions/{sid}/state', **{'X-Learner-ID': 'foreign'})
        self.assertEqual(code, 403)
        facts = self.poll(lambda: self.records('fact'), lambda values: len(values) >= 7)
        self.assertEqual(facts[0], facts[1], 'wrong ACK must retry exactly the committed event')
        unique = list({f['event_id']: f for f in facts}.values())
        self.assertEqual([f['event_type'] for f in unique], ['run_created', 'card_generated',
            'student_action_accepted', 'card_generated', 'student_action_accepted', 'run_finished'])
        self.assertEqual([f['level'] for f in unique if f['event_type'] == 'card_generated'], [1, 2])
        for fact in unique:
            self.assertEqual(fact['schema_version'], 'edu.u2.teaching-fact.v1')
            self.assertEqual((fact['instance_id'], fact['learner_id'], fact['project_id'], fact['session_id']),
                             ('inst', 'learner', 'p', sid))
            self.assertEqual((fact['revision'], fact['snapshot_id'], fact['job_id'], fact['evidence_origin']),
                             ('r', 's', 'j', 'tool'))
        raw = json.dumps(unique)
        for private in ('private-student-question', 'history-probe', 'synthetic-model-key', 'ui-probe', 'gateway-probe'):
            self.assertNotIn(private, raw)
        history = self.records('history')
        self.assertEqual(history, [dict(learner_id='learner', project_id='p', concept_ids=['job_observation'], max_bytes=1024)]*2)
        self.assertEqual([r['history'] for r in self.records('model')],
                         ['' if mode == 'foreign' else 'history-probe']*2)
        expected_hint = 'current-evidence-only' if mode == 'foreign' else 'history-probe'
        self.assertEqual([c['hint'] for c in final['cards']], [expected_hint]*2)
        self.assertEqual((first['model_calls'], second['model_calls'], final['model_calls']), (1, 2, 2))
        self.assertIn(('/api/v1/artifacts/a', 'Bearer gateway-probe'), self.gateway_requests)
        self.assertFalse(any(auth == 'Bearer ui-probe' for _, auth in self.gateway_requests))
        databases = list((Path(self.directory.name)/'u2-host').glob('*.sqlite3'))
        database = next(p for p in databases if '-verification' not in p.name)
        def delivery_counts():
            with closing(sqlite3.connect(database)) as connection:
                return connection.execute('SELECT COUNT(*), SUM(delivered) FROM teaching_outbox').fetchone()
        self.poll(delivery_counts, lambda counts: counts == (6, 6))
        code, _ = self.request('POST', '/api/v1/shutdown', {})
        self.assertEqual(code, 202)
        output, errors = self.process.communicate(timeout=6)
        self.assertEqual((self.process.returncode, output), (0, b''), errors)
        for private in (b'ui-probe', b'gateway-probe', b'synthetic-model-key', str(self.directory.name).encode()):
            self.assertNotIn(private, errors)
        self.assertEqual(delivery_counts(), (6, 6))

    def test_trusted_adapter_reaches_formal_http_and_committed_outbox(self):
        self.exercise('valid')

    def test_foreign_history_is_discarded_without_changing_model_budget(self):
        self.exercise('foreign')

    def test_invalid_adapter_fails_before_ready_with_sanitized_error(self):
        # The same entry must validate an adapter before publishing its port.
        environment = dict(os.environ, PYTHONPATH=str(self.package))
        environment.pop('DEEPSEEK_API_KEY', None)
        environment.pop('SIGFLOW_U2_ANCHORS', None)
        bootstrap = dict(type='sigflow-bootstrap', protocol='edu.api.v1', instance_id='inst', nonce='a'*64,
            gateway_url=f'http://127.0.0.1:{self.gateway.server_port}', gateway_token='gateway-probe',
            ui_token='ui-probe', data_directory=self.directory.name)
        result = subprocess.run([sys.executable, '-P', str(self.package/'tests/helpers/teaching_memory_host.py'),
            'invalid', str(self.log)], input=json.dumps(bootstrap).encode()+b'\n', capture_output=True,
            cwd=self.package, env=environment, timeout=6)
        self.assertEqual((result.returncode, result.stdout), (2, b''))
        self.assertEqual(result.stderr.splitlines(), [b'u2-host: initialization_or_runtime_failed'])


if __name__ == '__main__':
    unittest.main()
