import http.client
import importlib.util
import json
from pathlib import Path
import socket
import tempfile
from threading import Thread
import time
import unittest
from contextlib import redirect_stdout, redirect_stderr
import io
from unittest.mock import patch

from sigflow_edu_agent.host.runtime import Runtime
from sigflow_edu_agent.host.store import DurableStore
from sigflow_edu_agent.clients.gateway import GatewayConfig
import test_gateway_source
from _model_isolation import isolate_model_settings


class HostHTTPTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec('sigflow_edu_agent.host.http'), 'HTTP host missing')
        from sigflow_edu_agent.host.http import HostServer
        from sigflow_edu_agent.host.factory import make_factory
        from sigflow_edu_agent.domain import ProviderError
        provider_patch=patch('sigflow_edu_agent.host.factory.DeepSeekProvider.from_settings',
                             side_effect=ProviderError('model_unconfigured'))
        provider_patch.start()
        self.addCleanup(provider_patch.stop)
        self.gateway = test_gateway_source.GatewaySourceHTTPTests()
        self.gateway.setUp()
        self.addCleanup(self.gateway.doCleanups)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.store = DurableStore(Path(self.temp.name) / 'state.sqlite3', 'inst-1')
        self.addCleanup(self.store.close)
        factory = make_factory(GatewayConfig(self.gateway.server.server_port, 'fixture-token', 'inst-1'),
                               anchors=[self.gateway.anchor], evidence_origin='fixture')
        self.runtime = Runtime(self.store, 'inst-1', factory, evidence_origin='fixture')
        self.addCleanup(self.runtime.close)
        self.server = HostServer(self.runtime, 'ui-secret')
        self.thread = Thread(target=self.server.serve_forever, kwargs={'poll_interval': .01}, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop)
        self.body = dict(learner_id='learner-1', project_id='prj-1', revision='rev-12',
                         snapshot_id='snap-12', job_id='job-21', diagnostic_id='diag-1',
                         issue_id='issue-1', question='解释存储行为', mode='rule')

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)

    def request(self, method, path, body=None, **headers):
        conn = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=3)
        defaults = {'Authorization': 'Bearer ui-secret', 'X-Learner-ID': 'learner-1'}
        if body is not None:
            defaults['Content-Type'] = 'application/json'
            body = json.dumps(body).encode()
        conn.request(method, path, body, defaults | headers)
        response = conn.getresponse()
        status, value = response.status, json.loads(response.read())
        conn.close()
        return status, value

    def raw(self, header_lines, body=b''):
        with socket.create_connection(('127.0.0.1', self.server.server_port), timeout=3) as conn:
            conn.sendall(('\r\n'.join(header_lines) + '\r\n\r\n').encode() + body)
            response = http.client.HTTPResponse(conn)
            response.begin()
            return response.status, json.loads(response.read())

    def create(self):
        status, data = self.request('POST', '/api/v1/teaching/sessions', self.body, **{'Idempotency-Key': 'create-1'})
        self.assertEqual(status, 202, data)
        return data['data']['session_id']

    def wait(self, sid, level=1, status='WaitingStudent'):
        end = time.monotonic() + 3
        while time.monotonic() < end:
            code, data = self.request('GET', f'/api/v1/teaching/sessions/{sid}/state')
            self.assertEqual(code, 200, data)
            if data['data']['status'] == status and data['data']['level'] == level:
                return data['data']
            time.sleep(.01)
        self.fail('teaching action did not complete')

    def test_health_public_capabilities_authenticated_and_truthful(self):
        status, value = self.request('GET', '/api/v1/health', Authorization='')
        self.assertEqual(status, 200)
        data = value['data']
        self.assertEqual((data['protocol'], data['instance_id'], data['ready']), ('edu.api.v1', 'inst-1', True))
        self.assertFalse(data['model']['available'])
        self.assertEqual(self.request('GET', '/api/v1/capabilities', Authorization='')[0], 401)
        status, value = self.request('GET', '/api/v1/capabilities')
        self.assertEqual(status, 200)
        self.assertIn('u2.host.v1', value['data']['extensions'])
        self.assertFalse(value['data']['modes'][3]['available'])
        self.assertFalse(value['data']['execution']['can_run_eda'])
        self.assertIn('teaching_memory',value['data'])
        self.assertFalse(value['data']['teaching_memory']['configured'])
        self.assertEqual(value['data']['teaching_memory']['reason'],'adapter_required')

    def probe(self, *, instance='inst-1', body=None):
        from sigflow_edu_agent.host.probe import main
        path = Path(self.temp.name) / 'probe-request.json'
        path.write_text(json.dumps(self.body if body is None else body), encoding='utf-8')
        out, error = io.StringIO(), io.StringIO()
        with patch.dict('os.environ', {'SIGFLOW_U2_UI_TOKEN': 'ui-secret'}), redirect_stdout(out), redirect_stderr(error):
            result = main(['--port', str(self.server.server_port), '--instance-id', instance,
                           '--request', str(path), '--key', 'probe-1', '--wait', '2'])
        return result, out.getvalue(), error.getvalue()

    def test_readonly_probe_runs_bound_teaching_session_without_sensitive_output(self):
        code, out, error = self.probe()
        self.assertEqual((code, error), (0, ''))
        self.assertEqual(json.loads(out)['status'], 'WaitingStudent')
        self.assertEqual(json.loads(out)['evidence_origin'], 'fixture')
        self.assertNotIn(self.body['question'], out)
        self.assertNotIn('ui-secret', out)
        self.assertNotIn(str(self.temp.name), out)
        self.assertEqual(self.probe()[0], 0)
        self.assertEqual(len(self.store.list_sessions()), 1)

    def test_readonly_probe_refuses_foreign_host_and_execution_requests(self):
        code, out, error = self.probe(instance='foreign')
        self.assertEqual((code, out), (2, ''))
        self.assertIn('host_binding_mismatch', error)
        self.assertEqual(self.store.list_sessions(), [])
        code, out, error = self.probe(body=self.body | {'intent': 'verify'})
        self.assertEqual((code, out), (2, ''))
        self.assertIn('readonly_probe_required', error)
        self.assertEqual(self.store.list_sessions(), [])

    def test_readonly_probe_reports_missing_trusted_evidence(self):
        code, out, error = self.probe(body=self.body | {'job_id': 'job-untrusted'})
        self.assertEqual((code, error), (3, ''))
        self.assertEqual(json.loads(out)['status'], 'WaitingEvidence')

    def test_loopback_fixture_create_hint_replay_scope_events_cancel(self):
        sid = self.create()
        state = self.wait(sid)
        self.assertEqual(state['evidence_origin'], 'fixture')
        self.assertEqual(state['cards'][0]['evidence_ids'], ['ev-1'])
        self.assertEqual(self.request('GET', f'/api/v1/teaching/sessions/{sid}/state', **{'X-Learner-ID': 'other'})[0], 403)
        action = dict(kind='hint_next', action_id='hint-1', expected_version=state['state_version'])
        self.assertEqual(self.request('POST', f'/api/v1/teaching/sessions/{sid}/actions', action)[0], 202)
        second = self.wait(sid, 2)
        code, value = self.request('POST', f'/api/v1/teaching/sessions/{sid}/actions', action)
        self.assertEqual(code, 202)
        self.assertEqual(value['data']['result']['level'], 2)
        stale = action | {'action_id': 'stale'}
        self.assertEqual(self.request('POST', f'/api/v1/teaching/sessions/{sid}/actions', stale)[0], 409)
        l4 = action | dict(kind='reference_request', action_id='l4', expected_version=second['state_version'])
        self.assertEqual(self.request('POST', f'/api/v1/teaching/sessions/{sid}/actions', l4)[0], 403)
        status, events = self.request('GET', f'/api/v1/teaching/sessions/{sid}/events?after=0&limit=100')
        self.assertEqual(status, 200)
        self.assertTrue(events['data']['events'])
        self.assertEqual(self.request('POST', f'/api/v1/teaching/sessions/{sid}/cancel', {})[0], 202)
        self.wait(sid, 2, 'Cancelled')
        self.assertEqual(len(self.gateway.requests), 4)

    def test_bad_envelope_is_rejected_before_session_creation(self):
        host = f'127.0.0.1:{self.server.server_port}'
        prefix = ['POST /api/v1/teaching/sessions HTTP/1.1', 'Host: ' + host,
                  'Authorization: Bearer ui-secret', 'X-Learner-ID: learner-1',
                  'Idempotency-Key: unsafe', 'Content-Type: application/json']
        cases = [(prefix, b'', 411),
                 (prefix + ['Content-Length: 2', 'Content-Length: 2'], b'{}', 400),
                 (prefix + ['Transfer-Encoding: chunked', 'Content-Length: 2'], b'{}', 400),
                 (prefix + ['Content-Length: 65537'], b'', 413),
                 (prefix + ['Content-Length: 13'], b'{"x":1,"x":2}', 400),
                 (prefix + ['Content-Length: 9'], b'{"x":NaN}', 400),
                 (prefix + ['Content-Length: 2'], b'\xff\xff', 400),
                 (prefix + ['Content-Length: 2', 'Origin: https://evil.test'], b'{}', 403),
                 (prefix + ['Content-Length: 2', 'Host: evil.test'], b'{}', 403)]
        for headers, body, expected in cases:
            with self.subTest(headers=headers):
                self.assertEqual(self.raw(headers, body)[0], expected)
        self.assertEqual(self.store.list_sessions(), [])

    def test_json_shape_unknown_keys_scope_and_versions_rejected(self):
        path = '/api/v1/teaching/sessions'
        for body in (self.body | {'anchors': []}, self.body | {'learner_id': 'other'},
                     self.body | {'question': '\ud800'}):
            with self.subTest(body=body):
                self.assertIn(self.request('POST', path, body, **{'Idempotency-Key': 'bad'})[0], (400, 403))
        sid = self.create()
        self.wait(sid)
        path += f'/{sid}/actions'
        for version in (1, True, '-1', '01', '9223372036854775808'):
            with self.subTest(version=version):
                self.assertEqual(self.request('POST', path, dict(kind='hint_next', action_id='x', expected_version=version))[0], 400)
        self.assertEqual(self.request('POST', path, dict(kind='hint_next', action_id='x', expected_version='1', question='x'))[0], 400)

    def test_explicit_recovery_after_restart_refetches_fixture_evidence(self):
        sid = self.create()
        state = self.wait(sid)
        self.runtime.close()
        self.store.close()
        from sigflow_edu_agent.host.factory import make_factory
        self.store = DurableStore(Path(self.temp.name) / 'state.sqlite3', 'inst-1')
        self.addCleanup(self.store.close)
        self.runtime = Runtime(self.store, 'inst-1', make_factory(
            GatewayConfig(self.gateway.server.server_port, 'fixture-token', 'inst-1'),
            anchors=[self.gateway.anchor], evidence_origin='fixture'), evidence_origin='fixture')
        self.addCleanup(self.runtime.close)
        self.server.runtime = self.runtime
        code, response = self.request('GET', f'/api/v1/teaching/sessions/{sid}/state')
        self.assertEqual(code, 200)
        state = response['data']
        self.assertEqual(state['status'], 'RecoveryRequired')
        self.assertEqual(state['cards'], [])
        action = dict(kind='recover', action_id='recover-1', expected_version=state['state_version'], question=self.body['question'])
        self.assertEqual(self.request('POST', f'/api/v1/teaching/sessions/{sid}/actions', action | {'question': 'wrong'})[0], 409)
        self.assertEqual(self.request('POST', f'/api/v1/teaching/sessions/{sid}/actions', action)[0], 202)
        self.wait(sid)
        self.assertEqual(len(self.gateway.requests), 4)

    def test_unknown_future_cursor_reports_gone(self):
        sid = self.create()
        self.wait(sid)
        self.assertEqual(self.request('GET', f'/api/v1/teaching/sessions/{sid}/events?after=999')[0], 410)

    def test_missing_anchors_and_unconfigured_model_publish_no_invented_evidence(self):
        from sigflow_edu_agent.host.factory import make_factory
        from unittest.mock import patch
        with patch.dict('os.environ', {}, clear=True):
            self.runtime.factory = make_factory(GatewayConfig(self.gateway.server.server_port, 'fixture-token', 'inst-1'),
                                                evidence_origin='fixture')
        self.body['mode'] = 'deepseek'
        state = self.wait(self.create(), status='WaitingEvidence')
        self.assertEqual(state['cards'], [])
        self.assertEqual(state['reason'], 'unresolved_evidence')
        self.assertEqual(state['model_calls'], 0)

    def test_verify_http_accepts_ui_grant_and_does_not_invent_new_job_evidence(self):
        import test_host_verification
        from sigflow_edu_agent.context.models import EvidenceAnchor
        from sigflow_edu_agent.host.factory import make_factory, make_report_resolver
        fixture = test_host_verification.VerificationHTTPFixture()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        fixture.jobs['seed-job'] = dict(expected_revision='rev-a', snapshot_id='snap-a',
                                       capability='eda.sim.build', plugin_id='eda-sim-verilator')
        fixture.report_override = dict(input_fingerprint='fixture-input', diagnostics=[dict(
            id='diag-seed', code='EDU_LATCH_INFERRED', summary='Fixture storage observation',
            stage='synth', origin='core', severity='warning', confidence_kind='tool', evidence_refs=['ev-seed'])])
        seed = EvidenceAnchor('ev-seed', 'project-a', 'rev-a', 'seed-job', 'snap-a', 'artifact-seed-job', 'a' * 64)
        config = GatewayConfig(fixture.server.server_port, 'token-secret', 'inst-a', timeout=.3)
        # Only the known baseline has a trusted anchor. A new successful Job
        # cannot gain a trusted observation from its process exit alone.
        fixture.adapter.report_resolver = make_report_resolver(config, [seed], evidence_origin='fixture')
        self.runtime.close()
        self.store.close()
        self.store = DurableStore(Path(self.temp.name) / 'verify-host.sqlite3', 'inst-a')
        self.runtime = Runtime(self.store, 'inst-a', make_factory(config, anchors=[seed],
            evidence_origin='fixture', verification=fixture.adapter), evidence_origin='fixture')
        self.addCleanup(self.store.close)
        self.addCleanup(self.runtime.close)
        self.server.runtime = self.runtime
        plan = dict(project_id='project-a', revision='rev-a', snapshot_id='snap-a', goal='Verify counter',
            steps=[dict(step_id='compile', capability='eda.sim.build', provider='verilator',
                        params={'top': 'counter', 'testbench': 'counter_tb'}),
                   dict(step_id='execute', capability='eda.sim.run', provider='verilator',
                        params={'build_step_id': 'compile'}, dependencies=['compile'])],
            expected_observations=['count increments'])
        self.body.update(project_id='project-a', revision='rev-a', snapshot_id='snap-a', job_id='seed-job',
                         diagnostic_id='diag-seed', intent='verify', plan=plan, gateway_plan_hash='ui-approved-hash')
        sid = self.create()
        waiting = self.wait(sid, status='WaitingApproval')
        self.assertEqual(waiting['plan_card']['steps'][0]['params']['top_module'], 'counter')
        action = dict(kind='execute_plan', action_id='execute-1', expected_version=waiting['state_version'], grant_ref='grant-secret')
        self.assertEqual(self.request('POST', f'/api/v1/teaching/sessions/{sid}/actions', action)[0], 202)
        result = self.wait(sid, status='RecoveryRequired')
        self.assertEqual(result['reason'], 'verification_evidence_pending')
        self.assertEqual(result['verification']['job_id'], 'job-1')
        self.assertTrue(result['verification']['can_observe'])
        self.assertEqual(len(fixture.posts), 1)
        code, response = self.request('POST', f'/api/v1/teaching/sessions/{sid}/actions', action)
        self.assertEqual(code, 202)
        self.assertEqual(response['data']['result']['status'], 'RecoveryRequired')
        self.assertEqual(len(fixture.posts), 1)

    def test_model_authored_text_cannot_echo_process_secrets_or_absolute_paths(self):
        from sigflow_edu_agent.runtime.gateway_session import create_gateway_session
        class CandidateFixture:
            def generate(self, context):
                return dict(level=context.level, hint='ui-secret C:/private/key.txt /tmp/key.txt',
                            question='请核对观察。', evidence_ids=['ev-1'])
        def factory(binding, request, mode, reserve):
            session = create_gateway_session(binding=binding, request=request,
                gateway=GatewayConfig(self.gateway.server.server_port, 'fixture-token', 'inst-1'),
                anchors=[self.gateway.anchor], mode='rule', evidence_origin='fixture', combinational_goal=True)
            session._loop.provider = CandidateFixture()
            return session
        self.runtime.factory = factory
        state = self.wait(self.create())
        self.assertEqual(state['cards'][0]['producer'], 'model')
        self.assertEqual(state['cards'][0]['hint'], '[redacted] [path] [path]')


