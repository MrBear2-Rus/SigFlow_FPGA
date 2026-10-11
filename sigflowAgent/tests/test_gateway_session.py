from dataclasses import replace
import json
import os
import unittest
from unittest.mock import patch

from sigflow_edu_agent.clients.gateway import GatewayConfig
from sigflow_edu_agent.domain import ProviderError, RunRequest, Status
from sigflow_edu_agent.runtime.gateway_session import SessionBinding, create_gateway_session
from test_gateway_source import fixture


class GatewaySessionTests(unittest.TestCase):
    def setUp(self):
        self.report, self.anchor = fixture()
        self.binding = SessionBinding('inst-1', 'session-1', 'learner-1', 'prj-1', 'rev-12',
                                      'snap-12', 'job-21', 'diag-1', 'issue-1')
        self.request = RunRequest('prj-1', 'rev-12', 'issue-1', '为什么会推断锁存器？')
        self.config = GatewayConfig(12345, 'secret-token', 'inst-1')
        self.client_patch = patch('sigflow_edu_agent.runtime.gateway_session.GatewayClient')
        self.client = self.client_patch.start().return_value
        self.client.job_report.side_effect = lambda *a, **kw: self.report
        self.addCleanup(self.client_patch.stop)

    def create(self, **changes):
        args = dict(binding=self.binding, request=self.request, gateway=self.config,
                    anchors=[self.anchor], mode='rule', evidence_origin='fixture', combinational_goal=True)
        return create_gateway_session(**(args | changes))

    def advance(self, session):
        for _ in range(32):
            state = session.state
            if state.status != Status.RUNNING:
                return state
            session.advance(state.version)
        self.fail('session failed to pause')

    def test_rule_session_records_once_and_rereads_on_hint(self):
        session = self.create()
        self.client.job_report.assert_not_called()
        self.assertEqual(session.occurrences, ())
        state = self.advance(session)
        self.assertEqual(state.status, Status.WAITING_STUDENT)
        first = session.occurrences
        self.assertEqual(len(first), 1)
        self.assertEqual(first[0].binding, self.binding)
        self.assertNotIn('secret-token', repr(first))
        session.host_action('hint_next', 'a1', state.version)
        state = self.advance(session)
        self.assertEqual(state.level, 2)
        self.assertEqual(session.occurrences, first)
        self.assertEqual(self.client.job_report.call_count, 2)
        self.assertEqual(session.advance(state.version), state)

    def test_changed_same_report_is_rejected_without_new_card(self):
        session = self.create()
        state = self.advance(session)
        self.report['diagnostics'][0]['summary'] = 'changed observation'
        session.host_action('hint_next', 'a1', state.version)
        state = self.advance(session)
        self.assertEqual(state.status, Status.WAITING_EVIDENCE)
        self.assertEqual(state.reason, 'stale_report')
        self.assertEqual(len(state.cards), 1)
        self.assertEqual(len(session.occurrences), 1)

    def test_scope_mismatch_and_verify_fail_before_io(self):
        for changes in (dict(gateway=replace(self.config, instance_id='other')),
                        dict(request=replace(self.request, project_id='other')),
                        dict(request=replace(self.request, issue_id='other')),
                        dict(request=replace(self.request, intent='verify')),
                        dict(mode='unknown')):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.create(**changes)
        self.client.job_report.assert_not_called()

    def test_invalid_binding_rejected(self):
        for changes in (dict(learner_id=''), dict(session_id='../x'), dict(job_id=True), dict(policy='unknown')):
            with self.assertRaises(ValueError):
                replace(self.binding, **changes)

    def test_default_deepseek_requires_configuration(self):
        with patch.dict(os.environ, {}, clear=True), self.assertRaises(ProviderError):
            create_gateway_session(binding=self.binding, request=self.request, gateway=self.config,
                                   anchors=[self.anchor])
        self.client.job_report.assert_not_called()

    @patch('sigflow_edu_agent.runtime.gateway_session.DeepSeekProvider.from_env')
    def test_missing_evidence_never_calls_model_or_records_occurrence(self, provider):
        session = self.create(mode='deepseek', anchors=[])
        state = self.advance(session)
        self.assertEqual(state.status, Status.WAITING_EVIDENCE)
        provider.return_value.generate.assert_not_called()
        self.assertEqual(session.occurrences, ())

    def test_sessions_have_separate_records(self):
        one = self.create()
        two = self.create(binding=replace(self.binding, session_id='session-2', learner_id='learner-2'))
        self.advance(one)
        self.assertEqual(two.occurrences, ())
        self.advance(two)
        self.assertNotEqual(one.occurrences[0].binding, two.occurrences[0].binding)

    @patch('sigflow_edu_agent.deepseek.http.client.HTTPSConnection')
    def test_actual_provider_is_composed_and_only_sees_question_and_evidence(self, connection):
        reply = connection.return_value.getresponse.return_value
        reply.status = 200
        card = dict(level=1, hint='报告观察到存储行为。', question='输出需要记忆吗？', evidence_ids=['ev-1'])
        reply.read.return_value = json.dumps({'choices': [{'finish_reason': 'stop',
                                                          'message': {'content': json.dumps(card)}}]}).encode()
        with patch.dict(os.environ, {'DEEPSEEK_API_KEY': 'fake-key', 'DEEPSEEK_MODEL': 'test-model'}):
            session = self.create(mode='deepseek')
        connection.assert_not_called()
        state = self.advance(session)
        self.assertEqual(state.model_calls, 1)
        self.assertEqual(state.cards[0].producer, 'model')
        body = json.loads(connection.return_value.request.call_args.kwargs['body'])
        context = json.loads(body['messages'][1]['content'])
        self.assertEqual(context['evidence']['origin'], 'fixture')
        for secret in ('learner-1', 'session-1', 'fake-key', 'secret-token'):
            self.assertNotIn(secret, json.dumps(body))

    def test_explain_completes_and_l4_stays_denied(self):
        session = self.create(request=replace(self.request, intent='explain'))
        self.assertEqual(self.advance(session).status, Status.COMPLETED)
        session = self.create()
        state = self.advance(session)
        with self.assertRaisesRegex(ValueError, 'receipt_unavailable'):
            session.host_action('reference_request', 'reference-1', state.version)
        self.assertLessEqual(session.state.level, 3)
        self.assertTrue(all(card.level <= 3 for card in session.state.cards))


