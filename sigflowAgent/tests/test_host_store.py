import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
import unittest
from unittest.mock import patch

try:
    from sigflow_edu_agent.host.store import DurableStore, StoreError
except ModuleNotFoundError:
    DurableStore = StoreError = None


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(DurableStore, 'D1 durable store is missing')
        self.temp = tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1])
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'host.sqlite3'
        self.store = DurableStore(self.path, 'inst')
        self.addCleanup(lambda: self.store.close())

    def state(self, version=0, status='Running', cards=None):
        return dict(state_version=str(version), status=status, stage='E0', cards=cards or [])

    def create(self, session='session', state=None):
        binding = dict(instance_id='inst', session_id=session, learner_id='learner',
                       project_id='project', revision='rev', snapshot_id='snap',
                       job_id='job', diagnostic_id='diag', issue_id='issue',
                       policy='l1-l3-l4-denied')
        self.store.create(session, binding, hashlib.sha256(b'private question').hexdigest(),
                          'rule', 'diagnose', state or self.state())

    def reopen(self):
        self.store.close()
        self.store = DurableStore(self.path, 'inst')

    def assertCode(self, code, fn):
        with self.assertRaises(StoreError) as caught:
            fn()
        self.assertEqual(str(caught.exception), code)

    def test_create_roundtrip_never_persists_original_request(self):
        self.create()
        row = self.store.get('session')
        self.assertEqual(row['state']['state_version'], '0')
        self.assertEqual(row['binding']['learner_id'], 'learner')
        self.assertEqual(row['model_used'], 0)
        self.assertEqual(row['event_high_watermark'], 0)
        self.assertIs(type(row['event_high_watermark']), int)
        self.assertIsNone(row['checkpoint'])
        self.assertEqual([x['session_id'] for x in self.store.list_sessions()], ['session'])
        self.assertNotIn(b'private question', self.path.read_bytes())
        row['state']['cards'].append({'hint': 'changed'})
        self.assertEqual(self.store.get('session')['state']['cards'], [])
        self.assertCode('session_exists', self.create)

    def test_claim_replay_conflict_cas_and_busy(self):
        self.create()
        self.assertIsNone(self.store.claim('session', 'a', 'fingerprint', 0))
        self.assertEqual(self.store.claim('session', 'a', 'fingerprint', 123),
                         {'status': 'pending', 'result': None})
        self.assertCode('action_conflict', lambda: self.store.claim('session', 'a', 'other', 0))
        self.assertCode('session_busy', lambda: self.store.claim('session', 'b', 'second', 0))
        result = {'state': self.state(1, 'WaitingStudent')}
        self.store.commit('session', 'a', self.state(1, 'WaitingStudent'), 'checkpoint', result)
        self.assertEqual(self.store.action('session', 'a'), {'status': 'committed', 'result': result})
        self.assertEqual(self.store.claim('session', 'a', 'fingerprint', 0),
                         {'status': 'committed', 'result': result})
        self.assertCode('state_conflict', lambda: self.store.claim('session', 'b', 'second', 0))
        self.assertIsNone(self.store.claim('session', 'b', 'second', 1))

    def test_commit_requires_reserved_action_and_progress(self):
        self.create()
        self.assertCode('action_not_found', lambda: self.store.commit('session', 'a', self.state(1), None, {}))
        self.store.claim('session', 'a', 'fp', 0)
        self.assertCode('state_conflict', lambda: self.store.commit('session', 'a', self.state(), None, {}))
        self.assertEqual(self.store.action('session', 'a')['status'], 'pending')

    def test_restart_interrupts_pending_and_quarantines_cards_preserving_checkpoint(self):
        self.create()
        self.store.claim('session', 'a', 'fp', 0)
        old = self.state(5, 'WaitingStudent', [{'hint': 'old lesson', 'question': 'card question'}])
        result = {'state': old}
        self.store.commit('session', 'a', old, 'durable-original', result)
        self.store.claim('session', 'b', 'second', 5)
        self.reopen()
        row = self.store.get('session')
        self.assertEqual(row['state']['status'], 'RecoveryRequired')
        self.assertEqual(row['state']['state_version'], '6')
        self.assertEqual(row['state']['cards'], [])
        self.assertEqual(row['checkpoint'], 'durable-original')
        self.assertEqual(row['event_high_watermark'], 2)
        self.assertEqual(self.store.events('session', 1), {
            'events': [{'sequence': 2, 'action_id': None, 'state_version': '6',
                        'status': 'RecoveryRequired'}], 'high_watermark': 2})
        self.assertEqual(self.store.action('session', 'b'), {'status': 'interrupted', 'result': None})
        self.assertEqual(self.store.action('session', 'a')['result'], result)
        self.assertEqual(self.store.get('session')['state']['state_version'], '6')
        self.reopen()
        self.assertEqual(self.store.get('session')['state']['state_version'], '6')
        self.assertEqual(self.store.get('session')['event_high_watermark'], 2)
        self.assertEqual(self.store.events('session', 2), {'events': [], 'high_watermark': 2})

    def test_terminal_sessions_do_not_revive(self):
        for i, status in enumerate(('Completed', 'Cancelled', 'Failed', 'BudgetExceeded')):
            self.create(str(i), self.state(7, status))
        self.reopen()
        self.assertEqual([r['state']['status'] for r in self.store.list_sessions()],
                         ['Completed', 'Cancelled', 'Failed', 'BudgetExceeded'])
        self.assertEqual(self.store.get('0')['state']['state_version'], '7')

    def test_checkpoint_corruption_is_detected_before_return(self):
        self.create()
        self.store.claim('session', 'a', 'fp', 0)
        self.store.commit('session', 'a', self.state(1), 'original', {})
        with closing(sqlite3.connect(self.path)) as db:
            db.execute("UPDATE sessions SET checkpoint = 'tampered'")
            db.commit()
        self.assertCode('checkpoint_corrupt', lambda: self.store.get('session'))
        self.assertCode('checkpoint_corrupt', self.store.list_sessions)

    def test_budget_reservation_is_atomic_and_persistent(self):
        self.create()
        def reserve(_):
            try:
                return self.store.reserve_model('session', 3)
            except StoreError as exc:
                return str(exc)
        with ThreadPoolExecutor(max_workers=12) as pool:
            counts = list(pool.map(reserve, range(12)))
        self.assertEqual(sorted(x for x in counts if type(x) is int), [1, 2, 3])
        self.assertEqual(counts.count('budget_exceeded'), 9)
        self.reopen()
        self.assertEqual(self.store.model_used('session'), 3)
        self.assertCode('budget_exceeded', lambda: self.store.reserve_model('session', 3))

    def test_cancel_flag_survives_restart_without_replaying_action(self):
        self.create()
        self.store.claim('session', 'a', 'fp', 0)
        self.store.request_cancel('session')
        self.assertTrue(self.store.cancel_requested('session'))
        self.reopen()
        self.assertTrue(self.store.get('session')['cancel_requested'])
        self.assertEqual(self.store.action('session', 'a')['status'], 'interrupted')

    def test_same_process_and_subprocess_lock_are_exclusive_and_reusable(self):
        self.assertCode('store_locked', lambda: DurableStore(self.path, 'inst'))
        script = "from sigflow_edu_agent.host.store import DurableStore, StoreError\nimport sys\ntry:\n DurableStore(sys.argv[1], 'inst')\nexcept StoreError as e:\n print(str(e))\n"
        child = subprocess.run([sys.executable, '-c', script, str(self.path)],
                               cwd=Path(__file__).resolve().parents[1], capture_output=True, text=True)
        self.assertEqual(child.returncode, 0, child.stderr)
        self.assertEqual(child.stdout.strip(), 'store_locked')
        self.store.close()
        self.assertTrue(Path(str(self.path) + '.lock').exists())
        self.store = DurableStore(self.path, 'inst')

    def test_instance_mismatch_fails_without_recovering_rows(self):
        self.create()
        self.store.close()
        self.assertCode('instance_conflict', lambda: DurableStore(self.path, 'other'))
        self.store = DurableStore(self.path, 'inst')
        self.assertEqual(self.store.get('session')['state']['state_version'], '1')

    def test_input_validation_and_unknown_rows_use_stable_codes(self):
        self.create()
        for version in (True, -1, '0'):
            self.assertCode('invalid_input', lambda: self.store.claim('session', 'a', 'fp', version))
        for state in (self.state() | {'state_version': 0}, self.state() | {'state_version': '01'},
                      self.state() | {'token': 'credential'}, self.state() | {'request': {'question': 'private'}}):
            self.assertCode('invalid_input', lambda: self.create('bad', state))
        self.assertCode('session_not_found', lambda: self.store.get('absent'))
        self.assertCode('action_not_found', lambda: self.store.action('session', 'absent'))
        for maximum in (-1, True, '2', 101):
            self.assertCode('invalid_input', lambda: self.store.reserve_model('session', maximum))
        self.assertCode('invalid_input', lambda: self.store.events('session', -1))

    def test_checkpoint_byte_bound_and_commit_failure_do_not_publish(self):
        self.create()
        self.store.claim('session', 'a', 'fp', 0)
        self.assertCode('checkpoint_too_large', lambda: self.store.commit(
            'session', 'a', self.state(1), '界' * 50000, {}))
        with closing(sqlite3.connect(self.path)) as db:
            db.execute("CREATE TRIGGER fail_commit BEFORE UPDATE ON sessions BEGIN SELECT RAISE(ABORT, 'disk fault'); END")
            db.commit()
        self.assertCode('storage_error', lambda: self.store.commit('session', 'a', self.state(1), None, {'ok': True}))
        self.assertEqual(self.store.get('session')['state']['state_version'], '0')
        self.assertEqual(self.store.action('session', 'a')['status'], 'pending')

    def test_session_action_and_event_storage_is_bounded(self):
        for i in range(32):
            self.create(str(i))
        self.assertCode('session_limit', lambda: self.create('extra'))
        for i in range(256):
            self.store.claim('0', str(i), 'fp', i)
            self.store.commit('0', str(i), self.state(i + 1), None, {})
        self.assertCode('action_limit', lambda: self.store.claim('0', 'extra', 'fp', 256))
        first = self.store.events('0', 0, 2)
        self.assertEqual([e['sequence'] for e in first['events']], [1, 2])
        self.assertEqual(first['high_watermark'], 256)
        self.assertEqual(len(self.store.events('0', 254)['events']), 2)

    def test_closed_store_cannot_report_success(self):
        self.create()
        self.store.close()
        self.store.close()
        self.assertCode('store_closed', lambda: self.store.get('session'))
        self.assertCode('store_closed', lambda: self.store.request_cancel('session'))

    def test_wire_card_tuples_are_json_arrays(self):
        self.create()
        self.store.claim('session', 'a', 'fp', 0)
        wire = self.state(1, 'WaitingStudent', [{'question': 'What changed?', 'hint': 'Inspect output',
                                               'evidence_ids': ('evidence',), 'limitations': ()}])
        self.store.commit('session', 'a', wire, None, wire)
        card = self.store.get('session')['state']['cards'][0]
        self.assertEqual(card['evidence_ids'], ['evidence'])
        self.assertEqual(card['limitations'], [])

    def test_cancel_flag_atomically_prevents_a_late_card_commit(self):
        self.create()
        self.store.claim('session', 'a', 'fp', 0)
        self.store.request_cancel('session')
        for status in ('WaitingStudent', 'Completed', 'Running'):
            self.assertCode('cancel_requested', lambda: self.store.commit(
                'session', 'a', self.state(1, status, [{'hint': 'late'}]), None, {}))
        self.assertEqual(self.store.action('session', 'a')['status'], 'pending')
        self.store.commit('session', 'a', self.state(1, 'Cancelled'), None, {'status': 'Cancelled'})
        self.assertEqual(self.store.get('session')['state']['status'], 'Cancelled')

    def test_malformed_status_and_top_level_student_question_are_rejected(self):
        for bad in ({}, [], 42):
            self.assertCode('invalid_input', lambda: self.create('bad', self.state() | {'status': bad}))
        self.assertCode('invalid_input', lambda: self.create('bad', self.state() | {'question': 'student request'}))

    def test_corruption_on_startup_releases_lock_for_repair(self):
        self.create()
        self.store.claim('session', 'a', 'fp', 0)
        self.store.commit('session', 'a', self.state(1), 'original', {})
        self.store.close()
        with closing(sqlite3.connect(self.path)) as db:
            db.execute("UPDATE sessions SET checkpoint = 'tampered'")
            db.commit()
        self.assertCode('checkpoint_corrupt', lambda: DurableStore(self.path, 'inst'))
        with closing(sqlite3.connect(self.path)) as db:
            db.execute("UPDATE sessions SET checkpoint = 'original'")
            db.commit()
        self.store = DurableStore(self.path, 'inst')
        self.assertEqual(self.store.get('session')['checkpoint'], 'original')

    def test_budget_corruption_cannot_make_free_model_reservations(self):
        self.create()
        with closing(sqlite3.connect(self.path)) as db:
            db.execute('UPDATE sessions SET model_used = -2')
            db.commit()
        self.assertCode('state_corrupt', lambda: self.store.reserve_model('session', 3))

    def test_event_retention_preserves_monotone_cursor(self):
        self.create()
        # Exercise pruning at a reduced cap without a thousand disk restarts.
        with patch('sigflow_edu_agent.host.store.MAX_EVENTS', 3):
            for version in range(5):
                self.store.claim('session', str(version), 'fp', version)
                self.store.commit('session', str(version), self.state(version + 1), None, {})
            for after in (0, 1):
                self.assertCode('cursor_expired', lambda: self.store.events('session', after, 3))
            rows = self.store.events('session', 2, 3)
        self.assertEqual([x['sequence'] for x in rows['events']], [3, 4, 5])
        self.assertEqual(rows['high_watermark'], 5)
        self.assertEqual(self.store.events('session', 5), {'events': [], 'high_watermark': 5})

    def test_future_cursor_is_rejected_for_empty_and_committed_sessions(self):
        self.create()
        self.assertEqual(self.store.events('session', 0), {'events': [], 'high_watermark': 0})
        self.assertCode('cursor_stale', lambda: self.store.events('session', 1))
        self.store.claim('session', 'a', 'fp', 0)
        self.store.commit('session', 'a', self.state(7), None, {})
        for after in (2, 2**63 - 1):
            self.assertCode('cursor_stale', lambda: self.store.events('session', after))
        self.assertEqual(self.store.events('session', 1), {'events': [], 'high_watermark': 1})

    def test_event_retention_and_high_watermark_are_scoped_to_each_session(self):
        self.create('older')
        self.create('other')
        with patch('sigflow_edu_agent.host.store.MAX_EVENTS', 2):
            for version in range(3):
                self.store.claim('older', str(version), 'fp', version)
                self.store.commit('older', str(version), self.state(version + 1), None, {})
            self.store.claim('other', '0', 'fp', 0)
            self.store.commit('other', '0', self.state(9), None, {})
            self.assertCode('cursor_expired', lambda: self.store.events('older', 0, 2))
            self.assertEqual(self.store.events('other', 0, 2), {
                'events': [{'sequence': 1, 'action_id': '0', 'state_version': '9',
                            'status': 'Running'}], 'high_watermark': 1})
        self.assertEqual(self.store.get('older')['event_high_watermark'], 3)
        self.assertEqual(self.store.get('other')['event_high_watermark'], 1)
        self.assertCode('cursor_stale', lambda: self.store.events('other', 3))

    def test_state_and_high_watermark_stay_coherent_during_commits(self):
        self.create()

        def publish():
            for index in range(20):
                self.store.claim('session', str(index), 'fp', index * 3)
                self.store.commit('session', str(index), self.state((index + 1) * 3), None, {})

        with ThreadPoolExecutor(max_workers=1) as pool:
            writer = pool.submit(publish)
            for _ in range(40):
                row = self.store.get('session')
                self.assertEqual(int(row['state']['state_version']), row['event_high_watermark'] * 3)
            writer.result()
        row = self.store.get('session')
        self.assertEqual(row['state']['state_version'], '60')
        self.assertEqual(row['event_high_watermark'], 20)

    def test_uncommitted_work_and_failed_commit_do_not_advance_high_watermark(self):
        self.create()
        self.store.claim('session', 'a', 'fp', 0)
        self.store.reserve_model('session', 3)
        self.store.request_cancel('session')
        self.assertEqual(self.store.get('session')['event_high_watermark'], 0)
        self.assertCode('cancel_requested', lambda: self.store.commit(
            'session', 'a', self.state(1), None, {}))
        self.assertEqual(self.store.events('session', 0), {'events': [], 'high_watermark': 0})
        self.store.commit('session', 'a', self.state(8, 'Cancelled'), None, {})
        row = self.store.get('session')
        self.assertEqual(row['state']['state_version'], '8')
        self.assertEqual(row['event_high_watermark'], 1)
        self.reopen()
        self.assertEqual(self.store.get('session')['event_high_watermark'], 1)

    def test_events_publish_only_commit_metadata_without_cards_or_checkpoints(self):
        self.create()
        self.store.claim('session', 'a', 'fp', 0)
        self.store.commit('session', 'a', self.state(1, 'WaitingStudent', [
            {'question': 'private lesson question', 'hint': 'private lesson hint'}]),
            'private checkpoint', {'summary': 'private action result'})
        self.assertEqual(self.store.events('session', 0), {
            'events': [{'sequence': 1, 'action_id': 'a', 'state_version': '1',
                        'status': 'WaitingStudent'}], 'high_watermark': 1})


if __name__ == '__main__':
    unittest.main()
