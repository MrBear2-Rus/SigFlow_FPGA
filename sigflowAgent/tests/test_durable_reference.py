"""Crash boundaries against real SQLite; Core receipts and material are fixtures."""
from dataclasses import asdict, replace
import hashlib
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

from sigflow_edu_agent.clients.gateway import GatewayError
from sigflow_edu_agent.host.reference import ReviewedReference
from sigflow_edu_agent.host.store import DurableStore, StoreError
from sigflow_edu_agent.runtime.gateway_session import SessionBinding


class ReceiptPort:
    def __init__(self, challenge):
        self.challenge = challenge
        self.value = {key: challenge[key] for key in (
            'session_id', 'project_id', 'revision', 'challenge_id', 'action_id',
            'policy', 'state_version', 'level')}
        self.value.update(receipt_id='ui-receipt-1', status='active')
        self.calls = []
        self.executions = 0
        self.lose_response = False
        self.after_consume = lambda: None

    def lookup(self, receipt_id):
        return dict(self.value)

    def consume(self, receipt_id, project, run, action, version):
        args = (receipt_id, project, run, action, version)
        expected = ('ui-receipt-1', 'p1', 'run1', self.challenge['action_id'],
                    int(self.challenge['state_version']))
        if args != expected:
            raise GatewayError('POLICY_DENIED')
        self.calls.append(args)
        replay = self.value['status'] == 'consumed'
        if not replay:
            self.value['status'] = 'consumed'
            self.executions += 1
        self.after_consume()
        if self.lose_response:
            self.lose_response = False
            raise GatewayError('TIMEOUT')
        return dict(receipt_id=receipt_id, action_id=action, status='consumed',
                    consumed=True, replayed=replay)


class DurableReferenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'host.sqlite'
        self.binding = SessionBinding('inst', 's1', 'learner', 'p1', 'r1', 'snap',
                                      'job', 'diag', 'issue')
        self.store = DurableStore(self.path, 'inst')
        self.addCleanup(lambda: self.store.close())
        self.store.create('s1', asdict(self.binding), hashlib.sha256(b'question').hexdigest(),
                          'rule', 'diagnose', self.state(3))
        self.content = 'Synthetic reference text must never enter the journal.'
        self.item = ReviewedReference('ref1', self.content, hashlib.sha256(self.content.encode()).hexdigest(),
                                      ('fixture-reviewer1', 'fixture-reviewer2'))
        self.open_gate()

    @staticmethod
    def state(version, status='WaitingStudent'):
        return dict(state_version=str(version), status=status, level=3, run_id='run1',
                    stage='E5', cards=[])

    def open_gate(self):
        from sigflow_edu_agent.host.reference_journal import ReferenceJournal
        from sigflow_edu_agent.host.durable_reference import DurableReferenceGate
        self.journal = ReferenceJournal(self.store)
        self.gate = DurableReferenceGate(self.binding, 'run1', lambda binding: self.item,
                                         self.journal)

    def request(self):
        challenge = self.gate.request(3)
        self.assertNotIn('content', challenge)
        return challenge, ReceiptPort(challenge)

    def reopen_and_revalidate(self):
        self.store.close()
        self.store = DurableStore(self.path, 'inst')
        row = self.store.get('s1')
        self.assertEqual(row['state']['status'], 'RecoveryRequired')
        old = int(row['state']['state_version'])
        self.store.claim('s1', 'trusted-revalidation', 'revalidated', old)
        self.store.commit('s1', 'trusted-revalidation', self.state(old + 1), None, {})
        self.open_gate()
        return old + 1

    def test_prepare_then_publish_then_replay_never_consumes_twice(self):
        challenge, port = self.request()
        self.assertEqual(self.journal.get(self.binding, 'run1')['phase'], 'ready')
        result = self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        self.assertEqual(result['content'], self.content)
        self.assertEqual(self.journal.get(self.binding, 'run1')['phase'], 'released')
        self.assertEqual(self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port), result)
        self.assertEqual((port.executions, len(port.calls)), (1, 1))
        self.assertNotIn(self.content.encode(), self.path.read_bytes())

    def test_draft_targets_next_publication_version_without_journal_write(self):
        self.assertTrue(hasattr(self.gate, 'draft'), 'atomic publication draft is missing')
        challenge = self.gate.draft(3, 4)
        self.assertEqual(challenge['state_version'], '4')
        self.assertIsNone(self.journal.get(self.binding, 'run1'))
        for current, target in ((2, 3), (3, 3), (3, 5), (3, True)):
            with self.subTest(current=current, target=target), self.assertRaises(ValueError):
                self.gate.draft(current, target)

    def test_metadata_mode_never_returns_reference_body(self):
        from sigflow_edu_agent.host.durable_reference import DurableReferenceGate
        import inspect
        self.assertIn('metadata_only', inspect.signature(DurableReferenceGate).parameters,
                      'host metadata-only gate mode is missing')
        gate = DurableReferenceGate(self.binding, 'run1', lambda binding: self.item,
                                    self.journal, metadata_only=True)
        challenge = gate.request(3)
        result = gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, ReceiptPort(challenge))
        self.assertEqual(set(result), {'reference_id', 'sha256', 'policy', 'reviewers'})
        self.assertNotIn(self.content, str(result))

    def test_lost_core_response_retains_original_receipt_and_requires_explicit_reconcile(self):
        challenge, port = self.request()
        port.lose_response = True
        with self.assertRaises(GatewayError):
            self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        self.assertEqual(self.journal.get(self.binding, 'run1')['phase'], 'consuming')
        with self.assertRaisesRegex(ValueError, 'reference_pending'):
            self.gate.request(3)
        result = self.gate.reconcile(3, port)
        self.assertEqual(result['content'], self.content)
        self.assertEqual(port.executions, 1)
        self.assertEqual(port.calls[0], port.calls[1])

    def test_reopen_quarantines_before_same_receipt_can_be_reconciled(self):
        challenge, port = self.request()
        port.lose_response = True
        with self.assertRaises(GatewayError):
            self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        version = self.reopen_and_revalidate()
        result = self.gate.reconcile(version, port)
        self.assertEqual(result['content'], self.content)
        self.assertEqual(port.calls[-1][-1], 3)
        self.assertEqual(port.executions, 1)

    def test_local_release_failure_after_core_success_is_recoverable(self):
        challenge, port = self.request()
        with patch.object(self.journal, 'release', side_effect=StoreError('storage_error')):
            with self.assertRaises(StoreError):
                self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        self.assertEqual(self.journal.get(self.binding, 'run1')['phase'], 'consuming')
        self.assertEqual(self.gate.reconcile(3, port)['sha256'], self.item.sha256)
        self.assertEqual(port.executions, 1)

    def test_response_lost_after_release_replays_without_core_call_after_restart(self):
        challenge, port = self.request()
        self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        version = self.reopen_and_revalidate()
        self.assertEqual(self.gate.reconcile(version, port)['content'], self.content)
        self.assertEqual(len(port.calls), 1)

    def test_expired_active_receipt_is_not_consumed_but_committed_replay_survives_ttl(self):
        challenge, port = self.request()
        with patch('sigflow_edu_agent.host.durable_reference.time.time',
                   return_value=int(challenge['expires_at_epoch']) + 1):
            with self.assertRaises(ValueError):
                self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        self.assertEqual(port.executions, 0)
        port.lose_response = True
        with self.assertRaises(GatewayError):
            self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        with patch('sigflow_edu_agent.host.durable_reference.time.time',
                   return_value=int(challenge['expires_at_epoch']) + 1):
            self.assertEqual(self.gate.reconcile(3, port)['content'], self.content)
        self.assertEqual(port.executions, 1)

    def test_cancel_during_external_consume_never_publishes_reference(self):
        challenge, port = self.request()
        port.after_consume = lambda: self.store.cancel_session('s1')
        with self.assertRaises(ValueError):
            self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        self.assertEqual(port.executions, 1)
        with self.assertRaises(ValueError):
            self.gate.reconcile(int(self.store.get('s1')['state']['state_version']), port)
        self.assertNotEqual(self.journal.get(self.binding, 'run1')['phase'], 'released')

    def test_changed_material_or_review_context_prevents_release(self):
        challenge, port = self.request()
        self.item = replace(self.item, reviewers=('new-reviewer1', 'new-reviewer2'))
        with self.assertRaisesRegex(ValueError, 'reference_material_unavailable'):
            self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        self.assertEqual(port.executions, 0)

    def test_material_changed_during_core_call_is_not_published(self):
        challenge, port = self.request()
        port.after_consume = lambda: setattr(self, 'item', None)
        with self.assertRaisesRegex(ValueError, 'reference_material_unavailable'):
            self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        self.assertEqual(self.journal.get(self.binding, 'run1')['phase'], 'consuming')

    def test_wrong_learner_run_and_receipt_cannot_resume(self):
        from sigflow_edu_agent.host.durable_reference import DurableReferenceGate
        challenge, port = self.request()
        for binding, run in ((replace(self.binding, learner_id='other'), 'run1'),
                             (self.binding, 'other-run')):
            gate = DurableReferenceGate(binding, run, lambda _: self.item, self.journal)
            with self.assertRaises(ValueError):
                gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        with self.assertRaises(ValueError):
            self.gate.confirm('ui-receipt-2', challenge['challenge_id'], 3, port)
        self.assertEqual(port.executions, 0)

    def test_no_material_stale_version_and_ready_reconcile_have_no_external_side_effect(self):
        with self.assertRaises(ValueError):
            self.gate.request(2)
        challenge, port = self.request()
        with self.assertRaisesRegex(ValueError, 'reference_not_pending'):
            self.gate.reconcile(3, port)
        self.assertEqual(port.executions, 0)
        self.item = None
        with self.assertRaisesRegex(ValueError, 'reference_material_unavailable'):
            self.gate.request(3)

    def test_pending_unconsumed_receipt_cannot_be_consumed_after_local_expiry(self):
        challenge, port = self.request()
        with patch.object(port, 'consume', side_effect=GatewayError('SERVICE_UNAVAILABLE')):
            with self.assertRaises(GatewayError):
                self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        with patch('sigflow_edu_agent.host.durable_reference.time.time',
                   return_value=int(challenge['expires_at_epoch']) + 1):
            with self.assertRaises(ValueError):
                self.gate.reconcile(3, port)
        self.assertEqual(port.executions, 0)

    def test_pending_unconsumed_receipt_can_use_original_approval_within_ttl(self):
        challenge, port = self.request()
        with patch.object(port, 'consume', side_effect=GatewayError('SERVICE_UNAVAILABLE')):
            with self.assertRaises(GatewayError):
                self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        result = self.gate.reconcile(3, port)
        self.assertEqual(result['content'], self.content)
        self.assertEqual(port.executions, 1)

    def test_invalid_acknowledgement_preserves_pending_until_proven_replay(self):
        challenge, port = self.request()
        actual = port.consume
        def bad_ack(*args):
            return actual(*args) | dict(replayed='false')
        with patch.object(port, 'consume', side_effect=bad_ack):
            with self.assertRaises(ValueError):
                self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        self.assertEqual(self.journal.get(self.binding, 'run1')['phase'], 'consuming')
        self.assertEqual(self.gate.reconcile(3, port)['sha256'], self.item.sha256)
        self.assertEqual(port.executions, 1)

    def test_released_result_still_requires_current_reviewed_material(self):
        challenge, port = self.request()
        self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        self.item = None
        with self.assertRaisesRegex(ValueError, 'reference_material_unavailable'):
            self.gate.reconcile(3, port)
        self.assertEqual(port.executions, 1)

    def test_explicit_invalidation_retains_audit_but_denies_publication(self):
        challenge, port = self.request()
        port.lose_response = True
        with self.assertRaises(GatewayError):
            self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        self.gate.invalidate()
        record = self.journal.get(self.binding, 'run1')
        self.assertEqual((record['phase'], record['receipt_id']), ('invalid', 'ui-receipt-1'))
        with self.assertRaisesRegex(ValueError, 'reference_pending'):
            self.gate.request(3)
        self.assertEqual(self.journal.get(self.binding, 'run1'), record)
        with self.assertRaises(ValueError):
            self.gate.reconcile(3, port)
        self.assertEqual(port.executions, 1)

    def test_active_approval_expiring_while_reservation_blocks_never_consumes(self):
        challenge, port = self.request()
        clock = {'now': time.time()}
        actual_claim = self.journal.claim
        def claim_then_expire(*args, **kwargs):
            record = actual_claim(*args, **kwargs)
            clock['now'] = int(challenge['expires_at_epoch']) + 1
            return record
        with patch('sigflow_edu_agent.host.durable_reference.time.time',
                   side_effect=lambda: clock['now']), patch.object(self.journal, 'claim',
                   side_effect=claim_then_expire):
            with self.assertRaisesRegex(ValueError, 'reference_challenge_stale'):
                self.gate.confirm('ui-receipt-1', challenge['challenge_id'], 3, port)
        self.assertEqual(port.executions, 0)
        self.assertEqual(self.journal.get(self.binding, 'run1')['phase'], 'consuming')
