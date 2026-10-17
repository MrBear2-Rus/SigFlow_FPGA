from dataclasses import replace
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import http.client
import importlib
import json
import os
import subprocess
import sys
import tempfile
from queue import Queue
from threading import Thread
import time
import unittest
from unittest.mock import patch

from sigflow_edu_agent.clients.gateway import GatewayClient, GatewayConfig, GatewayError
from sigflow_edu_agent.domain import EvidenceError, RunRequest, Status
from sigflow_edu_agent.host.factory import make_factory, make_report_resolver
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.patterns import assess, PATTERNS
from sigflow_edu_agent.pedagogy import RuleProvider
from sigflow_edu_agent.domain import CardContext
from sigflow_edu_agent.runtime.gateway_session import SessionBinding
from _model_isolation import isolate_model_settings


class GatewayEvidenceTests(unittest.TestCase):
    def setUp(self):
        isolate_model_settings(self)
        self.config = GatewayConfig(12345, 'private-agent-token', 'inst-1')
        self.client = GatewayClient(self.config)
        self.scope = dict(project_id='p', revision='r', snapshot_id='s')
        self.report = dict(schema_version='edu.jobreport.v1', job_id='j', origin='core',
            project_id='p', revision='r', snapshot_id='s', input_fingerprint='input-hash',
            state='Succeeded', exit_code=0, completeness='complete', capability='eda.synth',
            plugin_id='eda-synth-yosys', plugin_version='1', diagnostics=[], metrics={},
            artifacts=[dict(artifact_id='a', sha256='a' * 64, schema='netlist.v1', role='netlist')])
        self.metadata = dict(artifact_id='a', project_id='p', revision='r', job_id='j',
            schema='netlist.v1', sha256='a' * 64, role='netlist', size='123',
            media_type='application/json', inline_readable=True)
        self.requests = []
        self.health_instance = 'inst-1'
        self.error = None
        owner = self

        class Connection:
            def __init__(self, host, port, timeout):
                if (host, port) != ('127.0.0.1', 12345):
                    raise AssertionError('unapproved transport')
            def request(self, method, path, headers):
                owner.requests.append((method, path, headers))
                self.path = path
            def getresponse(self):
                health = self.path == '/api/v1/health'
                if health:
                    data = dict(protocol_version='edu.api.v1', edition='edu', instance_id=owner.health_instance)
                elif self.path == '/api/v1/jobs/j/report':
                    data = owner.report
                elif self.path.startswith('/api/v1/artifacts/'):
                    data = owner.metadata
                else:
                    raise AssertionError('unapproved route: ' + self.path)
                envelope = dict(schema_version='edu.api.v1', request_id='req', trace_id='trace')
                if owner.error and not health:
                    envelope['error'] = dict(code=owner.error, message='secret-server-text', retryable=False)
                else:
                    envelope['data'] = data
                class Reply:
                    status = 410 if owner.error and not health else 200
                    def read(self, limit): return json.dumps(envelope).encode()[:limit]
                return Reply()
            def close(self): pass
        transport = patch('sigflow_edu_agent.clients.gateway.http.client.HTTPConnection', Connection)
        transport.start()
        self.addCleanup(transport.stop)

    def catalog(self):
        spec = importlib.util.find_spec('sigflow_edu_agent.host.gateway_evidence')
        self.assertIsNotNone(spec, 'trusted Gateway evidence catalog missing')
        return importlib.import_module(spec.name).GatewayEvidenceCatalog(self.config)

    def collect(self, catalog=None, diagnostic_id='job-report'):
        return (catalog or self.catalog()).collect('j', diagnostic_id=diagnostic_id, **self.scope)

    def test_metadata_is_authenticated_typed_and_scope_hash_bound(self):
        self.assertTrue(hasattr(self.client, 'artifact_metadata'), 'artifact metadata route missing')
        result = self.client.artifact_metadata('a', project_id='p', revision='r', job_id='j', sha256='a' * 64)
        self.assertEqual(result['size'], '123')
        self.assertEqual(self.requests[-1][0:2], ('GET', '/api/v1/artifacts/a'))
        self.assertEqual(self.requests[-1][2]['Authorization'], 'Bearer private-agent-token')
        for changes in (dict(project_id='other'), dict(revision='old'), dict(job_id='foreign'),
                        dict(sha256='b' * 64), dict(size=123), dict(size='-1'), dict(size='1e3'),
                        dict(inline_readable=1), dict(schema=None), dict(artifact_id='../a')):
            with self.subTest(changes=changes):
                original = self.metadata
                self.metadata = original | changes
                with self.assertRaises(GatewayError):
                    self.client.artifact_metadata('a', project_id='p', revision='r', job_id='j', sha256='a' * 64)
                self.metadata = original
        for identifier in ('../a', 'http://evil', 'a/content', 'a?content'):
            before = len(self.requests)
            with self.assertRaises(GatewayError):
                self.client.artifact_metadata(identifier, project_id='p', revision='r', job_id='j', sha256='a' * 64)
            self.assertEqual(len(self.requests), before)

    def test_complete_empty_diagnostics_produce_bounded_stable_observation(self):
        catalog = self.catalog()
        evidence = self.collect(catalog)
        self.assertEqual(evidence.concept, 'job_observation')
        self.assertEqual(evidence.origin, 'tool')
        self.assertEqual(evidence.anchor.artifact_id, 'a')
        self.assertIsNone(evidence.anchor.source_ref)
        self.assertIn('Succeeded', evidence.summary)
        self.assertIn('不证明功能正确', evidence.summary)
        self.assertEqual(self.collect(catalog), evidence)
        self.assertEqual(len([r for r in self.requests if '/artifacts/' in r[1]]), 2)
        self.assertEqual(self.report['diagnostics'], [])

    def test_untrusted_incomplete_or_unbounded_reports_do_not_certify(self):
        catalog = self.catalog()
        for changes in (dict(project_id='other'), dict(revision='old'), dict(snapshot_id='other'),
                        dict(job_id='other'), dict(origin='legacy'), dict(state='Running'),
                        dict(state='Failed'), dict(exit_code=1), dict(exit_code=None),
                        dict(completeness='partial'), dict(input_fingerprint=''), dict(artifacts=[]),
                        dict(plugin_id='unknown'), dict(capability='shell'),
                        dict(diagnostics=None), dict(artifacts=self.report['artifacts'] * 257)):
            with self.subTest(changes=changes):
                original = self.report
                self.report = original | changes
                with self.assertRaises((GatewayError, EvidenceError, ValueError)):
                    self.collect(catalog)
                self.report = original
        self.health_instance = 'other'
        with self.assertRaisesRegex(GatewayError, 'INSTANCE_MISMATCH'):
            self.collect(catalog)

    def test_ownership_hash_expiry_and_schema_are_rechecked_without_cache(self):
        catalog = self.catalog()
        self.collect(catalog)
        for changes in (dict(project_id='foreign'), dict(revision='old'), dict(job_id='foreign'),
                        dict(sha256='b' * 64), dict(schema='other')):
            original = self.metadata
            self.metadata = original | changes
            with self.subTest(changes=changes), self.assertRaises((GatewayError, EvidenceError)):
                self.collect(catalog)
            self.metadata = original
        self.error = 'ARTIFACT_EXPIRED'
        with self.assertRaisesRegex(GatewayError, 'ARTIFACT_EXPIRED'):
            self.collect(catalog)

    def test_core_original_id_role_differs_from_normalized_report_role(self):
        # Core metadata uses plugin-local id (netlist); the normalized report
        # carries the plugin's role (primary). Neither value assigns ownership.
        self.report['artifacts'][0]['role'] = 'primary'
        self.metadata['role'] = 'netlist'
        self.assertEqual(self.collect().anchor.artifact_id, 'a')

    def test_run_report_requires_actual_run_plugin_and_rejects_build_plugin(self):
        self.report.update(capability='eda.sim.run', plugin_id='eda-sim-verilator-run')
        self.assertEqual(self.collect().concept, 'job_observation')
        self.report['plugin_id'] = 'eda-sim-verilator'
        with self.assertRaises(EvidenceError):
            self.collect()

    def test_artifact_budget_is_checked_before_any_metadata_request(self):
        self.report['artifacts'] = [dict(self.report['artifacts'][0], artifact_id=f'a-{i}') for i in range(17)]
        with self.assertRaises((EvidenceError, ValueError)):
            self.collect()
        self.assertFalse(any('/artifacts/' in r[1] for r in self.requests))

    def test_structured_diagnostic_refs_bind_only_verified_artifacts(self):
        self.report['diagnostics'] = [dict(id='diag', code='EDU_LATCH_INFERRED', summary='storage observed',
            stage='synth', origin='core', severity='warning', confidence_kind='tool', evidence_refs=['a'])]
        evidence = self.collect(diagnostic_id='diag')
        self.assertEqual(evidence.concept, 'latch')
        self.assertEqual(evidence.evidence_id, 'a')
        self.assertIsNone(evidence.anchor.source_ref)
        self.report['diagnostics'][0]['evidence_refs'] = ['unbound-alias']
        with self.assertRaises(EvidenceError):
            self.collect(diagnostic_id='diag')

    def test_certified_observation_calls_provider_at_all_levels_invalid_unknown_do_not(self):
        evidence = self.collect()
        class Source:
            def collect(self, project_id, revision): return evidence
        class Provider:
            def generate(self, context):
                return dict(level=context.level, hint='请核对当前任务的报告范围。',
                    question='这份报告能说明哪些任务状态？', evidence_ids=[context.evidence.evidence_id])
        loop = TeachingLoop(RunRequest('p', 'r', 'issue', '解释任务'), Source(), Provider())
        state = loop.run_until_pause()
        self.assertEqual(state.cards[-1].producer, 'model')
        for level in (2, 3):
            loop.act('hint_next', str(level), state.version)
            state = loop.run_until_pause()
            self.assertEqual(state.cards[-1].level, level)
            self.assertEqual(state.cards[-1].producer, 'model')
        self.assertEqual(state.model_calls, 3)
        self.assertEqual(len(PATTERNS), 8)
        for level in (1, 2, 3):
            card = RuleProvider().generate(CardContext('解释', level, evidence))
            self.assertIn('不证明功能正确', card['hint'])
        for bad in (replace(evidence, concept='unknown'), replace(evidence, anchor=None),
                    replace(evidence, complete=False), replace(evidence, origin='fixture'),
                    replace(evidence, anchor=replace(evidence.anchor, expired=True))):
            evidence = bad
            state = TeachingLoop(RunRequest('p', 'r', 'issue', '解释'), Source(), Provider()).run_until_pause()
            self.assertEqual(state.model_calls, 0)
            self.assertNotEqual(assess(bad).status, 'supported')

    def test_factory_and_result_resolver_use_same_explicit_catalog(self):
        catalog = self.catalog()
        resolve = make_report_resolver(self.config, catalog)
        self.assertEqual(resolve('j', **self.scope).concept, 'job_observation')
        self.assertIsNone(make_report_resolver(self.config, ())('j', **self.scope))
        binding = SessionBinding('inst-1', 'session', 'learner', 'p', 'r', 's', 'j', 'job-report', 'issue')
        session = make_factory(self.config, anchors=catalog)(binding, RunRequest('p', 'r', 'issue', '解释'), 'rule', lambda: None)
        for _ in range(10):
            if session.state.status != Status.RUNNING: break
            session.advance(session.state.version)
        self.assertEqual(session.state.status, Status.WAITING_STUDENT)
        self.assertEqual(session.state.cards[-1].evidence_ids, (resolve('j', **self.scope).evidence_id,))
        self.error = 'ARTIFACT_EXPIRED'
        with self.assertRaises(GatewayError): resolve('j', **self.scope)

    def test_refresh_rejects_expired_artifacts_before_another_model_call(self):
        catalog = self.catalog()
        class Source:
            def collect(inner, project_id, revision):
                return catalog.collect('j', project_id=project_id, revision=revision, snapshot_id='s')
        loop = TeachingLoop(RunRequest('p', 'r', 'issue', '解释'), Source(), RuleProvider())
        state = loop.run_until_pause()
        self.assertEqual(state.model_calls, 1)
        self.error = 'ARTIFACT_EXPIRED'
        loop.act('hint_next', 'next', state.version)
        state = loop.run_until_pause()
        self.assertEqual(state.status, Status.WAITING_EVIDENCE)
        self.assertEqual(state.model_calls, 1)
        self.assertEqual(len(state.cards), 1)

    def test_process_success_without_independent_expected_result_stays_indeterminate(self):
        from sigflow_edu_agent.checkers.outcome import OutcomeChecker
        old = self.collect()
        new = replace(old, evidence_id='fresh', job_id='new-job', anchor=replace(old.anchor,
            evidence_id='fresh', job_id='new-job'))
        outcome = OutcomeChecker().compare(old, new, 'r', project_id='p', matches_expected=None, snapshot_id='s')
        self.assertEqual(outcome.state, 'indeterminate')

    def test_catalog_cannot_silently_rebind_gateway_or_fixture_origin(self):
        catalog = self.catalog()
        for compose in (make_factory, make_report_resolver):
            with self.subTest(compose=compose.__name__), self.assertRaisesRegex(ValueError, 'invalid_gateway_evidence_config'):
                if compose is make_factory:
                    compose(self.config, anchors=catalog, evidence_origin='fixture')
                else:
                    compose(self.config, catalog, evidence_origin='fixture')
            foreign = GatewayConfig(12345, 'other-token', 'foreign-instance')
            with self.assertRaisesRegex(ValueError, 'invalid_gateway_evidence_config'):
                if compose is make_factory:
                    compose(foreign, anchors=catalog)
                else:
                    compose(foreign, catalog)

    def test_cli_mode_conflicts_are_rejected_before_bootstrap(self):
        environment = os.environ | {'SIGFLOW_U2_ANCHORS': 'missing-catalog.json'}
        result = subprocess.run([sys.executable, '-m', 'sigflow_edu_agent.host', '--bootstrap-stdin', '--gateway-evidence'],
            input=b'', capture_output=True, env=environment, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b'gateway_evidence_conflicts_with_anchors', result.stderr)
        environment.pop('SIGFLOW_U2_ANCHORS', None)
        result = subprocess.run([sys.executable, '-m', 'sigflow_edu_agent.host', '--bootstrap-stdin',
            '--gateway-evidence', '--anchors', 'missing.json'], input=b'', capture_output=True, env=environment, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b'not allowed with argument', result.stderr)