class GatewaySessionHttpTests(unittest.TestCase):
    def test_factory_uses_real_read_transport_and_stops_on_new_revision(self):
        from test_gateway_source import GatewaySourceHTTPTests
        fixture_case = GatewaySourceHTTPTests()
        fixture_case.setUp()
        self.addCleanup(fixture_case.doCleanups)
        binding = SessionBinding('inst-1', 'session-1', 'learner-1', 'prj-1', 'rev-12',
                                 'snap-12', 'job-21', 'diag-1', 'issue-1')
        session = create_gateway_session(
            binding=binding, request=RunRequest('prj-1', 'rev-12', 'issue-1', '解释锁存器'),
            gateway=GatewayConfig(fixture_case.server.server_port, 'fixture-token', 'inst-1'),
            anchors=[fixture_case.anchor], mode='rule', evidence_origin='fixture')
        while session.state.status == Status.RUNNING:
            session.advance(session.state.version)
        self.assertEqual(session.state.status, Status.WAITING_STUDENT)
        self.assertEqual(len(session.occurrences), 1)
        fixture_case.report['revision'] = 'old'
        session.host_action('hint_next', 'next', session.state.version)
        while session.state.status == Status.RUNNING:
            session.advance(session.state.version)
        self.assertEqual(session.state.status, Status.WAITING_EVIDENCE)
        self.assertEqual(len(session.state.cards), 1)
        self.assertEqual(fixture_case.requests, [('/api/v1/health', None),
                         ('/api/v1/jobs/job-21/report', 'Bearer fixture-token')] * 2)
