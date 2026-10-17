import tempfile
import time
import unittest
from pathlib import Path
from dataclasses import replace
from threading import Event

from sigflow_edu_agent import host
from sigflow_edu_agent.demo import FixtureSource
from sigflow_edu_agent.domain import RunRequest, Status, VerificationResult
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.runtime.session import TeachingSession


class HostRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(hasattr(host, 'Runtime'), 'runtime_missing')
        from sigflow_edu_agent.host.store import DurableStore
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'state.sqlite3'
        self.store = DurableStore(self.path, 'inst')
        self.source = FixtureSource()
        self.provider = None
        self.runtime = self.open_runtime()
        self.addCleanup(self.shutdown)
        self.body = dict(learner_id='learner', project_id='p', revision='r',
                         snapshot_id='snap', job_id='fixture-job-1', diagnostic_id='diag',
                         issue_id='issue', question='为什么推断存储？', intent='diagnose', mode='rule')

    def open_runtime(self):
        def factory(binding, request, mode, reserve, **trusted):
            selected = self.provider
            provider = selected
            if selected:
                class Budgeted:
                    def generate(inner, context):
                        reserve()
                        return selected.generate(context)
                provider = Budgeted()
            return TeachingSession(TeachingLoop(request, self.source, provider,
                plan=trusted.get('plan'), verification=getattr(self, 'verification', None)))
        return host.Runtime(self.store, 'inst', factory, evidence_origin='fixture')

    def shutdown(self):
        self.runtime.close()
        self.store.close()

    def wait(self, sid, action='create'):
        until = time.monotonic() + 4
        while time.monotonic() < until:
            operation = self.store.action(sid, action)
            if operation['status'] in ('committed', 'interrupted'):
                return self.runtime.snapshot(sid, 'learner')
            time.sleep(.01)
        self.fail('worker did not complete')

    def create(self, **changes):
        key = changes.pop('_key', 'create-key')
        accepted = self.runtime.create(self.body | changes, key)
        self.assertTrue(accepted['accepted'])
        return accepted['session_id']

    def test_real_teaching_loop_and_idempotent_hints(self):
        sid = self.create()
        first = self.wait(sid)
        self.assertEqual((first['status'], first['level']), ('WaitingStudent', 1))
        self.assertEqual(first['cards'][0]['producer'], 'rule')
        self.assertEqual(first['evidence_origin'], 'fixture')
        accepted = self.runtime.action(sid, 'learner', 'hint_next', 'hint-1', int(first['state_version']))
        self.assertTrue(accepted['accepted'])
        second = self.wait(sid, 'hint-1')
        replay = self.runtime.action(sid, 'learner', 'hint_next', 'hint-1', int(first['state_version']))
        self.assertEqual(replay['result']['level'], 2)
        self.assertEqual(self.runtime.snapshot(sid, 'learner'), second)
        self.runtime.action(sid, 'learner', 'hint_next', 'hint-2', int(second['state_version']))
        self.assertEqual(self.wait(sid, 'hint-2')['level'], 3)

    def test_reject_scope_stale_unknown_and_l4_before_mutation(self):
        sid = self.create()
        first = self.wait(sid)
        for learner, kind, version in [('other', 'hint_next', int(first['state_version'])),
                                       ('learner', 'hint_next', 0), ('learner', 'shell', 5),
                                       ('learner', 'reference_confirm', 5)]:
            with self.subTest(kind=kind, learner=learner), self.assertRaises(ValueError):
                self.runtime.action(sid, learner, kind, 'bad', version)
        self.assertEqual(self.runtime.snapshot(sid, 'learner'), first)

    def test_creation_key_conflict_and_single_active_project(self):
        sid = self.create()
        self.wait(sid)
        self.assertEqual(self.runtime.create(self.body, 'create-key')['session_id'], sid)
        with self.assertRaises(ValueError):
            self.runtime.create(self.body | {'question': '另一个问题'}, 'create-key')
        with self.assertRaises(ValueError):
            self.runtime.create(self.body | {'issue_id': 'other'}, 'another-key')

    def test_restart_requires_original_question_and_fresh_reconcile(self):
        sid = self.create()
        first = self.wait(sid)
        self.runtime.close()
        self.store.close()
        from sigflow_edu_agent.host.store import DurableStore
        self.store = DurableStore(self.path, 'inst')
        self.runtime = self.open_runtime()
        recovered = self.runtime.snapshot(sid, 'learner')
        self.assertEqual(recovered['status'], 'RecoveryRequired')
        self.assertEqual(recovered['cards'], [])
        with self.assertRaises(ValueError):
            self.runtime.recover(sid, 'learner', 'recover-wrong', int(recovered['state_version']), 'wrong')
        self.runtime.recover(sid, 'learner', 'recover-1', int(recovered['state_version']), self.body['question'])
        after = self.wait(sid, 'recover-1')
        self.assertEqual(after['status'], 'WaitingStudent')
        self.assertEqual(after['cards'], first['cards'])

    def test_no_student_question_or_runtime_secret_in_database(self):
        self.body['question'] = 'unique-private-request-4826'
        sid = self.create()
        self.wait(sid)
        self.assertNotIn(b'unique-private-request-4826', self.path.read_bytes())

    def test_recovery_action_replay_does_not_requery_or_republish(self):
        sid = self.create()
        self.wait(sid)
        self.runtime.close()
        self.store.close()
        from sigflow_edu_agent.host.store import DurableStore
        self.store = DurableStore(self.path, 'inst')
        self.runtime = self.open_runtime()
        version = int(self.runtime.snapshot(sid, 'learner')['state_version'])
        self.runtime.recover(sid, 'learner', 'recovery', version, self.body['question'])
        result = self.wait(sid, 'recovery')
        replay = self.runtime.recover(sid, 'learner', 'recovery', version, self.body['question'])
        # The cursor is atomic snapshot metadata, not part of a historical
        # action result. Read /state to resubscribe after a replay.
        self.assertEqual(replay['result'], {k:v for k,v in result.items() if k != 'event_high_watermark'})
        with self.assertRaises(ValueError):
            self.runtime.recover(sid, 'learner', 'recovery', version, 'changed')

    def test_model_budget_is_reserved_before_provider_and_survives_restart(self):
        owner = self
        class InvalidProvider:
            def generate(inner, context):
                self.assertGreater(owner.store.model_used(owner.sid), 0)
                return {'unreviewed': 'bad'}
        self.provider = InvalidProvider()
        self.sid = self.create(mode='deepseek')
        first = self.wait(self.sid)
        self.assertEqual(self.store.model_used(self.sid), 2)
        self.runtime.action(self.sid, 'learner', 'hint_next', 'h1', int(first['state_version']))
        second = self.wait(self.sid, 'h1')
        self.assertEqual(self.store.model_used(self.sid), 4)
        self.runtime.close()
        self.store.close()
        from sigflow_edu_agent.host.store import DurableStore
        self.store = DurableStore(self.path, 'inst')
        self.runtime = self.open_runtime()
        recover = self.runtime.snapshot(self.sid, 'learner')
        self.runtime.recover(self.sid, 'learner', 'recovery', int(recover['state_version']), self.body['question'])
        restored = self.wait(self.sid, 'recovery')
        self.runtime.action(self.sid, 'learner', 'hint_next', 'h2', int(restored['state_version']))
        final = self.wait(self.sid, 'h2')
        self.assertEqual(final['cards'][-1]['producer'], 'rule')
        self.assertEqual(self.store.model_used(self.sid), 4)

    def test_cancel_during_blocking_evidence_does_not_publish_late_card(self):
        entered, release = Event(), Event()
        original = self.source.collect
        def blocked(*args):
            entered.set()
            release.wait(3)
            return original(*args)
        self.source.collect = blocked
        sid = self.create()
        self.assertTrue(entered.wait(1))
        ack = self.runtime.cancel(sid, 'learner')
        self.assertEqual(ack['status'], 'cancel_requested')
        release.set()
        result = self.wait(sid)
        self.assertEqual(result['status'], 'Cancelled')
        self.assertEqual(result['cards'], [])

    def verification_setup(self, job_state):
        from sigflow_edu_agent.context.models import EvidenceAnchor
        owner = self
        self.source = FixtureSource()
        original = self.source.collect
        self.source.collect = lambda p, r: replace(original(p, r), anchor=EvidenceAnchor(
            'fixture-latch-1', p, r, 'fixture-job-1', 'snap', 'artifact-1', 'a' * 64))
        class Verification:
            def authorized(inner, plan, grant):
                return grant == 'fixture-grant'
            def execute(inner, plan, index, grant, key):
                if job_state != 'Succeeded':
                    return VerificationResult(job_state, None, None)
                current = owner.source.collect('p', 'r')
                evidence = replace(current, evidence_id='result', job_id='result-job',
                    anchor=replace(current.anchor, evidence_id='result', job_id='result-job'))
                return VerificationResult('Succeeded', evidence, None)
        self.verification = Verification()
        self.plan_body = dict(project_id='p', revision='r', snapshot_id='snap', goal='验证存储行为',
            steps=[dict(step_id='s1', capability='eda.synth', provider='yosys', params={'top': 'counter'})],
            expected_observations=['报告中的存储行为'], max_jobs=1)

    def test_detailed_plan_flows_through_authorized_port_and_bounded_outcome(self):
        self.verification_setup('Succeeded')
        sid = self.create(intent='verify', plan=self.plan_body, gateway_plan_hash='ui-hash')
        waiting = self.wait(sid)
        self.assertEqual(waiting['status'], 'WaitingApproval')
        self.runtime.action(sid, 'learner', 'execute_plan', 'execute', int(waiting['state_version']),
                            grant_ref='fixture-grant')
        result = self.wait(sid, 'execute')
        self.assertEqual(result['status'], 'Completed')
        self.assertEqual(result['reason'], 'outcome_indeterminate')
        self.assertIn('不是真实 EDA', result['cards'][-1]['limitations'][-1])

    def test_pending_and_unknown_jobs_stop_following_steps_for_reconciliation(self):
        for state in ('Running', 'Unknown'):
            with self.subTest(state=state):
                self.verification_setup(state)
                sid = self.create(_key='verify-'+state, intent='verify', plan=self.plan_body, gateway_plan_hash='ui-hash')
                waiting = self.wait(sid)
                self.runtime.action(sid, 'learner', 'execute_plan', 'execute', int(waiting['state_version']),
                                    grant_ref='fixture-grant')
                result = self.wait(sid, 'execute')
                self.assertEqual(result['status'], 'RecoveryRequired')
                self.assertFalse(any('功能正确' in c['hint'] for c in result['cards']))
                self.runtime.cancel(sid, 'learner')
                self.assertEqual(self.runtime.snapshot(sid, 'learner')['status'], 'Cancelled')
                self.body['issue_id'] += '-next'

    def restart(self):
        self.runtime.close()
        self.store.close()
        from sigflow_edu_agent.host.store import DurableStore
        self.store = DurableStore(self.path, 'inst')
        self.runtime = self.open_runtime()

    def test_replay_cannot_bypass_recovery_card_quarantine(self):
        sid = self.create()
        first = self.wait(sid)
        self.runtime.action(sid, 'learner', 'hint_next', 'h1', int(first['state_version']))
        self.wait(sid, 'h1')
        self.restart()
        replay = self.runtime.action(sid, 'learner', 'hint_next', 'h1', int(first['state_version']))
        self.assertEqual(replay['result']['cards'], [])
        self.assertEqual(replay['result']['status'], 'RecoveryRequired')

    def test_failed_reconcile_preserves_checkpoint_and_allows_explicit_retry(self):
        sid = self.create()
        first = self.wait(sid)
        self.restart()
        original = self.source.collect
        self.source.collect = lambda *args: None
        recovering = self.runtime.snapshot(sid, 'learner')
        self.runtime.recover(sid, 'learner', 'recover-failed', int(recovering['state_version']), self.body['question'])
        failed = self.wait(sid, 'recover-failed')
        self.assertEqual(failed['status'], 'RecoveryRequired')
        self.assertIsNotNone(self.store.get(sid)['checkpoint'])
        self.restart()
        self.source.collect = original
        recovering = self.runtime.snapshot(sid, 'learner')
        self.runtime.recover(sid, 'learner', 'recover-again', int(recovering['state_version']), self.body['question'])
        self.assertEqual(self.wait(sid, 'recover-again')['cards'], first['cards'])

    def test_cancel_recovered_session_terminalizes_and_releases_project(self):
        sid = self.create()
        self.wait(sid)
        self.restart()
        self.runtime.cancel(sid, 'learner')
        self.assertEqual(self.runtime.snapshot(sid, 'learner')['status'], 'Cancelled')
        new = self.create(_key='new-after-cancel', issue_id='other')
        self.assertEqual(self.wait(new)['status'], 'WaitingStudent')

    def test_cancel_after_commit_before_worker_cleanup_is_not_lost(self):
        original_commit = self.store.commit
        entered, release = Event(), Event()
        self.addCleanup(release.set)
        def block_after_commit(sid, action, *args):
            original_commit(sid, action, *args)
            if action == 'create':
                entered.set()
                release.wait(2)
        self.store.commit = block_after_commit
        sid = self.create()
        self.assertTrue(entered.wait(1))
        self.runtime.cancel(sid, 'learner')
        release.set()
        deadline = time.monotonic() + 2
        while self.runtime.snapshot(sid, 'learner')['status'] != 'Cancelled' and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertEqual(self.runtime.snapshot(sid, 'learner')['status'], 'Cancelled')

    def test_sqlite_initial_reservation_failure_rolls_back_entire_creation(self):
        self.store._db.execute("CREATE TRIGGER fail_claim BEFORE INSERT ON actions BEGIN SELECT RAISE(FAIL, 'write fail'); END")
        with self.assertRaises(ValueError):
            self.runtime.create(self.body, 'create-key')
        self.assertEqual(self.store.list_sessions(), [])
        self.store._db.execute('DROP TRIGGER fail_claim')
        sid = self.create()
        self.assertEqual(self.wait(sid)['status'], 'WaitingStudent')

    def test_cancel_has_no_collision_with_client_action_key_or_action_quota(self):
        from unittest.mock import patch
        sid = self.create()
        state = self.wait(sid)
        self.runtime.action(sid, 'learner', 'hint_next', 'cancel-10', int(state['state_version']))
        self.wait(sid, 'cancel-10')
        with patch('sigflow_edu_agent.host.store.MAX_ACTIONS', 2):
            self.runtime.cancel(sid, 'learner')
        self.assertEqual(self.runtime.snapshot(sid, 'learner')['status'], 'Cancelled')

    def test_cancel_can_interrupt_pending_after_storage_failure_without_rerun(self):
        self.store._db.execute("CREATE TRIGGER fail_state BEFORE UPDATE OF state ON sessions BEGIN SELECT RAISE(FAIL, 'write fail'); END")
        sid = self.create()
        deadline = time.monotonic() + 2
        while sid in self.runtime._threads and time.monotonic() < deadline:
            time.sleep(.01)
        self.store._db.execute('DROP TRIGGER fail_state')
        self.assertEqual(self.store.action(sid, 'create')['status'], 'pending')
        self.runtime.cancel(sid, 'learner')
        self.assertEqual(self.runtime.snapshot(sid, 'learner')['status'], 'Cancelled')
        self.assertEqual(self.store.action(sid, 'create')['status'], 'interrupted')

    def test_cancel_schedules_only_the_owned_verification_port(self):
        self.verification_setup('Running')
        entered, cancelled = Event(), Event()
        def blocked(*args):
            entered.set()
            cancelled.wait(2)
            return VerificationResult('Running', None, None)
        def cancel_owned(plan):
            self.assertEqual(plan.project_id, 'p')
            cancelled.set()
            return ()
        self.verification.execute = blocked
        self.verification.cancel_owned = cancel_owned
        sid = self.create(intent='verify', plan=self.plan_body, gateway_plan_hash='ui-hash')
        state = self.wait(sid)
        self.runtime.action(sid, 'learner', 'execute_plan', 'execute', int(state['state_version']), grant_ref='fixture-grant')
        self.assertTrue(entered.wait(1))
        ack = self.runtime.cancel(sid, 'learner')
        self.assertTrue(ack['eda_cancel_scheduled'])
        self.assertTrue(cancelled.wait(1))
        self.assertEqual(self.runtime.snapshot(sid, 'learner')['status'], 'Cancelled')

    def test_action_cancel_uses_the_same_owned_job_hook_and_replays_safely(self):
        self.verification_setup('Running')
        cancelled = Event()
        calls = []
        def cancel_owned(plan):
            calls.append(plan.fingerprint)
            cancelled.set()
            return ()
        self.verification.cancel_owned = cancel_owned
        sid = self.create(intent='verify', plan=self.plan_body, gateway_plan_hash='ui-hash')
        state = self.wait(sid)
        version = int(state['state_version'])
        self.runtime.action(sid, 'learner', 'cancel', 'cancel-action', version)
        self.assertEqual(self.wait(sid, 'cancel-action')['status'], 'Cancelled')
        self.assertTrue(cancelled.wait(.3), 'actions/cancel must request cancellation of owned EDA Jobs')
        replay = self.runtime.action(sid, 'learner', 'cancel', 'cancel-action', version)
        self.assertEqual(replay['result']['status'], 'Cancelled')
        self.assertEqual(len(calls), 1)

    def test_cancel_while_e6_collects_prevents_later_job_submission(self):
        self.verification_setup('Running')
        sid = self.create(intent='verify', plan=self.plan_body, gateway_plan_hash='ui-hash')
        state = self.wait(sid)
        entered, release = Event(), Event()
        self.addCleanup(release.set)
        original = self.source.collect
        def blocked(*args):
            entered.set()
            release.wait(2)
            return original(*args)
        self.source.collect = blocked
        submissions = []
        self.verification.execute = lambda *a: submissions.append(a) or VerificationResult('Running', None, None)
        self.verification.cancel_owned = lambda p: ()
        self.runtime.action(sid, 'learner', 'execute_plan', 'execute', int(state['state_version']), grant_ref='fixture-grant')
        self.assertTrue(entered.wait(1))
        self.runtime.cancel(sid, 'learner')
        release.set()
        self.runtime.close()
        self.assertEqual(submissions, [])
        self.assertEqual(self.runtime.snapshot(sid, 'learner')['status'], 'Cancelled')


if __name__ == '__main__':
    unittest.main()