class GatewayEvidenceProcessTests(unittest.TestCase):
    def setUp(self):
        isolate_model_settings(self)

    def test_explicit_mode_boots_and_teaches_from_authenticated_loopback_metadata(self):
        report = dict(schema_version='edu.jobreport.v1', project_id='p', revision='r', snapshot_id='s',
            job_id='j', origin='core', capability='eda.synth', plugin_id='eda-synth-yosys',
            plugin_version='1', state='Succeeded', exit_code=0, completeness='complete',
            input_fingerprint='input-hash', diagnostics=[], metrics={},
            artifacts=[dict(artifact_id='a', sha256='a' * 64, schema='netlist.v1', role='primary')])
        metadata = dict(artifact_id='a', project_id='p', revision='r', job_id='j', sha256='a' * 64,
            schema='netlist.v1', role='netlist', size='123', media_type='application/json', inline_readable=True)
        seen = []
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                seen.append((self.path, self.headers.get('Authorization')))
                if self.path == '/api/v1/health':
                    data = dict(protocol_version='edu.api.v1', instance_id='inst', edition='edu')
                elif self.headers.get('Authorization') != 'Bearer agent-test':
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
            def log_message(self, *args): pass
        gateway = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        worker = Thread(target=gateway.serve_forever, kwargs={'poll_interval': .01}, daemon=True)
        worker.start()
        self.addCleanup(gateway.server_close)
        self.addCleanup(gateway.shutdown)
        with tempfile.TemporaryDirectory() as directory:
            environment = dict(os.environ)
            environment.pop('SIGFLOW_U2_ANCHORS', None)
            process = subprocess.Popen([sys.executable, '-m', 'sigflow_edu_agent.host',
                '--bootstrap-stdin', '--gateway-evidence'], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)
            self.addCleanup(lambda: process.kill() if process.poll() is None else None)
            bootstrap = dict(type='sigflow-bootstrap', protocol='edu.api.v1', instance_id='inst',
                nonce='a' * 64, gateway_url=f'http://127.0.0.1:{gateway.server_port}',
                gateway_token='agent-test', ui_token='ui-test', data_directory=directory)
            process.stdin.write(json.dumps(bootstrap).encode() + b'\n')
            process.stdin.flush()
            queue = Queue()
            Thread(target=lambda: queue.put(process.stdout.readline()), daemon=True).start()
            ready = queue.get(timeout=5)
            self.assertTrue(ready)
            port = json.loads(ready)['port']
            def request(method, path, body=None, **headers):
                conn = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
                combined = {'Authorization': 'Bearer ui-test', 'X-Learner-ID': 'learner'} | headers
                if body is not None:
                    body = json.dumps(body).encode()
                    combined['Content-Type'] = 'application/json'
                try:
                    conn.request(method, path, body, combined)
                    response = conn.getresponse()
                    return response.status, json.loads(response.read())
                finally:
                    conn.close()
            try:
                code, result = request('POST', '/api/v1/teaching/sessions', dict(learner_id='learner',
                    project_id='p', revision='r', snapshot_id='s', job_id='j', diagnostic_id='job-report',
                    issue_id='issue', question='解释工具任务范围', mode='rule'), **{'Idempotency-Key': 'create'})
                self.assertEqual(code, 202, result)
                sid = result['data']['session_id']
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    code, result = request('GET', f'/api/v1/teaching/sessions/{sid}/state')
                    if result['data']['status'] != 'Running': break
                    time.sleep(.01)
                self.assertEqual(result['data']['status'], 'WaitingStudent', result)
                self.assertIn('不证明功能正确', result['data']['cards'][0]['hint'])
                self.assertTrue(result['data']['cards'][0]['evidence_ids'][0].startswith('gateway-'))
                self.assertIn(('/api/v1/artifacts/a', 'Bearer agent-test'), seen)
                self.assertFalse(any(auth == 'Bearer ui-test' for _, auth in seen))
            finally:
                request('POST', '/api/v1/shutdown', {})
                output, errors = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, errors)
                self.assertEqual(output, b'')
