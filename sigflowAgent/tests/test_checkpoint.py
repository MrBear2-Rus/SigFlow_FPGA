import json
from pathlib import Path
import subprocess
import sys
import unittest
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace
from threading import Barrier

from sigflow_edu_agent.demo import FixtureSource
from sigflow_edu_agent.context.models import EvidenceAnchor
from sigflow_edu_agent.domain import Plan, RunRequest, Status
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.runtime.gateway_session import SessionBinding
from sigflow_edu_agent.runtime.session import TeachingSession


class Source(FixtureSource):
    def __init__(self):
        self.current = super().collect('p', 'r')
        self.calls = 0
        self.error = None

    def collect(self, project_id, revision):
        self.calls += 1
        if self.error:
            raise self.error
        return self.current


class Provider:
    def __init__(self):
        self.calls = 0

    def generate(self, context):
        self.calls += 1
        return dict(level=context.level, hint='先观察存储行为。', question='输出是否需要记忆？',
                    evidence_ids=[context.evidence.evidence_id])


class CheckpointTests(unittest.TestCase):
    def setUp(self):
        self.binding = SessionBinding('inst', 'session', 'learner', 'p', 'r', 'snap',
                                      'fixture-job-1', 'diag', 'issue')
        self.request = RunRequest('p', 'r', 'issue', 'private student question')
        self.source, self.provider = Source(), Provider()
        self.session = TeachingSession(TeachingLoop(self.request, self.source, self.provider))

    def pause(self, session=None):
        session = session or self.session
        for _ in range(32):
            if session.state.status != Status.RUNNING:
                return session.state
            session.advance(session.state.version)
        self.fail('did not pause')

    def save(self):
        return self.session.checkpoint(self.binding)

    def restore(self, payload=None, **changes):
        from sigflow_edu_agent.runtime import checkpoint
        args = dict(binding=self.binding, request=self.request, source=self.source,
                    provider=self.provider)
        return checkpoint.restore_readonly_session(self.save() if payload is None else payload,
                                                   **(args | changes))

    def test_restored_cards_are_quarantined_until_fresh_evidence_check(self):
        before = self.pause()
        restored = self.restore()
        self.assertEqual(restored.state.status, Status.RECOVERY_REQUIRED)
        self.assertEqual(restored.state.run_id, before.run_id)
        self.assertEqual(restored.state.cards, [])
        calls = self.source.calls, self.provider.calls
        for method in (lambda: restored.advance(restored.state.version),
                       lambda: restored.host_action('hint_next', 'new', restored.state.version)):
            with self.assertRaisesRegex(ValueError, 'recovery_required'):
                method()
        self.assertEqual((self.source.calls, self.provider.calls), calls)
        state = restored.reconcile(restored.state.version)
        self.assertEqual(state.status, Status.WAITING_STUDENT)
        self.assertEqual(state.cards, before.cards)
        self.assertGreater(state.version, before.version)
        self.assertEqual(self.provider.calls, 1)
        self.assertEqual(self.source.calls, 2)
        self.assertFalse(restored.check_phase('E4')[0])

    def test_replayed_hint_survives_restart_without_double_escalation(self):
        first = self.pause()
        self.session.host_action('hint_next', 'accepted', first.version)
        before = self.pause()
        restored = self.restore()
        state = restored.reconcile(restored.state.version)
        replay = restored.host_action('hint_next', 'accepted', first.version)
        self.assertEqual(replay, state)
        self.assertEqual(replay.level, 2)
        with self.assertRaisesRegex(ValueError, 'action_conflict'):
            restored.host_action('finish', 'accepted', first.version)
        restored.host_action('hint_next', 'next', state.version)
        self.assertEqual(self.pause(restored).level, 3)

    def test_initial_refresh_after_failed_collection_can_be_checkpointed(self):
        self.source.error = RuntimeError('temporary failure')
        failed = self.pause()
        self.assertEqual(failed.status, Status.WAITING_EVIDENCE)
        self.source.error = None
        self.session.host_action('refresh_evidence', 'retry-1', failed.version)
        before = self.pause()
        restored = self.restore()
        state = restored.reconcile(restored.state.version)
        self.assertEqual(state.status, Status.WAITING_STUDENT)
        self.assertEqual((state.level, state.steps, state.model_calls), (1, 6, 1))
        self.assertEqual(restored.host_action('refresh_evidence', 'retry-1', failed.version), state)

    def test_refresh_after_recovered_report_failure_can_be_saved_again(self):
        self.pause()
        original = self.source.current
        restored = self.restore()
        state = restored.reconcile(restored.state.version)
        self.source.current = replace(original, summary='changed')
        restored.host_action('hint_next', 'next', state.version)
        failed = self.pause(restored)
        self.source.current = original
        restored.host_action('refresh_evidence', 'refresh', failed.version)
        published = self.pause(restored)
        payload = restored.checkpoint(self.binding)
        again = self.restore(payload)
        final = again.reconcile(again.state.version)
        self.assertEqual((final.status, final.level, final.steps, final.model_calls),
                         (Status.WAITING_STUDENT, 2, 10, 2))
        self.assertEqual(final.cards, published.cards)
        self.assertEqual(again.host_action('refresh_evidence', 'refresh', failed.version), final)

    def test_recovery_can_be_cancelled_without_evidence_or_model_io(self):
        self.pause()
        restored = self.restore()
        before = restored.state
        self.source.error = RuntimeError('offline')
        calls = self.source.calls, self.provider.calls
        state = restored.host_action('cancel', 'stop-recovery', before.version)
        self.assertEqual(state.status, Status.CANCELLED)
        self.assertEqual(state.cards, [])
        self.assertEqual((self.source.calls, self.provider.calls), calls)
        self.assertEqual(restored.host_action('cancel', 'stop-recovery', before.version), state)
        with self.assertRaisesRegex(ValueError, 'recovery_not_required'):
            restored.reconcile(state.version)

    def test_old_ui_version_and_l4_cannot_unlock_after_reconciliation(self):
        before = self.pause()
        restored = self.restore()
        state = restored.reconcile(restored.state.version)
        with self.assertRaisesRegex(ValueError, 'stale_state'):
            restored.host_action('finish', 'old', before.version)
        for kind in ('reference_request', 'reference_confirm'):
            with self.assertRaisesRegex(ValueError, 'receipt_unavailable'):
                restored.host_action(kind, kind, state.version)
        with self.assertRaisesRegex(ValueError, 'action_not_allowed'):
            restored.host_action('execute_plan', 'execute', state.version, grant_ref='old-grant')
        self.assertEqual(restored.state, state)

    def test_budget_and_model_counter_do_not_reset_on_restart(self):
        self.session = TeachingSession(TeachingLoop(self.request, self.source, self.provider,
                                                   max_steps=9, max_model_calls=1))
        first = self.pause()
        restored = self.restore()
        recovered = restored.reconcile(restored.state.version)
        self.assertEqual((recovered.steps, recovered.model_calls), (5, 1))
        restored.host_action('hint_next', 'next', recovered.version)
        second = self.pause(restored)
        self.assertEqual((second.steps, second.model_calls), (9, 1))
        self.assertEqual(second.cards[-1].producer, 'rule')
        self.assertEqual(self.provider.calls, 1)
        restored.host_action('hint_next', 'last', second.version)
        self.assertEqual(self.pause(restored).status, Status.BUDGET_EXCEEDED)

    def test_cross_scope_or_changed_request_rejected_before_io(self):
        self.pause()
        payload = self.save()
        calls = self.source.calls
        for field in ('instance_id', 'session_id', 'learner_id', 'project_id', 'revision',
                      'snapshot_id', 'job_id', 'diagnostic_id', 'issue_id'):
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.restore(payload, binding=replace(self.binding, **{field: 'other'}))
        for field in ('project_id', 'revision', 'issue_id', 'question', 'intent'):
            value = 'explain' if field == 'intent' else 'other'
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.restore(payload, request=replace(self.request, **{field: value}))
        self.assertEqual(self.source.calls, calls)

    def test_changed_missing_or_incomplete_evidence_keeps_cards_hidden(self):
        self.pause()
        payload = self.save()
        original = self.source.current
        for evidence in (None, replace(original, complete=False),
                         replace(original, summary='changed'), replace(original, job_id='other'),
                         replace(original, revision='other')):
            with self.subTest(evidence=evidence):
                self.source.current = evidence
                restored = self.restore(payload)
                state = restored.reconcile(restored.state.version)
                self.assertEqual(state.status, Status.RECOVERY_REQUIRED)
                self.assertEqual(state.cards, [])
                self.assertEqual(self.provider.calls, 1)
        self.source.current = original
        state = restored.reconcile(restored.state.version)
        self.assertEqual(state.status, Status.WAITING_STUDENT)

    def test_tool_evidence_requires_matching_live_snapshot_anchor(self):
        original = self.source.current
        self.source.current = replace(original, origin='tool')
        self.pause()
        with self.assertRaisesRegex(ValueError, 'checkpoint_not_available'):
            self.save()
        anchor = EvidenceAnchor(original.evidence_id, 'p', 'r', original.job_id, 'snap',
                                'artifact', 'a' * 64)
        self.source.current = replace(original, origin='tool', anchor=anchor)
        self.session = TeachingSession(TeachingLoop(self.request, self.source, self.provider))
        self.pause()
        payload = self.save()
        for changed in (replace(anchor, snapshot_id='other'), replace(anchor, expired=True),
                        replace(anchor, artifact_hash='b' * 64)):
            with self.subTest(anchor=changed):
                self.source.current = replace(self.source.current, anchor=changed)
                restored = self.restore(payload)
                state = restored.reconcile(restored.state.version)
                self.assertEqual(state.status, Status.RECOVERY_REQUIRED)
                self.assertEqual(state.cards, [])

    def test_student_finish_and_l3_recover_without_new_privileges(self):
        state = self.pause()
        for action_id in ('more-1', 'more-2'):
            self.session.host_action('hint_next', action_id, state.version)
            state = self.pause()
        restored = self.restore()
        state = restored.reconcile(restored.state.version)
        self.assertEqual(state.level, 3)
        self.assertEqual(len(state.cards), 3)
        with self.assertRaisesRegex(ValueError, 'reference_required'):
            restored.host_action('hint_next', 'too-far', state.version)
        restored.host_action('finish', 'done', state.version)
        checkpoint_restore = restored.checkpoint(self.binding)
        restored_again = self.restore(checkpoint_restore)
        final = restored_again.reconcile(restored_again.state.version)
        self.assertEqual(final.status, Status.COMPLETED)
        self.assertEqual(final.level, 3)
        self.assertEqual(final.model_calls, 3)
        self.assertEqual(restored_again.host_action('finish', 'done', state.version), final)

    def test_failed_and_unreconciled_sessions_cannot_checkpoint(self):
        self.pause()
        restored = self.restore()
        with self.assertRaisesRegex(ValueError, 'checkpoint_not_available'):
            restored.checkpoint(self.binding)
        self.session.host_action('cancel', 'stop', self.session.state.version)
        with self.assertRaisesRegex(ValueError, 'checkpoint_not_available'):
            self.save()

    def test_source_failure_is_redacted_and_does_not_publish(self):
        self.pause()
        restored = self.restore()
        self.source.error = RuntimeError('private credential and filesystem path')
        state = restored.reconcile(restored.state.version)
        self.assertEqual(state.reason, 'recovery_evidence_unavailable')
        self.assertEqual(state.cards, [])
        self.assertNotIn('credential', repr(state))

    def test_checkpoint_contains_no_raw_question_evidence_or_provider_configuration(self):
        self.pause()
        payload = self.save()
        self.assertIsInstance(payload, str)
        self.assertNotIn(self.request.question, payload)
        self.assertNotIn(self.source.current.summary, payload)
        self.assertNotIn('grant_ref', payload)
        self.assertNotIn('api_key', payload)
        restored = self.restore(payload)
        restored.reconcile(restored.state.version)
        second = json.loads(restored.checkpoint(self.binding))
        self.assertEqual(second['state']['run_id'], self.session.state.run_id)

    def test_restored_binding_cannot_be_relabelled_in_new_checkpoint(self):
        self.pause()
        restored = self.restore()
        restored.reconcile(restored.state.version)
        with self.assertRaisesRegex(ValueError, 'checkpoint_scope_mismatch'):
            restored.checkpoint(replace(self.binding, learner_id='different'))

    def test_changed_report_after_recovery_cannot_generate_another_hint(self):
        self.pause()
        restored = self.restore()
        state = restored.reconcile(restored.state.version)
        self.source.current = replace(self.source.current, summary='changed after recovery')
        restored.host_action('hint_next', 'more', state.version)
        state = self.pause(restored)
        self.assertEqual(state.status, Status.WAITING_EVIDENCE)
        self.assertEqual(state.reason, 'stale_report')
        self.assertEqual(len(state.cards), 1)
        self.assertEqual(len(restored.occurrences), 1)
        self.assertEqual(self.provider.calls, 1)

    def test_only_readonly_published_boundary_can_be_saved(self):
        with self.assertRaisesRegex(ValueError, 'checkpoint_not_available'):
            self.save()
        state = self.pause()
        self.session.host_action('hint_next', 'a', state.version)
        with self.assertRaisesRegex(ValueError, 'checkpoint_not_available'):
            self.save()
        plan = Plan('p', 'r', 'verify', ('eda.synth',))
        for intent in ('verify', 'diagnose'):
            session = TeachingSession(TeachingLoop(replace(self.request, intent=intent),
                                                   self.source, plan=plan))
            self.pause(session)
            with self.assertRaisesRegex(ValueError, 'checkpoint_not_available'):
                session.checkpoint(self.binding)

    def test_complete_explanation_restores_without_generating_another_card(self):
        self.request = replace(self.request, intent='explain')
        self.session = TeachingSession(TeachingLoop(self.request, self.source, self.provider))
        before = self.pause()
        restored = self.restore()
        state = restored.reconcile(restored.state.version)
        self.assertEqual(state.status, Status.COMPLETED)
        self.assertEqual(state.cards, before.cards)
        self.assertEqual(self.provider.calls, 1)
        self.assertEqual(restored.advance(state.version), state)

    def test_restart_in_new_process_preserves_level_budget_and_action_replay(self):
        first = self.pause()
        self.session.host_action('hint_next', 'accepted', first.version)
        before = self.pause()
        script = '''
import json,sys
from sigflow_edu_agent.demo import FixtureSource
from sigflow_edu_agent.domain import RunRequest
from sigflow_edu_agent.runtime.gateway_session import SessionBinding
from sigflow_edu_agent.runtime.checkpoint import restore_readonly_session
payload=sys.stdin.read()
binding=SessionBinding('inst','session','learner','p','r','snap','fixture-job-1','diag','issue')
session=restore_readonly_session(payload,binding=binding,
    request=RunRequest('p','r','issue','private student question'),source=FixtureSource())
hidden=len(session.state.cards)
state=session.reconcile(session.state.version)
replay=session.host_action('hint_next','accepted',5)
print(json.dumps(dict(run_id=state.run_id,level=state.level,steps=state.steps,
    model_calls=state.model_calls,cards=len(state.cards),hidden=hidden,
    replay_unchanged=state==replay,status=state.status)))
'''
        result = subprocess.run([sys.executable, '-S', '-c', script], input=self.save(),
                                text=True, capture_output=True, timeout=10,
                                cwd=Path(__file__).resolve().parents[1])
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report, dict(run_id=before.run_id, level=2, steps=9,
                                      model_calls=2, cards=2, hidden=0,
                                      replay_unchanged=True, status='WaitingStudent'))

    def test_concurrent_reconciliation_accepts_only_current_version(self):
        self.pause()
        restored = self.restore()
        version, barrier = restored.state.version, Barrier(2)
        def reconcile():
            barrier.wait(timeout=3)
            try:
                return restored.reconcile(version).status
            except ValueError as error:
                return str(error)
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: reconcile(), range(2)))
        self.assertCountEqual(results, [Status.WAITING_STUDENT, 'stale_state'])
        self.assertEqual(self.source.calls, 2)

    def test_invalid_schema_duplicate_keys_and_size_are_rejected(self):
        self.pause()
        payload = self.save()
        for invalid in ('', '{}', '[]', '{"schema":"a","schema":"b"}',
                        payload.replace('readonly-checkpoint.v1', 'readonly-checkpoint.v99'),
                        ' ' * (128 * 1024 + 1), payload[:-1], payload.encode(), True, 1, {}):
            with self.subTest(kind=type(invalid)), self.assertRaisesRegex(ValueError, 'invalid_checkpoint'):
                self.restore(invalid)

    def test_invalid_counts_actions_or_card_never_restore_authority(self):
        self.pause()
        original = json.loads(self.save())
        edits = (('state', 'level', 4), ('state', 'model_calls', True),
                 ('state', 'steps', -1), ('state', 'version', 0),
                 ('state', 'status', 'Running'), ('state', 'run_id', '../path'),
                 ('budgets', 'max_model_calls', 0), ('state', 'cards', []),
                 ('actions', 'evil', ['execute_plan', 1, 'grant']))
        for section, field, value in edits:
            with self.subTest(field=field), self.assertRaises(ValueError):
                data = json.loads(json.dumps(original))
                data[section][field] = value
                self.restore(json.dumps(data))
        for field, value in (('level', 4), ('evidence_ids', ['invented']), ('producer', 'trusted-ui'),
                             ('hint', '``` leaked answer'), ('question', '\ud800')):
            with self.subTest(card_field=field), self.assertRaises(ValueError):
                data = json.loads(json.dumps(original))
                data['state']['cards'][0][field] = value
                restored = self.restore(json.dumps(data))
                restored.reconcile(restored.state.version)


if __name__ == '__main__':
    unittest.main()
