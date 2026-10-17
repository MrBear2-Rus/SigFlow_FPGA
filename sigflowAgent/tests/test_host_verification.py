"""Explicit loopback HTTP fixture: no EDA tool is executed by these tests."""
from copy import deepcopy
from dataclasses import replace
from datetime import datetime, timedelta, timezone
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import socket
import sqlite3
import tempfile
from threading import Barrier, Thread
from concurrent.futures import ThreadPoolExecutor
import time
import unittest

from sigflow_edu_agent.checkers.plan import DetailedPlan, PlanStep
from sigflow_edu_agent.clients.gateway import GatewayConfig, GatewayError
from sigflow_edu_agent.context.models import EvidenceAnchor
from sigflow_edu_agent.contracts.plan import ApprovalPackage, build_plan_card
from sigflow_edu_agent.domain import Evidence, Plan
try:
    from sigflow_edu_agent.host.verification import GatewayVerification
except ImportError:
    GatewayVerification = None


def plan():
    return DetailedPlan('project-a', 'rev-a', 'snap-a', 'Verify counter', (
        PlanStep('compile', 'eda.sim.build', 'verilator', {'top': 'counter', 'testbench': 'counter_tb'}),
        PlanStep('execute', 'eda.sim.run', 'verilator', {'build_step_id': 'compile'}, ('compile',)),
    ), ('count increments',))


