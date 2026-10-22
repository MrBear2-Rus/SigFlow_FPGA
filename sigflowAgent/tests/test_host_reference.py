"""Internal metadata actions against real teaching/SQLite; synthetic reviewed material.

No real reviewers, model, GUI approval or EDA execution is exercised here.
"""
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import tempfile
import time
import unittest
from threading import Event, Thread

from sigflow_edu_agent.clients.gateway import GatewayError
from sigflow_edu_agent.demo import FixtureSource
from sigflow_edu_agent.host.reference import ReviewedReference
from sigflow_edu_agent.host.runtime import Runtime
from sigflow_edu_agent.host.store import DurableStore
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.runtime.session import TeachingSession


class ReceiptFixture:
    def __init__(self):
        self.record = None
        self.calls = []
        self.lookups = 0
        self.executions = 0
        self.lose_response = False
        self.after_consume = lambda: None

    def issue(self, challenge):
        self.record = {key: challenge[key] for key in (
            'session_id', 'project_id', 'revision', 'challenge_id', 'action_id',
            'policy', 'state_version', 'level')}
        self.record.update(receipt_id='ui-receipt-1', status='active')
        self.run_id = challenge['run_id']

    def lookup(self, receipt_id):
        self.lookups += 1
        return dict(self.record)

    def consume(self, receipt_id, project, run_id, action, version):
        args = (receipt_id, project, run_id, action, version)
        expected = (self.record['receipt_id'], self.record['project_id'], self.run_id,
                    self.record['action_id'], int(self.record['state_version']))
        if args != expected:
            raise GatewayError('POLICY_DENIED')
        self.calls.append(args)
        replayed = self.record['status'] == 'consumed'
        if not replayed:
            self.executions += 1
            self.record['status'] = 'consumed'
        self.after_consume()
        if self.lose_response:
            self.lose_response = False
            raise GatewayError('TIMEOUT')
        return dict(receipt_id=receipt_id, action_id=action, status='consumed',
                    consumed=True, replayed=replayed)


class HostReferenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'host.sqlite'
        self.source = FixtureSource()
        self.port = ReceiptFixture()
        self.content = 'Synthetic reviewed reference body excluded from host storage 97285'
        self.item = ReviewedReference('synthetic-ref', self.content,
            hashlib.sha256(self.content.encode()).hexdigest(), ('fixture-a', 'fixture-b'))
        self.body = dict(learner_id='learner', project_id='p', revision='r', snapshot_id='snap',
            job_id='fixture-job-1', diagnostic_id='diag', issue_id='issue',
            question='Explain the latch diagnostic', mode='rule', intent='diagnose')
        self.open()
        self.addCleanup(self.close)

    def open(self, configured=True):
        def factory(binding, request, mode, reserve):
            return TeachingSession(TeachingLoop(request, self.source))
        self.store = DurableStore(self.path, 'inst')
        options = (dict(reference_provider=lambda binding: self.item, receipt_port=self.port)
                   if configured else {})
        self.runtime = Runtime(self.store, 'inst', factory, evidence_origin='fixture', **options)

    def close(self):
        self.runtime.close()
        self.store.close()

    def wait(self, sid, aid):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            if self.store.action(sid, aid)['status'] in ('committed', 'interrupted'):
                return self.runtime.snapshot(sid, 'learner')
            time.sleep(.01)
        self.fail('worker did not finish')

    def create(self, level=3):
        sid = self.runtime.create(self.body, 'create')['session_id']
        state = self.wait(sid, 'create')
        for n in range(1, level):
            aid = 'hint-' + str(n)
            self.runtime.action(sid, 'learner', 'hint_next', aid, int(state['state_version']))
            state = self.wait(sid, aid)
        return sid, state

    def action(self, sid, state, kind, aid, payload=None):
        self.runtime.action(sid, 'learner', kind, aid, int(state['state_version']), reference=payload or {})
        return self.wait(sid, aid)

    def request(self):
        sid, state = self.create()
        requested = self.action(sid, state, 'reference_request', 'request')
        challenge = requested['reference']['challenge']
        self.port.issue(challenge)
        payload = dict(challenge_id=challenge['challenge_id'], receipt_id='ui-receipt-1')
        return sid, state, requested, payload

    def assert_no_private_publication(self, sid):
        row = self.store.get(sid)
        with self.store._transaction() as db:
            actions = [dict(r) for r in db.execute('SELECT * FROM actions')]
        public = json.dumps([row, actions, self.runtime.events(sid, 'learner', 0)])
        self.assertNotIn(self.content, public)
        self.assertNotIn('ui-receipt-1', public)
        self.assertNotIn(self.content.encode(), self.path.read_bytes())

    def test_request_and_confirm_publish_only_metadata_without_advancing_level(self):
        sid, before, state, payload = self.request()
        self.assertEqual(state['reference']['status'], 'awaiting_confirmation')
        self.assertEqual(state['reference']['challenge']['state_version'], state['state_version'])
        self.assertEqual(self.port.lookups, 0)
        confirmed = self.action(sid, state, 'reference_confirm', 'confirm', payload)
        self.assertEqual(confirmed['reference']['status'], 'confirmed')
        self.assertFalse(confirmed['reference']['body_available'])
        self.assertEqual(confirmed['reference']['sha256'], self.item.sha256)
        self.assertEqual((confirmed['level'], len(confirmed['cards']), confirmed['model_calls']), (3, 3, 0))
        self.assertEqual(confirmed['steps'], before['steps'] + 2)
        self.assertEqual((len(self.port.calls), self.port.executions), (1, 1))
        self.assert_no_private_publication(sid)

    def test_confirm_replay_is_idempotent_and_changed_receipt_conflicts(self):
        sid, _, state, payload = self.request()
        confirmed = self.action(sid, state, 'reference_confirm', 'confirm', payload)
        replay = self.runtime.action(sid, 'learner', 'reference_confirm', 'confirm',
            int(state['state_version']), reference=payload)
        self.assertEqual(replay['result']['reference'], confirmed['reference'])
        self.assertEqual(len(self.port.calls), 1)
        with self.assertRaisesRegex(ValueError, 'action_conflict'):
            self.runtime.action(sid, 'learner', 'reference_confirm', 'confirm',
                int(state['state_version']), reference=payload | dict(receipt_id='ui-receipt-2'))

    def test_unknown_consume_survives_restart_without_automatic_retry(self):
        sid, _, state, payload = self.request()
        self.port.lose_response = True
        pending = self.action(sid, state, 'reference_confirm', 'confirm', payload)
        self.assertEqual((pending['status'], pending['reference']['status']), ('WaitingStudent', 'pending'))
        calls = list(self.port.calls)
        self.close()
        self.open()
        quarantined = self.runtime.snapshot(sid, 'learner')
        self.assertEqual(quarantined['reference'], dict(status='revalidation_required'))
        with self.assertRaisesRegex(ValueError, 'recovery_required'):
            self.runtime.action(sid, 'learner', 'reference_reconcile', 'too-soon',
                int(quarantined['state_version']), reference={})
        self.runtime.recover(sid, 'learner', 'recover', int(quarantined['state_version']), self.body['question'])
        recovered = self.wait(sid, 'recover')
        self.assertEqual(self.port.calls, calls)
        self.assertEqual(recovered['reference']['status'], 'reconcile_required')
        reconciled = self.action(sid, recovered, 'reference_reconcile', 'reconcile')
        self.assertEqual(reconciled['reference']['status'], 'confirmed')
        self.assertEqual((self.port.executions, len(self.port.calls)), (1, 2))
        self.assertEqual(self.port.calls[0], self.port.calls[1])
        self.assert_no_private_publication(sid)

    def test_ready_challenge_cannot_confirm_after_restart_revalidation(self):
        sid, _, state, payload = self.request()
        self.close()
        self.open()
        quarantined = self.runtime.snapshot(sid, 'learner')
        self.runtime.recover(sid, 'learner', 'recover', int(quarantined['state_version']), self.body['question'])
        recovered = self.wait(sid, 'recover')
        with self.assertRaisesRegex(ValueError, 'reference_challenge_stale'):
            self.runtime.action(sid, 'learner', 'reference_confirm', 'stale-confirm',
                int(recovered['state_version']), reference=payload)
        replay = self.runtime.action(sid, 'learner', 'reference_request', 'request',
            int(state['state_version']) - 1, reference={})
        self.assertNotIn('challenge', replay['result']['reference'])
        self.assertEqual(self.port.lookups, 0)

    def test_finish_revokes_reference_and_historic_request_replay(self):
        sid, before, state, _ = self.request()
        self.runtime.action(sid, 'learner', 'finish', 'finish', int(state['state_version']))
        finished = self.wait(sid, 'finish')
        self.assertNotIn('challenge', finished['reference'])
        replay = self.runtime.action(sid, 'learner', 'reference_request', 'request',
            int(before['state_version']), reference={})
        self.assertNotIn('challenge', replay['result']['reference'])
        self.assertEqual(self.port.lookups, 0)

    def test_cancellation_during_consume_blocks_publication_and_preserves_audit(self):
        from sigflow_edu_agent.host.reference_journal import ReferenceJournal
        from sigflow_edu_agent.runtime.gateway_session import SessionBinding
        sid, _, state, payload = self.request()
        entered, release = Event(), Event()
        def hold():
            entered.set()
            release.wait(3)
        self.port.after_consume = hold
        self.runtime.action(sid, 'learner', 'reference_confirm', 'confirm', int(state['state_version']), reference=payload)
        cancelled_done = Event()
        cancel_thread = Thread(target=lambda: (self.runtime.cancel(sid, 'learner'), cancelled_done.set()), daemon=True)
        try:
            self.assertTrue(entered.wait(2))
            cancel_thread.start()
            self.assertTrue(cancelled_done.wait(.5), 'cancel waits on blocking receipt while holding runtime guard')
            cancelled = self.runtime.snapshot(sid, 'learner')
            self.assertEqual(cancelled['reference'], dict(status='cancelled'))
        finally:
            release.set()
            if cancel_thread.ident is not None:
                cancel_thread.join(2)
            self.runtime.close()
        row = self.store.get(sid)
        record = ReferenceJournal(self.store).get(SessionBinding(**row['binding']), row['state']['run_id'])
        self.assertEqual((record['phase'], record['receipt_id']), ('invalid', 'ui-receipt-1'))
        self.assert_no_private_publication(sid)

    def test_second_action_rejects_busy_without_blocking_cancel_or_snapshots(self):
        sid, _, state, payload = self.request()
        entered, release, returned = Event(), Event(), Event()
        errors = []
        def hold():
            entered.set()
            release.wait(3)
        def second():
            try:
                self.runtime.action(sid, 'learner', 'reference_request', 'second',
                    int(state['state_version']), reference={})
            except ValueError as error:
                errors.append(str(error))
            finally:
                returned.set()
        self.port.after_consume = hold
        self.runtime.action(sid, 'learner', 'reference_confirm', 'confirm', int(state['state_version']), reference=payload)
        second_thread = Thread(target=second, daemon=True)
        try:
            self.assertTrue(entered.wait(2))
            second_thread.start()
            self.assertTrue(returned.wait(.5), 'busy request waits on session lock under global runtime guard')
            self.assertEqual(errors, ['session_busy'])
            self.assertEqual(self.runtime.snapshot(sid, 'learner')['state_version'], state['state_version'])
            self.runtime.cancel(sid, 'learner')
        finally:
            release.set()
            if second_thread.ident is not None:
                second_thread.join(2)
            self.runtime.close()

    def test_material_change_during_consume_never_publishes_confirmed_metadata(self):
        sid, _, state, payload = self.request()
        self.port.after_consume = lambda: setattr(self, 'item', replace(self.item, reviewers=('new-a', 'new-b')))
        result = self.action(sid, state, 'reference_confirm', 'confirm', payload)
        self.assertEqual(result['reference']['status'], 'pending')
        self.assertEqual(result['reference']['reason'], 'reference_material_unavailable')
        self.assertEqual(result['status'], 'WaitingStudent')
        self.assert_no_private_publication(sid)

    def test_invalid_payload_scope_and_budget_reject_before_receipt_io(self):
        sid, _, state, payload = self.request()
        for bad in ({}, payload | dict(content=self.content), payload | dict(receipt_id='../private'),
                    payload | dict(challenge_id='bad')):
            with self.subTest(payload=bad), self.assertRaisesRegex(ValueError, 'invalid_reference'):
                self.runtime.action(sid, 'learner', 'reference_confirm', 'bad',
                    int(state['state_version']), reference=bad)
        with self.assertRaisesRegex(ValueError, 'scope_denied'):
            self.runtime.action(sid, 'other', 'reference_confirm', 'wrong',
                int(state['state_version']), reference=payload)
        session = self.runtime._live[sid][0]
        session._loop.max_steps = session.state.steps
        with self.assertRaisesRegex(ValueError, 'reference_budget_exceeded'):
            self.runtime.action(sid, 'learner', 'reference_confirm', 'budget',
                int(state['state_version']), reference=payload)
        self.assertEqual((self.port.lookups, self.port.executions), (0, 0))

    def test_config_requires_paired_trusted_ports_and_default_stays_disabled(self):
        factory = self.runtime.factory
        for options in (dict(reference_provider=lambda b: self.item), dict(receipt_port=self.port),
                        dict(reference_provider=lambda b: self.item, receipt_port=object())):
            with self.subTest(options=tuple(options)), self.assertRaisesRegex(ValueError, 'invalid_reference_config'):
                Runtime(self.store, 'inst', factory, **options)
        self.close()
        self.open(configured=False)
        sid, state = self.create()
        for kind in ('reference_request', 'reference_confirm', 'reference_reconcile'):
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, 'receipt_unavailable'):
                self.runtime.action(sid, 'learner', kind, kind, int(state['state_version']), reference={})
        self.assertEqual(self.runtime.snapshot(sid, 'learner')['reference'], dict(status='disabled'))

    def test_pending_operation_cannot_be_replaced_by_new_request_or_receipt(self):
        sid, _, state, payload = self.request()
        self.port.lose_response = True
        pending = self.action(sid, state, 'reference_confirm', 'confirm', payload)
        with self.assertRaisesRegex(ValueError, 'reference_pending'):
            self.runtime.action(sid, 'learner', 'reference_request', 'replace',
                int(pending['state_version']), reference={})
        with self.assertRaisesRegex(ValueError, 'reference_challenge_stale'):
            self.runtime.action(sid, 'learner', 'reference_confirm', 'replace-receipt',
                int(pending['state_version']), reference=payload | dict(receipt_id='ui-receipt-2'))
        self.assertEqual(len(self.port.calls), 1)

    def test_cancel_while_quarantined_invalidates_unknown_operation_without_recovery(self):
        from sigflow_edu_agent.host.reference_journal import ReferenceJournal
        from sigflow_edu_agent.runtime.gateway_session import SessionBinding
        sid, _, state, payload = self.request()
        self.port.lose_response = True
        self.action(sid, state, 'reference_confirm', 'confirm', payload)
        self.close()
        self.open()
        self.runtime.cancel(sid, 'learner')
        row = self.store.get(sid)
        record = ReferenceJournal(self.store).get(SessionBinding(**row['binding']), row['state']['run_id'])
        self.assertEqual((record['phase'], record['receipt_id']), ('invalid', 'ui-receipt-1'))
        self.assertEqual(self.runtime.snapshot(sid, 'learner')['reference'], dict(status='cancelled'))
        self.assertEqual(len(self.port.calls), 1)

    def test_released_metadata_recovery_does_not_consume_again(self):
        sid, _, state, payload = self.request()
        self.action(sid, state, 'reference_confirm', 'confirm', payload)
        calls = list(self.port.calls)
        self.close()
        self.open()
        quarantined = self.runtime.snapshot(sid, 'learner')
        self.runtime.recover(sid, 'learner', 'recover', int(quarantined['state_version']), self.body['question'])
        recovered = self.wait(sid, 'recover')
        result = self.action(sid, recovered, 'reference_reconcile', 'reconcile')
        self.assertEqual(result['reference']['status'], 'confirmed')
        self.assertEqual(self.port.calls, calls)

    def test_expired_challenge_is_hidden_from_snapshot_and_action_replay(self):
        from unittest.mock import patch
        sid, before, state, _ = self.request()
        deadline = int(state['reference']['challenge']['expires_at_epoch'])
        with patch('sigflow_edu_agent.host.reference_actions.time.time', return_value=deadline):
            self.assertEqual(self.runtime.snapshot(sid, 'learner')['reference'], dict(status='expired'))
            replay = self.runtime.action(sid, 'learner', 'reference_request', 'request',
                int(before['state_version']), reference={})
            self.assertEqual(replay['result']['reference'], dict(status='expired'))
        self.assertEqual(self.port.lookups, 0)

    def test_material_unavailable_is_sanitized_and_teaching_can_finish(self):
        sid, state = self.create()
        self.item = None
        result = self.action(sid, state, 'reference_request', 'missing')
        self.assertEqual(result['reference'], dict(status='unavailable', reason='reference_material_unavailable'))
        self.assertEqual(result['status'], 'WaitingStudent')
        self.runtime.action(sid, 'learner', 'finish', 'finish', int(result['state_version']))
        self.assertEqual(self.wait(sid, 'finish')['status'], 'Completed')

    def test_http_reference_stays_denied_even_with_internal_configuration(self):
        import http.client
        import threading
        from sigflow_edu_agent.host.http import HostServer
        sid, state = self.create()
        server = HostServer(self.runtime, 'test-ui')
        thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': .01}, daemon=True)
        thread.start()
        def request(method, path, body=None):
            conn = http.client.HTTPConnection('127.0.0.1', server.server_port, timeout=3)
            conn.request(method, path, None if body is None else json.dumps(body),
                {'Authorization': 'Bearer test-ui', 'X-Learner-ID': 'learner', 'Content-Type': 'application/json'})
            response = conn.getresponse()
            result = response.status, json.loads(response.read())
            conn.close()
            return result
        try:
            caps = request('GET', '/api/v1/capabilities')[1]['data']
            self.assertFalse(next(m for m in caps['modes'] if m['id'] == 'L4')['available'])
            for kind in ('reference_request', 'reference_confirm', 'reference_reconcile'):
                status, data = request('POST', '/api/v1/teaching/sessions/' + sid + '/actions',
                    dict(kind=kind, action_id='http-' + kind, expected_version=state['state_version']))
                self.assertEqual((status, data['error']['code']), (403, 'receipt_unavailable'))
            self.assertEqual(self.port.lookups, 0)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(2)