class HostFactoryTests(unittest.TestCase):
    def setUp(self):
        isolate_model_settings(self)

    def test_verify_plan_requires_explicit_verifier_and_composes_real_session(self):
        import inspect
        from sigflow_edu_agent.host.factory import make_factory
        self.assertIn('verification', inspect.signature(make_factory).parameters, 'verification composition missing')
        from sigflow_edu_agent.checkers.plan import DetailedPlan, PlanStep
        from sigflow_edu_agent.domain import RunRequest, Status
        from sigflow_edu_agent.host.verification import GatewayVerification
        from sigflow_edu_agent.runtime.gateway_session import SessionBinding
        gateway = test_gateway_source.GatewaySourceHTTPTests()
        gateway.setUp()
        self.addCleanup(gateway.doCleanups)
        config = GatewayConfig(gateway.server.server_port, 'fixture-token', 'inst-1')
        binding = SessionBinding('inst-1', 'session-1', 'learner-1', 'prj-1', 'rev-12', 'snap-12', 'job-21', 'diag-1', 'issue-1')
        request = RunRequest('prj-1', 'rev-12', 'issue-1', '验证存储观察', 'verify')
        plan = DetailedPlan('prj-1', 'rev-12', 'snap-12', 'Verify observation',
            (PlanStep('synth', 'eda.synth', 'yosys', {'top': 'top'}),), ('inspect diagnostic',))
        factory = make_factory(config, anchors=[gateway.anchor], evidence_origin='fixture')
        with self.assertRaisesRegex(ValueError, 'verification_unavailable'):
            factory(binding, request, 'rule', lambda: None, plan=plan, gateway_plan_hash='ui-hash')
        self.assertEqual(gateway.requests, [])
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        verifier = GatewayVerification(config, Path(directory.name) / 'verify.sqlite', lambda *a, **k: None)
        self.addCleanup(verifier.close)
        factory = make_factory(config, anchors=[gateway.anchor], evidence_origin='fixture', verification=verifier)
        session = factory(binding, request, 'rule', lambda: None, plan=plan, gateway_plan_hash='ui-hash')
        for _ in range(10):
            if session.state.status != Status.RUNNING:
                break
            session.advance(session.state.version)
        self.assertEqual(session.state.status, Status.WAITING_APPROVAL)
        self.assertEqual(session._loop.plan.fingerprint, plan.fingerprint)
        self.assertEqual(len(gateway.requests), 2)

    def test_quota_failure_prevents_provider_request(self):
        from sigflow_edu_agent.host.factory import BudgetedProvider
        from sigflow_edu_agent.deepseek import DeepSeekProvider, DeepSeekConfig
        from sigflow_edu_agent.domain import ProviderError
        provider = DeepSeekProvider(DeepSeekConfig('fake-test-key', 'test-model'))
        def reserve():
            raise ValueError('budget_exhausted')
        with self.assertRaises(ProviderError) as error:
            BudgetedProvider(provider, reserve).generate(None)
        self.assertEqual(error.exception.code, 'model_budget_exceeded')

    def test_strict_json_rejects_overflow_number(self):
        from sigflow_edu_agent.host.http import strict_json
        with self.assertRaises(ValueError):
            strict_json(b'{"number":1e999}')

    def test_trusted_report_resolver_refetches_only_matching_anchor(self):
        from sigflow_edu_agent.host.factory import make_report_resolver
        gateway = test_gateway_source.GatewaySourceHTTPTests()
        gateway.setUp()
        self.addCleanup(gateway.doCleanups)
        config = GatewayConfig(gateway.server.server_port, 'fixture-token', 'inst-1')
        resolve = make_report_resolver(config, [gateway.anchor], evidence_origin='fixture')
        scope = dict(project_id='prj-1', revision='rev-12', snapshot_id='snap-12')
        self.assertIsNone(resolve('unknown-job', **scope))
        self.assertEqual(gateway.requests, [])
        evidence = resolve('job-21', **scope)
        self.assertEqual(evidence.anchor, gateway.anchor)
        self.assertEqual(evidence.origin, 'fixture')
        self.assertEqual(len(gateway.requests), 4)
        gateway.report['diagnostics'] = []
        self.assertIsNone(resolve('job-21', **scope))


if __name__ == '__main__':
    unittest.main()