class VerificationHTTPFixture(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(GatewayVerification, 'GatewayVerification adapter is required')
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.journal = Path(self.temp.name) / 'verification.sqlite'
        self.plan = plan()
        self.package = build_plan_card(self.plan, 'plan-1', 0)
        self.grant = dict(grant_id='grant-secret', status='active', project_id='project-a',
            revision='rev-a', snapshot_id='snap-a', plan_hash='ui-approved-hash',
            max_jobs=3, used_jobs=0, steps=self.package.card['steps'],
            expires_at=(datetime.now(timezone.utc) + timedelta(hours=1)).strftime('%Y-%m-%dT%H:%M:%SZ'))
        self.posts, self.gets, self.jobs = [], [], {}
        self.job_state, self.post_mode = 'Succeeded', 'normal'
        self.cancel_mode, self.cancel_posts = 'normal', []
        self.health = dict(instance_id='inst-a', protocol_version='edu.api.v1', edition='edu')
        self.context = dict(project_id='project-a', revision='rev-a', dirty=False, synchronized=True,
                            top='counter', target='fixture', sources=[], buffer_hash=None, policy_version=None)
        self.envelope_version = 'edu.api.v1'
        self.report_override = {}
        self.report_overrides = {}
        self.job_override = {}
        self.job_delay = 0
        self.match = None
        self.run_plugin = 'eda-sim-verilator-run'
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def respond(self, data, status=200):
                raw = json.dumps(dict(schema_version=owner.envelope_version, request_id='r1',
                                      trace_id='t1', data=data)).encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(raw)))
                self.end_headers()
                try:
                    self.wfile.write(raw)
                except OSError:
                    pass  # client time limits intentionally close the socket
            def do_GET(self):
                owner.gets.append((self.path, self.headers.get('Authorization')))
                if self.path.endswith('/health'):
                    return self.respond(owner.health)
                if self.path == '/api/v1/projects/project-a/context':
                    return self.respond(owner.context)
                if self.path.endswith('/capabilities'):
                    return self.respond(dict(**owner.health, capabilities=[dict(id=c, ready=True,
                        disabled=False, plugin_id=p, plugin_version='1.0', reason=None)
                        for c, p in [('eda.synth', 'eda-synth-yosys'),
                                     ('eda.sim.build', 'eda-sim-verilator'), ('eda.sim.run', owner.run_plugin)]]))
                if self.path.startswith('/api/v1/grants/'):
                    return self.respond(owner.grant)
                job_id = self.path.split('/')[4]
                body = owner.jobs[job_id]
                if not self.path.endswith('/report') and owner.job_delay:
                    time.sleep(owner.job_delay)
                if self.path.endswith('/report'):
                    report = dict(schema_version='edu.jobreport.v1', job_id=job_id,
                        project_id='project-a', revision=body['expected_revision'], snapshot_id=body['snapshot_id'],
                        capability=body['capability'], plugin_id=body['plugin_id'], origin='core',
                        state=owner.job_state, completeness='complete', diagnostics=[], metrics={},
                        artifacts=[dict(artifact_id='artifact-' + job_id, sha256='a' * 64, role='primary')])
                    report.update(owner.report_override)
                    report.update(owner.report_overrides.get(job_id, {}))
                    return self.respond(report)
                job = dict(job_id=job_id, project_id='project-a',
                    job_type={'eda.synth': 'synth', 'eda.sim.build': 'sim.build', 'eda.sim.run': 'sim.run'}[body['capability']],
                    state=owner.job_state, exit_code=0, created_at='now', updated_at='now')
                job.update(owner.job_override)
                return self.respond(job)
            def do_POST(self):
                if self.path.endswith('/cancel'):
                    job_id = self.path.split('/')[4]
                    owner.cancel_posts.append(job_id)
                    if owner.cancel_mode == 'drop':
                        self.connection.shutdown(socket.SHUT_RDWR)
                        self.connection.close()
                        return
                    if owner.cancel_mode == 'reject':
                        raw = json.dumps(dict(schema_version='edu.api.v1', request_id='r', trace_id='t',
                            error=dict(code='POLICY_DENIED', message='private error detail', retryable=False))).encode()
                        self.send_response(409)
                        self.send_header('Content-Length', str(len(raw)))
                        self.end_headers()
                        self.wfile.write(raw)
                        return
                    return self.respond(dict(job_id=job_id if owner.cancel_mode != 'wrong_job' else 'job-other',
                                             accepted=1 if owner.cancel_mode == 'untyped' else True))
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                owner.posts.append((self.path, self.headers['Idempotency-Key'], body))
                job_id = 'job-' + str(len(owner.posts))
                owner.jobs[job_id] = body
                if owner.post_mode == 'drop':
                    self.connection.shutdown(socket.SHUT_RDWR)
                    self.connection.close()
                    return
                if owner.post_mode == 'malformed':
                    return self.respond(dict(job_id=job_id, state=True, revision='rev-a'), 202)
                if owner.post_mode == 'ledger_readonly':
                    owner.adapter._db.execute('PRAGMA query_only=ON')
                self.respond(dict(job_id=job_id, state='Queued', revision='rev-a'), 202)
            def log_message(self, *args):
                pass
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = Thread(target=self.server.serve_forever, kwargs={'poll_interval': 0.01}, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_server)
        self.adapter = self.create_adapter()
        self.addCleanup(self.adapter.close)

    def stop_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)

    def evidence(self, job_id, *, project_id, revision, snapshot_id):
        anchor = EvidenceAnchor('ev-' + job_id, project_id, revision, job_id, snapshot_id,
                                'artifact-' + job_id, 'a' * 64)
        return Evidence('ev-' + job_id, project_id, revision, job_id, 'counter', True,
                        'Fixture counter observation', origin='fixture', anchor=anchor)

    def create_adapter(self, **kwargs):
        obj = GatewayVerification(GatewayConfig(self.server.server_port, 'token-secret', 'inst-a', timeout=0.3),
            self.journal, self.evidence, wait_timeout=0.08, poll_interval=0.005, **kwargs)
        obj.register_approval(self.plan, self.package, 'ui-approved-hash')
        return obj

    def execute(self, index=0, key='action-build'):
        return self.adapter.execute(self.plan, index, 'grant-secret', key)

    def test_new_revision_denied_before_job_post_or_submission_reservation(self):
        self.context['revision'] = 'rev-new'
        self.assertFalse(self.adapter.authorized(self.plan, 'grant-secret'))
        with self.assertRaisesRegex(GatewayError, 'STALE_REVISION'):
            self.execute()
        self.assertEqual(self.posts, [])
        self.assertEqual(self.adapter._db.execute('SELECT COUNT(*) FROM verification_requests').fetchone()[0], 0)

    def test_unsaved_or_unsynchronized_context_denied_before_submission(self):
        for changes in ({'dirty': True}, {'synchronized': False}):
            with self.subTest(changes=changes):
                original = self.context.copy()
                self.context.update(changes)
                with self.assertRaisesRegex(GatewayError, 'STALE_REVISION'):
                    self.execute()
                self.context = original
        self.assertEqual(self.posts, [])
        self.assertEqual(self.adapter._db.execute('SELECT COUNT(*) FROM verification_requests').fetchone()[0], 0)

    def test_wrong_project_and_untyped_context_fail_closed(self):
        for changes in ({'project_id': 'project-other'}, {'revision': None}, {'revision': ''},
                        {'dirty': 0}, {'synchronized': 1}):
            with self.subTest(changes=changes):
                original = self.context.copy()
                self.context.update(changes)
                with self.assertRaisesRegex(GatewayError, 'INVALID_RESPONSE'):
                    self.execute()
                self.context = original
        self.assertEqual(self.posts, [])
        self.assertEqual(self.adapter._db.execute('SELECT COUNT(*) FROM verification_requests').fetchone()[0], 0)

    def test_known_job_readonly_observation_survives_current_revision_change(self):
        self.assertEqual(self.execute().state, 'Succeeded')
        self.context.update(revision='rev-new', dirty=True, synchronized=False)
        gets_before = len(self.gets)
        result = self.adapter.observe(self.plan, 0, 'action-build')
        self.assertEqual((result.state, result.job_id, result.evidence.revision), ('Succeeded', 'job-1', 'rev-a'))
        self.assertEqual(len(self.posts), 1)
        self.assertFalse(any(path.endswith('/context') for path, _ in self.gets[gets_before:]))

    def test_success_and_idempotent_duplicate_have_one_submission(self):
        self.assertTrue(self.adapter.authorized(self.plan, 'grant-secret'))
        result = self.execute()
        self.assertEqual(result.state, 'Succeeded')
        self.assertEqual(result.evidence.job_id, 'job-1')
        self.assertIsNone(result.matches_expected)
        self.assertEqual(self.execute().evidence.job_id, 'job-1')
        self.assertEqual(len(self.posts), 1)
        self.assertEqual(self.posts[0][1], 'action-build')
        self.assertEqual(self.posts[0][2], dict(snapshot_id='snap-a', expected_revision='rev-a',
            capability='eda.sim.build', grant_id='grant-secret', plugin_id='eda-sim-verilator',
            params={'top_module': 'counter', 'testbench': 'counter_tb'}, plan_hash='ui-approved-hash', step_id='s1'))

    def test_empty_and_memory_journals_are_rejected_before_use(self):
        config = self.adapter.config
        for path in ('', ':memory:'):
            with self.subTest(path=path):
                with self.assertRaisesRegex(ValueError, 'durable_journal_required'):
                    obj = GatewayVerification(config, path, self.evidence)
                    self.addCleanup(obj.close)

    def test_success_does_not_infer_expected_observation(self):
        adapter = self.create_adapter(expected_resolver=lambda p, i, e: True)
        self.addCleanup(adapter.close)
        self.assertIs(adapter.execute(self.plan, 0, 'grant-secret', 'expected-action').matches_expected, True)

    def test_grant_scope_steps_and_hash_mismatch_denied_before_post(self):
        original = deepcopy(self.grant)
        changes = [('project_id', 'other'), ('revision', 'other'), ('snapshot_id', 'other'),
                   ('plan_hash', self.plan.fingerprint), ('status', 'revoked'), ('steps', []),
                   ('max_jobs', True), ('expires_at', '2000-01-01T00:00:00Z')]
        for key, value in changes:
            self.grant = dict(original, **{key: value})
            self.assertFalse(self.adapter.authorized(self.plan, 'grant-secret'), key)
        for key, value in [('provider', 'iverilog'), ('params', {'top_module': 'other'}), ('depends_on', ['s2'])]:
            self.grant = deepcopy(original)
            self.grant['steps'][0][key] = value
            self.assertFalse(self.adapter.authorized(self.plan, 'grant-secret'), key)
        self.assertFalse(self.posts)

    def test_grant_reference_and_plan_path_injection_denied(self):
        self.assertFalse(self.adapter.authorized(self.plan, '../grants'))
        self.assertFalse(self.adapter.authorized(replace(self.plan, project_id='../escape'), 'grant-secret'))
        self.assertFalse(self.adapter.authorized(Plan('p', 'r', 'goal', ('eda.synth',)), 'grant-secret'))
        self.assertFalse(self.posts)

    def test_tampered_approval_and_no_trusted_approval_denied(self):
        bad = json.loads(self.package._material_json)
        bad['card']['steps'][0]['params']['top_module'] = 'other'
        with self.assertRaises(ValueError):
            self.adapter.register_approval(self.plan, ApprovalPackage(json.dumps(bad)), 'ui-approved-hash')
        changed = replace(self.plan, goal='Different expected intent')
        self.assertFalse(self.adapter.authorized(changed, 'grant-secret'))

    def test_unsupported_iverilog_and_timeout_fail_closed(self):
        for changed in [replace(self.plan, steps=tuple(replace(s, provider='iverilog') for s in self.plan.steps)),
                        replace(self.plan, steps=(self.plan.steps[0], replace(self.plan.steps[1],
                            params={'build_step_id': 'compile', 'timeout_ms': 100})))]:
            self.assertFalse(self.adapter.authorized(changed, 'grant-secret'))

    def test_instance_protocol_edition_handshake_before_credentials(self):
        for key, value in [('instance_id', 'foreign'), ('protocol_version', 'foreign'), ('edition', 'pro')]:
            original = self.health[key]
            self.health[key] = value
            before = len(self.gets)
            self.assertFalse(self.adapter.authorized(self.plan, 'grant-secret'))
            self.assertEqual(self.gets[before:], [('/api/v1/health', None)])
            self.health[key] = original

    def test_real_core_distinct_build_run_provider_and_readonly_observe(self):
        self.assertTrue(self.adapter.authorized(self.plan, 'grant-secret'))
        self.execute()
        result = self.execute(1, 'run-action')
        self.assertEqual(result.state, 'Succeeded')
        self.assertEqual(self.posts[0][2]['plugin_id'], 'eda-sim-verilator')
        self.assertEqual(self.posts[1][2]['plugin_id'], 'eda-sim-verilator-run')
        self.assertEqual(self.posts[1][2]['params'], {'build_job_id': 'job-1'})
        self.assertEqual(self.adapter.observe(self.plan, 1, 'run-action').state, 'Succeeded')
        self.assertEqual(len(self.posts), 2)

    def test_build_plugin_advertised_for_run_is_denied_before_submission(self):
        self.run_plugin = 'eda-sim-verilator'
        self.assertFalse(self.adapter.authorized(self.plan, 'grant-secret'))
        self.assertFalse(self.posts)

    def test_sim_run_requires_this_plans_successful_build(self):
        with self.assertRaises(GatewayError):
            self.execute(1, 'run-action')
        self.execute()
        self.execute(1, 'run-action')
        self.assertEqual(self.posts[-1][2]['params'], {'build_job_id': 'job-1'})
        foreign = replace(self.plan, goal='another approved plan')
        self.adapter.register_approval(foreign, build_plan_card(foreign, 'plan-2', 0), 'other-hash')
        self.grant['plan_hash'] = 'other-hash'
        with self.assertRaises(GatewayError):
            self.adapter.execute(foreign, 1, 'grant-secret', 'foreign-run')
        self.assertEqual(len(self.posts), 2)

    def test_failure_cancellation_and_timeout_stop_later_steps(self):
        for state in ['Failed', 'Cancelled', 'TimedOut', 'Unknown']:
            self.adapter.close()
            self.journal = Path(self.temp.name) / (state + '.sqlite')
            self.adapter = self.create_adapter()
            self.addCleanup(self.adapter.close)
            self.job_state = state
            result = self.execute(key='failed-' + state)
            self.assertEqual(result.state, state)
            self.assertIsNone(result.evidence)
            with self.assertRaises(GatewayError):
                self.execute(1, 'run-' + state)
        self.assertEqual(len(self.posts), 4)

    def test_wait_is_bounded_and_repeat_only_polls_known_job(self):
        self.job_state = 'Running'
        start = time.monotonic()
        result = self.execute()
        self.assertEqual(result.state, 'Running')
        self.assertLess(time.monotonic() - start, 0.8)
        self.job_state = 'Succeeded'
        self.assertEqual(self.execute().state, 'Succeeded')
        self.assertEqual(len(self.posts), 1)

    def test_unknown_post_is_durable_and_never_automatically_resubmitted(self):
        self.post_mode = 'drop'
        self.assertEqual(self.execute().state, 'Unknown')
        self.adapter.close()
        self.adapter = self.create_adapter()
        self.addCleanup(self.adapter.close)
        self.assertEqual(self.execute().state, 'Unknown')
        self.assertEqual(len(self.posts), 1)
        pending = self.adapter.pending()
        self.assertEqual(len(pending), 1)
        self.assertIsNone(pending[0]['job_id'])
        raw = self.journal.read_bytes()
        self.assertNotIn(b'grant-secret', raw)
        self.assertNotIn(b'token-secret', raw)

    def test_malformed_post_keeps_unknown_instead_of_retry(self):
        self.post_mode = 'malformed'
        self.assertEqual(self.execute().state, 'Unknown')
        self.assertEqual(self.execute().state, 'Unknown')
        self.assertEqual(len(self.posts), 1)

    def test_same_key_different_step_or_grant_is_conflict(self):
        self.execute()
        with self.assertRaisesRegex(GatewayError, 'IDEMPOTENCY_CONFLICT'):
            self.execute(1)
        self.grant['grant_id'] = 'grant-other'
        with self.assertRaisesRegex(GatewayError, 'IDEMPOTENCY_CONFLICT'):
            self.adapter.execute(self.plan, 0, 'grant-other', 'action-build')
        self.assertEqual(len(self.posts), 1)

    def test_unknown_can_only_reconcile_exact_actual_job_binding(self):
        self.post_mode = 'drop'
        self.execute()
        binding = dict(job_id='job-1', instance_id='inst-a', project_id='project-a', revision='rev-a',
            snapshot_id='snap-a', capability='eda.sim.build', step_id='s1', plan_hash='ui-approved-hash',
            grant_id='grant-secret', idempotency_key='action-build',
            schema='sigflow.edu-agent-job-binding.v1', idempotency_scope='project-a|eda.sim.build',
            request_hash=hashlib.sha256(b'{"capability":"eda.sim.build","params":{"testbench":"counter_tb","top_module":"counter"},"plugin_id":"eda-sim-verilator","snapshot_id":"snap-a"}').hexdigest())
        with self.assertRaises(GatewayError):
            self.adapter.reconcile(self.plan, 0, 'grant-secret', 'action-build', dict(binding, snapshot_id='other'))
        result = self.adapter.reconcile(self.plan, 0, 'grant-secret', 'action-build', binding)
        self.assertEqual(result.state, 'Succeeded')
        self.assertEqual(self.execute().state, 'Succeeded')
        self.assertEqual(len(self.posts), 1)

    def test_report_wrong_provider_or_missing_verified_anchor_not_success_evidence(self):
        self.report_override = {'plugin_version': '1'}
        self.adapter.report_resolver = lambda *a, **k: replace(self.evidence(*a, **k), anchor=None)
        result = self.execute()
        self.assertIsNone(result.evidence)
        self.assertIsNone(result.matches_expected)

    def test_wrong_report_binding_or_provider_cannot_enable_build_dependency(self):
        for field, value in [('snapshot_id', 'foreign'), ('plugin_id', 'eda-sim-other')]:
            self.adapter.close()
            self.journal = Path(self.temp.name) / (field + '.sqlite')
            self.adapter = self.create_adapter()
            self.addCleanup(self.adapter.close)
            self.report_override = {field: value}
            self.assertEqual(self.execute(key=field).state, 'Unknown')
            with self.assertRaises(GatewayError):
                self.execute(1, 'run-' + field)
        self.assertEqual(len(self.posts), 2)

    def test_same_step_cannot_be_submitted_again_under_another_key(self):
        self.execute()
        with self.assertRaisesRegex(GatewayError, 'IDEMPOTENCY_CONFLICT'):
            self.execute(key='new-build-action')
        self.assertEqual(len(self.posts), 1)

    def test_typed_grant_step_bool_budget_is_rejected(self):
        self.grant['steps'][0]['max_jobs'] = True
        self.assertFalse(self.adapter.authorized(self.plan, 'grant-secret'))

    def test_partial_report_never_certifies_observations(self):
        self.report_override = {'completeness': 'partial'}
        self.assertIsNone(self.execute().evidence)

    def test_observation_mismatch_stops_later_steps(self):
        adapter = self.create_adapter(expected_resolver=lambda p, i, e: False)
        self.addCleanup(adapter.close)
        self.assertIs(adapter.execute(self.plan, 0, 'grant-secret', 'mismatch').matches_expected, False)
        with self.assertRaises(GatewayError):
            adapter.execute(self.plan, 1, 'grant-secret', 'after-mismatch')
        self.assertEqual(len(self.posts), 1)

    def test_mismatch_remains_stopped_after_missing_evidence_and_restart(self):
        self.adapter.expected_resolver = lambda p, i, e: False
        self.assertIs(self.execute().matches_expected, False)
        self.adapter.close()
        self.adapter = self.create_adapter()
        self.addCleanup(self.adapter.close)
        self.adapter.report_resolver = lambda *a, **k: None
        self.execute()
        with self.assertRaises(GatewayError):
            self.execute(1, 'after-lost-evidence')
        self.assertEqual(len(self.posts), 1)

    def test_mismatch_remains_stopped_after_invalid_report_refresh(self):
        self.adapter.expected_resolver = lambda p, i, e: False
        self.execute()
        self.report_override = {'plugin_id': 'foreign-provider'}
        self.execute()
        self.report_override = {}
        self.adapter.expected_resolver = lambda p, i, e: True
        self.execute()
        with self.assertRaises(GatewayError):
            self.execute(1, 'after-refreshed-mismatch')
        self.assertEqual(len(self.posts), 1)

    def test_invalid_refresh_cannot_leave_successful_build_eligible(self):
        self.execute()
        self.report_override = {'plugin_id': 'foreign-provider'}
        self.assertEqual(self.execute().state, 'Unknown')
        with self.assertRaises(GatewayError):
            self.execute(1, 'after-invalid-refresh')
        self.assertEqual(len(self.posts), 1)

    def test_terminal_failure_is_sticky_on_duplicate_observation(self):
        self.job_state = 'Failed'
        self.assertEqual(self.execute().state, 'Failed')
        self.job_state = 'Succeeded'
        self.assertEqual(self.execute().state, 'Failed')
        with self.assertRaises(GatewayError):
            self.execute(1, 'after-failure')

    def test_disk_refusal_before_reservation_cannot_submit(self):
        self.adapter._db.execute('PRAGMA query_only=ON')
        with self.assertRaises(sqlite3.OperationalError):
            self.execute()
        self.assertFalse(self.posts)

    def test_disk_refusal_after_post_preserves_unknown_reservation(self):
        self.post_mode = 'ledger_readonly'
        with self.assertRaises(sqlite3.OperationalError):
            self.execute()
        self.adapter.close()
        self.adapter = self.create_adapter()
        self.addCleanup(self.adapter.close)
        self.assertEqual(self.execute().state, 'Unknown')
        self.assertEqual(len(self.posts), 1)

    def test_typed_job_response_rejects_boolean_exit_code(self):
        self.job_override = {'exit_code': True}
        self.assertEqual(self.execute().state, 'Unknown')
        with self.assertRaises(GatewayError):
            self.execute(1, 'typed-run')

    def test_untrusted_anchor_not_returned_even_with_successful_job(self):
        self.adapter.report_resolver = lambda *a, **k: replace(self.evidence(*a, **k),
            anchor=replace(self.evidence(*a, **k).anchor, artifact_hash='b' * 64))
        self.assertIsNone(self.execute().evidence)

    def test_restarted_known_job_reuses_original_id_without_new_post(self):
        self.job_state = 'Running'
        self.execute()
        self.adapter.close()
        self.adapter = self.create_adapter()
        self.addCleanup(self.adapter.close)
        self.job_state = 'Succeeded'
        self.assertEqual(self.execute().evidence.job_id, 'job-1')
        self.assertEqual(len(self.posts), 1)

    def test_synthesis_uses_only_real_yosys_mapping(self):
        self.plan = replace(self.plan, steps=(PlanStep('synth', 'eda.synth', 'yosys', {'top': 'counter'}),))
        self.package = build_plan_card(self.plan, 'plan-2', 0)
        self.adapter.register_approval(self.plan, self.package, 'synth-hash')
        self.grant.update(steps=self.package.card['steps'], plan_hash='synth-hash')
        self.assertEqual(self.execute().state, 'Succeeded')
        self.assertEqual(self.posts[0][2]['plugin_id'], 'eda-synth-yosys')
        self.assertEqual(self.posts[0][2]['params'], {'top_module': 'counter'})

    def test_budget_exhaustion_cannot_prevent_safe_duplicate_observation(self):
        self.execute()
        self.grant['used_jobs'] = 3
        self.assertFalse(self.adapter.authorized(self.plan, 'grant-secret'))
        self.assertEqual(self.execute().state, 'Succeeded')
        with self.assertRaises(GatewayError):
            self.execute(1, 'over-budget')
        self.assertEqual(len(self.posts), 1)

    def test_callback_failure_exposes_no_sensitive_exception(self):
        def unavailable(*args, **kwargs):
            raise ValueError('private-host-path-and-key')
        self.adapter.report_resolver = unavailable
        self.assertIsNone(self.execute().evidence)

    def test_cross_instance_restart_cannot_reuse_prior_key(self):
        self.execute()
        foreign = GatewayVerification(GatewayConfig(self.server.server_port, 'other-token', 'inst-other'),
            self.journal, self.evidence, wait_timeout=0.05)
        self.addCleanup(foreign.close)
        foreign.register_approval(self.plan, self.package, 'ui-approved-hash')
        self.health['instance_id'] = 'inst-other'
        with self.assertRaisesRegex(GatewayError, 'IDEMPOTENCY_CONFLICT'):
            foreign.execute(self.plan, 0, 'grant-secret', 'action-build')
        self.assertEqual(foreign.pending(), ())
        self.assertEqual(len(self.posts), 1)

    def test_reconciliation_cannot_clear_previously_observed_terminal_failure(self):
        self.job_state = 'Failed'
        self.assertEqual(self.execute().state, 'Failed')
        binding = dict(job_id='job-1', instance_id='inst-a', project_id='project-a', revision='rev-a',
            snapshot_id='snap-a', capability='eda.sim.build', step_id='s1', plan_hash='ui-approved-hash',
            grant_id='grant-secret', idempotency_key='action-build',
            schema='sigflow.edu-agent-job-binding.v1', idempotency_scope='project-a|eda.sim.build',
            request_hash=hashlib.sha256(b'{"capability":"eda.sim.build","params":{"testbench":"counter_tb","top_module":"counter"},"plugin_id":"eda-sim-verilator","snapshot_id":"snap-a"}').hexdigest())
        self.job_state = 'Succeeded'
        with self.assertRaises(GatewayError):
            self.adapter.reconcile(self.plan, 0, 'grant-secret', 'action-build', binding)
        self.assertEqual(self.execute().state, 'Failed')

    def test_cancel_owned_only_targets_exact_plan_journal_job(self):
        self.job_state = 'Running'
        self.execute()
        foreign = replace(self.plan, goal='Different approved goal')
        package = build_plan_card(foreign, 'plan-2', 0)
        self.adapter.register_approval(foreign, package, 'foreign-hash')
        self.grant.update(steps=package.card['steps'], plan_hash='foreign-hash')
        self.adapter.execute(foreign, 0, 'grant-secret', 'foreign-build')
        results = self.adapter.cancel_owned(self.plan)
        self.assertEqual(results, ({'job_id': 'job-1', 'status': 'CancelRequested', 'cancel_requested': True},))
        self.assertEqual(self.cancel_posts, ['job-1'])
        self.assertEqual(len(self.posts), 2)
        self.assertEqual(self.job_state, 'Running')

    def test_cancel_owned_avoids_terminal_and_unknown_id_jobs(self):
        self.execute()
        self.assertEqual(self.adapter.cancel_owned(self.plan),
            ({'job_id': 'job-1', 'status': 'Succeeded', 'cancel_requested': False},))
        self.assertFalse(self.cancel_posts)

        self.adapter.close()
        self.journal = Path(self.temp.name) / 'unknown-cancel.sqlite'
        self.adapter = self.create_adapter()
        self.addCleanup(self.adapter.close)
        self.post_mode = 'drop'
        self.execute()
        self.assertEqual(self.adapter.cancel_owned(self.plan), ())
        self.assertFalse(self.cancel_posts)

    def test_cancel_before_any_job_persistently_fences_later_submission(self):
        self.assertEqual(self.adapter.cancel_owned(self.plan), ())
        self.adapter.close()
        self.adapter = self.create_adapter()
        self.addCleanup(self.adapter.close)
        with self.assertRaisesRegex(GatewayError, 'POLICY_DENIED'):
            self.execute()
        self.assertFalse(self.posts)

    def test_delayed_cancel_keeps_original_approval_and_cannot_fence_successor(self):
        original = self.adapter.bind(self.plan, 'ui-approved-hash')
        original.request_cancel(self.plan)
        self.adapter.register_approval(self.plan, self.package, 'successor-hash')
        successor = self.adapter.bind(self.plan, 'successor-hash')
        self.grant['plan_hash'] = 'successor-hash'
        self.assertEqual(original.cancel_owned(self.plan), ())
        result = successor.execute(self.plan, 0, 'grant-secret', 'successor-build')
        self.assertEqual(result.state, 'Succeeded')
        self.assertEqual(len(self.posts), 1)
        self.assertFalse(self.cancel_posts)

    def test_cancel_during_inflight_post_cancels_late_job_receipt(self):
        other = self.create_adapter()
        self.addCleanup(other.close)
        original = self.adapter._request
        def request(method, path, *args, **kwargs):
            if method == 'POST' and path.endswith('/jobs'):
                self.assertEqual(other.cancel_owned(self.plan), ())
            return original(method, path, *args, **kwargs)
        self.adapter._request = request
        self.job_state = 'Running'
        self.execute()
        self.assertEqual(self.cancel_posts, ['job-1'])

    def test_cancel_rejection_and_untyped_ack_never_claim_cancellation(self):
        self.job_state = 'Running'
        for mode, status in [('reject', 'POLICY_DENIED'), ('untyped', 'Unknown'), ('wrong_job', 'Unknown'), ('drop', 'Unknown')]:
            self.adapter.close()
            self.journal = Path(self.temp.name) / (mode + '-cancel.sqlite')
            self.adapter = self.create_adapter()
            self.addCleanup(self.adapter.close)
            self.execute()
            self.cancel_mode = mode
            result = self.adapter.cancel_owned(self.plan)
            job_id = 'job-' + str(len(self.posts))
            self.assertEqual(result, ({'job_id': job_id, 'status': status, 'cancel_requested': False},))
        self.assertEqual(self.cancel_posts, ['job-1', 'job-2', 'job-3', 'job-4'])

    def test_cancel_duplicate_ack_does_not_repeat_post(self):
        self.job_state = 'Running'
        self.execute()
        self.adapter.cancel_owned(self.plan)
        self.assertEqual(self.adapter.cancel_owned(self.plan),
            ({'job_id': 'job-1', 'status': 'CancelRequested', 'cancel_requested': True},))
        self.assertEqual(self.cancel_posts, ['job-1'])

    def test_concurrent_adapters_cancel_at_most_once(self):
        self.job_state = 'Running'
        self.execute()
        other = self.create_adapter()
        self.addCleanup(other.close)
        barrier = Barrier(2)
        for adapter in (self.adapter, other):
            original = adapter._job
            def together(*args, _job=original, **kwargs):
                result = _job(*args, **kwargs)
                barrier.wait(timeout=2)
                return result
            adapter._job = together
        with ThreadPoolExecutor(2) as pool:
            results = list(pool.map(lambda a: a.cancel_owned(self.plan), (self.adapter, other)))
        self.assertEqual(self.cancel_posts, ['job-1'])
        self.assertTrue(any(r[0]['cancel_requested'] for r in results))

    def test_cancel_wrong_actual_job_scope_is_rejected_before_post(self):
        self.job_state = 'Running'
        self.execute()
        self.job_override = {'project_id': 'foreign-project'}
        self.assertEqual(self.adapter.cancel_owned(self.plan),
            ({'job_id': 'job-1', 'status': 'INVALID_RESPONSE', 'cancel_requested': False},))
        self.assertFalse(self.cancel_posts)

    def test_cancel_after_close_is_safe_and_cannot_touch_gateway(self):
        self.job_state = 'Running'
        self.execute()
        self.adapter.close()
        before = len(self.gets)
        self.assertEqual(self.adapter.cancel_owned(self.plan), ())
        self.assertEqual(len(self.gets), before)
        self.assertFalse(self.cancel_posts)

    def test_unknown_cancel_is_durable_and_never_automatically_repeated(self):
        self.job_state = 'Running'
        self.execute()
        self.cancel_mode = 'drop'
        self.assertEqual(self.adapter.cancel_owned(self.plan)[0]['status'], 'Unknown')
        self.adapter.close()
        self.adapter = self.create_adapter()
        self.addCleanup(self.adapter.close)
        self.cancel_mode = 'normal'
        self.assertEqual(self.adapter.cancel_owned(self.plan),
            ({'job_id': 'job-1', 'status': 'Unknown', 'cancel_requested': False},))
        self.assertEqual(self.cancel_posts, ['job-1'])

    def test_cancel_intent_stops_later_execution_even_if_build_finishes(self):
        self.job_state = 'Running'
        self.execute()
        self.adapter.cancel_owned(self.plan)
        self.job_state = 'Succeeded'
        self.execute()
        with self.assertRaises(GatewayError):
            self.execute(1, 'after-cancel')
        self.assertEqual(len(self.posts), 1)

    def test_cancel_wait_is_bounded_before_post(self):
        self.job_state = 'Running'
        self.execute()
        self.job_delay = 0.2
        start = time.monotonic()
        self.assertEqual(self.adapter.cancel_owned(self.plan),
            ({'job_id': 'job-1', 'status': 'TIMEOUT', 'cancel_requested': False},))
        self.assertLess(time.monotonic() - start, 0.7)
        self.assertFalse(self.cancel_posts)

    def test_cancel_excludes_unbound_legacy_ledger_record(self):
        self.job_state = 'Running'
        self.execute()
        with self.adapter._db:
            self.adapter._db.execute("UPDATE verification_requests SET plan_digest=''")
        self.assertEqual(self.adapter.cancel_owned(self.plan), ())
        self.assertFalse(self.cancel_posts)

if __name__ == '__main__':
    unittest.main()
