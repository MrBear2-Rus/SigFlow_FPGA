"""Reference metadata boundaries against a real caller-owned SQLite store."""
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict, replace
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from sigflow_edu_agent.host.store import DurableStore
from sigflow_edu_agent.runtime.gateway_session import SessionBinding


class ReferenceJournalTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'host.sqlite'
        self.binding = SessionBinding('inst', 's1', 'learner', 'p1', 'r1', 'snap',
                                      'job', 'diag', 'issue')
        self.store = DurableStore(self.path, 'inst')
        self.addCleanup(lambda: self.store.close())
        self.store.create('s1', asdict(self.binding), hashlib.sha256(b'synthetic').hexdigest(),
                          'rule', 'diagnose', self.state(3))
        # An absent implementation is a behavioral assertion during the first RED.
        try:
            from sigflow_edu_agent.host.reference_journal import ReferenceJournal
        except ModuleNotFoundError:
            self.fail('ReferenceJournal durable metadata implementation is missing')
        self.journal = ReferenceJournal(self.store)
        self.challenge = dict(challenge_id='a' * 32, action_id='reference-' + 'b' * 32,
            session_id='s1', project_id='p1', revision='r1', learner_id='learner',
            issue_id='issue', run_id='run1', reference_id='synthetic-ref',
            reference_sha256='c' * 64, review_sha256='d' * 64, level='l4',
            policy='u2-reviewed-reference.v1', state_version='3', expires_at_epoch='2000000000')

    @staticmethod
    def state(version, status='WaitingStudent'):
        return dict(state_version=str(version), status=status, level=3, run_id='run1', cards=[])

    def code(self, expected, action):
        with self.assertRaisesRegex(ValueError, '^' + expected + '$'):
            action()

    def prepare(self):
        return self.journal.prepare(self.binding, 'run1', self.challenge)

    def claim(self, receipt='ui-receipt-1', **kwargs):
        return self.journal.claim(self.binding, 'run1', self.challenge['challenge_id'], receipt, **kwargs)

    def release(self, **kwargs):
        return self.journal.release(self.binding, 'run1', self.challenge['challenge_id'], **kwargs)

    def publish(self, state):
        old = int(self.store.get('s1')['state']['state_version'])
        action = 'trusted-' + str(old)
        self.store.claim('s1', action, 'synthetic-fingerprint', old)
        self.store.commit('s1', action, state, None, {})

    def test_prepare_returns_independent_metadata_without_mutating_session(self):
        self.assertIsNone(self.journal.get(self.binding, 'run1'))
        record = self.prepare()
        self.assertEqual(record, {'challenge': self.challenge, 'phase': 'ready', 'receipt_id': None})
        self.challenge['reference_id'] = 'caller-change'
        record['challenge']['reference_id'] = 'return-change'
        self.assertEqual(self.journal.get(self.binding, 'run1')['challenge']['reference_id'], 'synthetic-ref')
        self.assertEqual(self.store.get('s1')['state']['state_version'], '3')
        self.assertEqual(self.store.events('s1', 0)['events'], [])

    def test_claim_release_and_receipt_conflicts_are_atomic(self):
        self.prepare()
        self.code('reference_challenge_stale', self.release)
        self.assertEqual(self.claim()['phase'], 'consuming')
        self.assertEqual(self.claim()['phase'], 'consuming')
        self.code('reference_pending', self.prepare)
        self.code('reference_receipt_conflict', lambda: self.claim('ui-receipt-2'))
        self.assertEqual(self.release()['phase'], 'released')
        self.assertEqual(self.release()['phase'], 'released')
        self.assertEqual(self.claim()['phase'], 'released')
        self.code('reference_receipt_conflict', lambda: self.claim('ui-receipt-2'))
        self.assertEqual(self.prepare()['receipt_id'], None)

    def test_full_binding_and_run_scope_reject_every_cross_scope_operation(self):
        self.prepare()
        for field in ('instance_id', 'session_id', 'learner_id', 'project_id', 'revision',
                      'snapshot_id', 'job_id', 'diagnostic_id', 'issue_id'):
            wrong = replace(self.binding, **{field: 'other'})
            for method, args in ((self.journal.get, ()), (self.journal.prepare, (self.challenge,)),
                                 (self.journal.claim, ('a' * 32, 'ui-receipt-1')),
                                 (self.journal.release, ('a' * 32,)), (self.journal.invalidate, ())):
                self.code('reference_scope_mismatch', lambda m=method, a=args: m(wrong, 'run1', *a))
        self.code('reference_scope_mismatch', lambda: self.journal.get(self.binding, 'other-run'))
        self.code('reference_scope_mismatch', lambda: self.journal.invalidate(self.binding, 'other-run'))

    def test_raw_challenge_whitelist_and_canonical_fields_reject_private_text(self):
        private = 'PRIVATE SYNTHETIC BODY MUST NEVER BE WRITTEN'
        for extra in ('content', 'source_code', 'question', 'token', 'unknown'):
            self.code('reference_invalid_record', lambda: self.journal.prepare(
                self.binding, 'run1', self.challenge | {extra: private}))
        bad_values = {'challenge_id': 'A' * 32, 'action_id': 'reference-bad',
            'reference_id': '../private', 'reference_sha256': 'z' * 64,
            'review_sha256': 'c' * 63, 'level': 'l3', 'policy': 'other',
            'state_version': '03', 'expires_at_epoch': '0'}
        for field, value in bad_values.items():
            self.code('reference_invalid_record', lambda f=field, v=value: self.journal.prepare(
                self.binding, 'run1', self.challenge | {f: v}))
        for value in ('18446744073709551616', '01', True, 2000000000):
            self.code('reference_invalid_record', lambda v=value: self.journal.prepare(
                self.binding, 'run1', self.challenge | {'expires_at_epoch': v}))
        self.code('reference_invalid_record', lambda: self.journal.prepare(
            self.binding, 'run1', self.challenge | {'state_version': str(2**63)}))
        self.assertIsNone(self.journal.get(self.binding, 'run1'))
        self.assertNotIn(private.encode(), self.path.read_bytes())

    def test_mismatched_challenge_scope_and_version_never_replace_ready(self):
        self.prepare()
        for field in ('session_id', 'project_id', 'revision', 'learner_id', 'issue_id', 'run_id'):
            self.code('reference_scope_mismatch', lambda f=field: self.journal.prepare(
                self.binding, 'run1', self.challenge | {f: 'other'}))
        self.code('reference_challenge_stale', lambda: self.journal.prepare(
            self.binding, 'run1', self.challenge | {'state_version': '2'}))
        self.code('reference_challenge_stale', lambda: self.journal.claim(
            self.binding, 'run1', 'e' * 32, 'ui-receipt-1'))
        self.assertEqual(self.journal.get(self.binding, 'run1')['phase'], 'ready')

    def test_cas_catches_state_changes_before_claim_and_release(self):
        self.prepare()
        self.code('reference_context_changed', lambda: self.claim(expected_version=2))
        self.claim(expected_version=3)
        self.publish(self.state(4))
        self.code('reference_context_changed', lambda: self.claim(expected_version=3))
        self.code('reference_context_changed', lambda: self.release(expected_version=3))
        self.assertEqual(self.journal.get(self.binding, 'run1')['phase'], 'consuming')
        self.assertEqual(self.release(expected_version=4)['phase'], 'released')

    def test_stale_ready_record_cannot_first_claim_after_state_change(self):
        self.prepare()
        self.publish(self.state(4))
        self.code('reference_challenge_stale', lambda: self.claim(expected_version=4))
        self.assertEqual(self.journal.get(self.binding, 'run1')['phase'], 'ready')

    def test_cancel_blocks_mutations_but_allows_metadata_invalidation(self):
        self.prepare()
        self.claim()
        self.store.request_cancel('s1')
        self.code('reference_context_changed', self.prepare)
        self.code('reference_context_changed', self.claim)
        self.code('reference_context_changed', self.release)
        invalid = self.journal.invalidate(self.binding, 'run1')
        self.assertEqual(invalid, {'challenge': self.challenge, 'phase': 'invalid', 'receipt_id': 'ui-receipt-1'})
        self.assertEqual(self.journal.get(self.binding, 'run1'), invalid)

    def test_terminal_and_recovery_contexts_cannot_mutate(self):
        self.prepare()
        for version, status in enumerate(('RecoveryRequired', 'Completed', 'Cancelled', 'Failed', 'BudgetExceeded'), 4):
            self.publish(self.state(version, status))
            self.code('reference_context_changed', self.prepare)
            self.code('reference_context_changed', self.claim)
            self.code('reference_context_changed', self.release)

    def test_invalidation_keeps_audit_and_can_replace_only_when_live(self):
        self.prepare()
        self.journal.invalidate(self.binding, 'run1')
        self.code('reference_challenge_stale', self.claim)
        self.code('reference_challenge_stale', self.release)
        self.assertEqual(self.prepare()['phase'], 'ready')

    def test_invalidated_receipt_cannot_be_forgotten_by_a_new_prepare(self):
        self.prepare()
        self.claim()
        invalid = self.journal.invalidate(self.binding, 'run1')
        replacement = self.challenge | {'challenge_id': 'e' * 32, 'action_id': 'reference-' + 'f' * 32}
        self.code('reference_pending', lambda: self.journal.prepare(self.binding, 'run1', replacement))
        self.assertEqual(self.journal.get(self.binding, 'run1'), invalid)

    def test_invalidated_released_receipt_also_preserves_its_audit(self):
        self.prepare()
        self.claim()
        self.release()
        invalid = self.journal.invalidate(self.binding, 'run1')
        self.code('reference_pending', self.prepare)
        self.assertEqual(self.journal.get(self.binding, 'run1'), invalid)

    def test_reopen_keeps_receipt_and_original_version_after_revalidation(self):
        self.prepare()
        self.claim()
        self.store.close()
        self.store = DurableStore(self.path, 'inst')
        from sigflow_edu_agent.host.reference_journal import ReferenceJournal
        self.journal = ReferenceJournal(self.store)
        self.assertEqual(self.store.get('s1')['state']['status'], 'RecoveryRequired')
        self.assertEqual(self.journal.get(self.binding, 'run1')['receipt_id'], 'ui-receipt-1')
        self.code('reference_context_changed', lambda: self.claim(expected_version=4))
        self.publish(self.state(5))
        record = self.claim(expected_version=5)
        self.assertEqual(record['challenge']['state_version'], '3')
        self.release(expected_version=5)
        self.store.close()
        self.store = DurableStore(self.path, 'inst')
        self.journal = ReferenceJournal(self.store)
        self.assertEqual(self.journal.get(self.binding, 'run1')['phase'], 'released')

    def test_concurrent_claim_has_one_receipt_winner(self):
        self.prepare()
        def attempt(number):
            receipt = 'ui-receipt-' + str(number)
            try:
                return self.claim(receipt, expected_version=3)['receipt_id']
            except ValueError as exc:
                return str(exc)
        with ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(attempt, range(1, 9)))
        winners = [r for r in results if r.startswith('ui-receipt-')]
        self.assertEqual(len(winners), 1)
        self.assertEqual(results.count('reference_receipt_conflict'), 7)
        self.assertEqual(self.journal.get(self.binding, 'run1')['receipt_id'], winners[0])

    def test_receipt_syntax_and_malformed_cas_never_claim(self):
        self.prepare()
        for receipt in ('ui-receipt-0', 'ui-receipt-01', 'ui-receipt-' + '1' * 21, 'raw secret', None):
            self.code('reference_invalid_record', lambda r=receipt: self.claim(r))
        for version in (True, '3', -1, 2**63):
            self.code('reference_context_changed', lambda v=version: self.claim(expected_version=v))
        self.assertEqual(self.journal.get(self.binding, 'run1')['phase'], 'ready')

    def test_sql_failure_rolls_back_and_closed_store_never_succeeds(self):
        self.prepare()
        with self.store._transaction() as db:
            db.execute("CREATE TRIGGER fail_reference BEFORE UPDATE ON reference_records BEGIN SELECT RAISE(ABORT, 'synthetic fault'); END")
        self.code('storage_error', self.claim)
        self.assertEqual(self.journal.get(self.binding, 'run1')['phase'], 'ready')
        self.store.close()
        self.code('store_closed', lambda: self.journal.get(self.binding, 'run1'))

    def test_storage_is_one_row_per_session_and_rejects_corrupt_record(self):
        self.prepare()
        for _ in range(4):
            self.journal.invalidate(self.binding, 'run1')
            self.prepare()
        with self.store._transaction() as db:
            self.assertEqual(db.execute('SELECT COUNT(*) FROM reference_records').fetchone()[0], 1)
            self.assertEqual(db.execute('PRAGMA foreign_key_list(reference_records)').fetchone()['table'], 'sessions')
            db.execute("UPDATE reference_records SET phase='consuming'")
        self.code('reference_invalid_record', lambda: self.journal.get(self.binding, 'run1'))

    def request(self, challenge=None, state=None, binding=None, run_id='run1'):
        self.assertTrue(callable(getattr(self.journal, 'commit_request', None)),
                        'atomic reference request commit is missing')
        challenge = self.challenge | {'state_version': '4'} if challenge is None else challenge
        state = (self.state(4) | {'reference': {'status': 'awaiting_confirmation',
                                              'challenge': challenge}}) if state is None else state
        return self.journal.commit_request(binding or self.binding, run_id, challenge,
                                           'request-1', state, '{"cursor":"request"}')

    def reserve_request(self):
        self.store.claim('s1', 'request-1', 'request-fingerprint', 3)

    def assert_request_rolled_back(self, original=None):
        self.assertEqual(self.store.get('s1')['state'], self.state(3))
        self.assertIsNone(self.store.get('s1')['checkpoint'])
        self.assertEqual(self.store.action('s1', 'request-1'), {'status': 'pending', 'result': None})
        self.assertEqual(self.store.events('s1', 0), {'events': [], 'high_watermark': 0})
        self.assertEqual(self.journal.get(self.binding, 'run1'), original)

    def test_request_commits_state_action_checkpoint_event_and_next_version_challenge(self):
        self.reserve_request()
        record = self.request()
        self.assertEqual(record, {'challenge': self.challenge | {'state_version': '4'},
                                  'phase': 'ready', 'receipt_id': None})
        saved = self.store.get('s1')
        self.assertEqual(saved['state']['state_version'], '4')
        self.assertEqual(saved['state']['reference']['challenge'], record['challenge'])
        self.assertEqual(saved['checkpoint'], '{"cursor":"request"}')
        self.assertEqual(self.store.action('s1', 'request-1'),
                         {'status': 'committed', 'result': saved['state']})
        self.assertEqual(self.store.events('s1', 0), {'events': [
            {'sequence': 1, 'action_id': 'request-1', 'state_version': '4',
             'status': 'WaitingStudent'}], 'high_watermark': 1})
        record['challenge']['reference_id'] = 'changed-return'
        self.assertEqual(self.journal.get(self.binding, 'run1')['challenge']['reference_id'],
                         'synthetic-ref')

    def test_request_prepare_failure_rolls_back_every_host_write_and_preserves_ready(self):
        original = self.prepare()
        self.reserve_request()
        self.code('reference_challenge_stale', lambda: self.request(self.challenge))
        self.assert_request_rolled_back(original)
        for field in ('session_id', 'project_id', 'revision', 'learner_id', 'issue_id', 'run_id'):
            with self.subTest(field=field):
                self.code('reference_scope_mismatch', lambda f=field: self.request(
                    self.challenge | {'state_version': '4', f: 'other'}))
                self.assert_request_rolled_back(original)

    def test_request_cannot_overwrite_consuming_or_invalid_receipt_audit(self):
        self.prepare()
        original = self.claim()
        self.reserve_request()
        self.code('reference_pending', self.request)
        self.assert_request_rolled_back(original)
        original = self.journal.invalidate(self.binding, 'run1')
        self.code('reference_pending', self.request)
        self.assert_request_rolled_back(original)

    def test_request_cancel_rejects_without_publishing(self):
        self.reserve_request()
        self.store.request_cancel('s1')
        self.code('reference_context_changed', self.request)
        self.assert_request_rolled_back()

    def test_request_action_cas_failure_keeps_existing_challenge(self):
        original = self.prepare()
        self.reserve_request()
        # Simulate another trusted state advance after this action was reserved.
        with self.store._transaction() as db:
            db.execute("UPDATE actions SET expected_version=2 WHERE session_id='s1'")
        self.code('state_conflict', self.request)
        self.assert_request_rolled_back(original)

    def test_request_rejects_mismatched_binding_run_and_nonlive_original_context(self):
        self.reserve_request()
        for field in ('instance_id', 'session_id', 'learner_id', 'project_id', 'revision',
                      'snapshot_id', 'job_id', 'diagnostic_id', 'issue_id'):
            with self.subTest(field=field):
                self.code('reference_scope_mismatch', lambda f=field: self.request(
                    binding=replace(self.binding, **{f: 'other'})))
                self.assert_request_rolled_back()
        self.code('reference_scope_mismatch', lambda: self.request(run_id='other-run'))
        self.assert_request_rolled_back()
        with self.store._transaction() as db:
            db.execute("UPDATE sessions SET state=? WHERE session_id='s1'",
                       (json.dumps(self.state(3, 'RecoveryRequired')),))
        self.code('reference_context_changed', self.request)
        self.assertEqual(self.store.action('s1', 'request-1')['status'], 'pending')
        self.assertEqual(self.store.events('s1', 0)['events'], [])
        self.assertIsNone(self.journal.get(self.binding, 'run1'))

    def test_request_rejects_unmatched_or_private_reference_before_storage(self):
        self.reserve_request()
        private = 'PRIVATE REQUEST BODY MUST NEVER REACH SQLITE'
        challenge = self.challenge | {'state_version': '4'}
        reference = {'status': 'awaiting_confirmation', 'challenge': challenge}
        for metadata in (None, {}, reference | {'content': private},
                         reference | {'receipt_id': 'ui-receipt-1'},
                         reference | {'status': 'released'},
                         reference | {'challenge': self.challenge}):
            with self.subTest(metadata=metadata):
                self.code('reference_invalid_record', lambda m=metadata: self.request(
                    state=self.state(4) | {'reference': m}))
                self.assert_request_rolled_back()
        self.code('reference_invalid_record', lambda: self.request(challenge | {'content': private}))
        self.assert_request_rolled_back()
        self.assertNotIn(private.encode(), self.path.read_bytes())

    def test_request_reference_sql_fault_rolls_back_host_commit(self):
        self.reserve_request()
        with self.store._transaction() as db:
            db.execute("CREATE TRIGGER fail_request BEFORE INSERT ON reference_records "
                       "BEGIN SELECT RAISE(ABORT, 'synthetic fault'); END")
        self.code('storage_error', self.request)
        self.assert_request_rolled_back()


if __name__ == '__main__':
    unittest.main()
