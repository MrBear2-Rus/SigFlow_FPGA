"""Restart regressions: loopback Gateway fixture, no real EDA execution."""
from dataclasses import replace
import hashlib
import json
import subprocess
import sys
from pathlib import Path
import time
import unittest

import test_host_verification as fixture
from sigflow_edu_agent.host.runtime import Runtime
from sigflow_edu_agent.host.store import DurableStore
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.runtime.session import TeachingSession
from sigflow_edu_agent.contracts.plan import build_plan_card


class VerificationRestartTests(unittest.TestCase):
    def setUp(self):
        self.f = fixture.VerificationHTTPFixture()
        self.f.setUp()
        self.addCleanup(self.f.doCleanups)
        self.path = Path(self.f.temp.name) / 'host.sqlite'
        self.store = DurableStore(self.path, 'inst-a')
        self.baseline = self.f.evidence('baseline', project_id='project-a', revision='rev-a', snapshot_id='snap-a')
        self.collections = 0
        self.provider_calls = []
        self.source_changed = False
        self.runtime = self.open_runtime()
        self.addCleanup(self.shutdown)
        self.body = dict(learner_id='learner', project_id='project-a', revision='rev-a',
            snapshot_id='snap-a', job_id='baseline', diagnostic_id='diag', issue_id='issue',
            question='unique-private-restart-question', intent='verify', mode='rule',
            plan=self.f.package.binding['plan'], gateway_plan_hash='ui-approved-hash')

    def open_runtime(self):
        owner = self
        class Source:
            def collect(inner, project, revision):
                owner.collections += 1
                return replace(owner.baseline, summary='changed') if owner.source_changed else owner.baseline
        def factory(binding, request, mode, reserve, *, plan=None, gateway_plan_hash=None):
            package = build_plan_card(plan, 'plan-1', 0)
            owner.f.adapter.register_approval(plan, package, gateway_plan_hash)
            port = owner.f.adapter.bind(plan, gateway_plan_hash)
            return TeachingSession(TeachingLoop(request, Source(), plan=plan, verification=port,
                                                course_provider=getattr(owner, 'courses', None)))
        return Runtime(self.store, 'inst-a', factory, evidence_origin='fixture')

    def shutdown(self):
        self.runtime.close()
        self.store.close()
        self.f.adapter.close()

    def wait(self, sid, action='create'):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            if self.store.action(sid, action)['status'] in ('committed', 'interrupted'):
                return self.runtime.snapshot(sid, 'learner')
            time.sleep(.01)
        self.fail('worker did not finish')

    def create(self):
        sid = self.runtime.create(self.body, 'create')['session_id']
        return sid, self.wait(sid)

    def pending(self):
        self.f.job_state = 'Running'
        sid, state = self.create()
        self.runtime.action(sid, 'learner', 'execute_plan', 'execute', int(state['state_version']), grant_ref='grant-secret')
        return sid, self.wait(sid, 'execute')

    def restart(self):
        self.runtime.close()
        self.store.close()
        self.f.adapter.close()
        self.f.adapter = self.f.create_adapter()
        self.store = DurableStore(self.path, 'inst-a')
        self.runtime = self.open_runtime()

    def recover(self, sid, action='recover'):
        state = self.runtime.snapshot(sid, 'learner')
        self.runtime.recover(sid, 'learner', action, int(state['state_version']), self.body['question'])
        return self.wait(sid, action)

    def test_reopened_store_reads_pending_receipt_and_requires_fresh_successor_approval(self):
        sid, pending = self.pending()
        self.restart()
        quarantined = self.runtime.snapshot(sid, 'learner')
        self.assertEqual((quarantined['status'], quarantined['cards']), ('RecoveryRequired', []))
        self.assertFalse(quarantined['verification']['can_observe'])
        self.f.job_state = 'Succeeded'
        self.f.adapter.authorized = lambda *a, **kw: self.fail('recovery authorizes execution')
        self.f.adapter.execute = lambda *a, **kw: self.fail('recovery submits execution')
        count = self.collections
        after = self.recover(sid)
        self.assertEqual((after['status'], after['reason']), ('WaitingApproval', 'successor_approval_required'))
        self.assertEqual(after['verification']['completed_steps'], 1)
        self.assertEqual(after['run_id'], pending['run_id'])
        self.assertGreater(self.collections, count)
        self.assertGreater(int(after['state_version']), int(quarantined['state_version']))
        self.assertEqual(len(self.f.posts), 1)
        self.assertEqual(after['model_calls'], 0)
        self.assertNotIn(b'unique-private-restart-question', self.path.read_bytes())
        self.assertNotIn(b'grant-secret', self.path.read_bytes())
        self.assertNotIn(b'token-secret', self.path.read_bytes())

    def test_waiting_unissued_plan_recovers_without_observing_or_executing(self):
        sid, before = self.create()
        self.restart()
        after = self.recover(sid)
        self.assertEqual(after['status'], 'WaitingApproval')
        self.assertEqual(after['plan_fingerprint'], before['plan_fingerprint'])
        self.assertEqual((self.f.posts, self.f.gets), ([], []))

    def test_course_background_retrieved_on_recovery_without_execution_or_model(self):
        from test_courses import Courses, material
        from sigflow_edu_agent.domain import CourseBundle
        self.courses = Courses(CourseBundle('ok', (material('before'),)))
        sid, before = self.create()
        self.assertEqual(len(self.courses.calls), 1)
        self.courses = Courses(CourseBundle('ok', (material('after'),)))
        self.restart()
        self.f.adapter.authorized = lambda *args, **kw: self.fail('recovery authorizes execution')
        self.f.adapter.execute = lambda *args, **kw: self.fail('recovery submits execution')
        after = self.recover(sid)
        self.assertEqual(after['status'], 'WaitingApproval')
        self.assertEqual(after['run_id'], before['run_id'])
        self.assertEqual(after['model_calls'], 0)
        loop = self.runtime._live[sid][0]._loop
        self.assertEqual(loop._state.course_status, 'ok')
        self.assertEqual(loop._course_refs[0].passage_id, 'test:latch:after')
        self.assertEqual((len(self.courses.calls), len(self.f.posts)), (1, 0))
        self.assertNotIn('test:latch:', self.store.get(sid)['checkpoint'])

    def test_question_plan_and_scope_tampering_rejected_before_gateway_reads(self):
        sid, _ = self.pending()
        self.restart()
        state = self.runtime.snapshot(sid, 'learner')
        with self.assertRaises(ValueError):
            self.runtime.recover(sid, 'learner', 'wrong', int(state['state_version']), 'wrong')
        original = self.store.get(sid)['checkpoint']
        self.assertIsNotNone(original)
        count = len(self.f.gets)
        for field in ('plan', 'binding'):
            data = json.loads(original)
            if field == 'plan':
                data[field]['goal'] = 'tampered goal'
            else:
                data[field]['snapshot_id'] = 'other'
            payload = json.dumps(data)
            with self.store._db:
                self.store._db.execute('UPDATE sessions SET checkpoint=?, checkpoint_sha256=? WHERE session_id=?',
                    (payload, hashlib.sha256(payload.encode()).hexdigest(), sid))
            with self.assertRaises(ValueError):
                self.recover(sid, 'bad-' + field)
        self.assertEqual(len(self.f.gets), count)

    def test_unknown_receipt_stays_unknown_after_restart_without_resubmission(self):
        self.f.post_mode = 'drop'
        sid, _ = self.pending()
        self.restart()
        self.f.post_mode = 'normal'
        after = self.recover(sid)
        self.assertEqual((after['status'], after['verification']['job_state']), ('RecoveryRequired', 'Unknown'))
        self.assertIsNone(after['verification']['job_id'])
        self.assertEqual(len(self.f.posts), 1)

    def test_late_result_evidence_can_be_observed_after_second_restart(self):
        sid, _ = self.pending()
        self.restart()
        self.f.job_state = 'Succeeded'
        self.f.adapter.report_resolver = lambda *a, **kw: None
        pending = self.recover(sid)
        self.assertEqual(pending['reason'], 'verification_evidence_pending')
        self.restart()
        after = self.recover(sid, 'recover-late')
        self.assertEqual(after['status'], 'WaitingApproval')
        self.assertEqual(len(self.f.posts), 1)

    def test_fresh_baseline_mismatch_cannot_observe_receipt_and_remains_recoverable(self):
        sid, _ = self.pending()
        self.restart()
        self.source_changed = True
        count = len(self.f.gets)
        after = self.recover(sid)
        self.assertEqual((after['status'], after['reason']), ('RecoveryRequired', 'recovery_evidence_mismatch'))
        self.assertEqual(len(self.f.gets), count)
        self.assertIsNotNone(self.store.get(sid)['checkpoint'])
        self.source_changed = False
        self.f.job_state = 'Succeeded'
        self.assertEqual(self.recover(sid, 'recover-again')['status'], 'WaitingApproval')

    def test_cancel_after_restart_uses_original_exact_approval_binding(self):
        sid, _ = self.pending()
        self.restart()
        # Another live approval must not claim the original receipt.
        self.f.adapter.register_approval(self.f.plan, build_plan_card(self.f.plan, 'plan-2', 19), 'other-hash')
        ack = self.runtime.cancel(sid, 'learner')
        self.assertTrue(ack['eda_cancel_scheduled'])
        self.runtime.close()
        self.assertEqual(self.f.cancel_posts, ['job-1'])
        self.assertEqual(self.runtime.snapshot(sid, 'learner')['status'], 'Cancelled')

    def test_completed_predecessor_is_revalidated_after_another_restart(self):
        sid, _ = self.pending()
        self.restart()
        self.f.job_state = 'Succeeded'
        after = self.recover(sid)
        self.assertEqual(after['status'], 'WaitingApproval')
        self.restart()
        self.f.report_overrides['job-1'] = {'artifacts': []}
        after = self.recover(sid, 'recover-invalid-predecessor')
        self.assertEqual(after['status'], 'RecoveryRequired')
        self.assertEqual(after['cards'], [])
        self.assertEqual(len(self.f.posts), 1)

    def test_process_loss_after_receipt_before_final_commit_observes_original_run(self):
        self.runtime.close()
        self.store.close()
        self.f.job_state = 'Running'
        script = '''
import json, os, sys, threading
from sigflow_edu_agent.clients.gateway import GatewayConfig
from sigflow_edu_agent.host.verification import GatewayVerification
from sigflow_edu_agent.host.runtime import Runtime
from sigflow_edu_agent.host.store import DurableStore
from sigflow_edu_agent.domain import Evidence
from sigflow_edu_agent.context.models import EvidenceAnchor
from sigflow_edu_agent.contracts.plan import build_plan_card
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.runtime.session import TeachingSession
data=json.loads(sys.stdin.read())
raw=data['baseline']; raw['anchor']=EvidenceAnchor(**raw['anchor'])
raw['context_items']=(); raw['omitted']=()
baseline=Evidence(**raw)
class Source:
    def collect(self, project, revision): return baseline
class CrashAfterReceipt(GatewayVerification):
    def execute(self, *args, **kwargs):
        super().execute(*args, **kwargs)
        os._exit(23)
adapter=CrashAfterReceipt(GatewayConfig(data['port'],'token-secret','inst-a',timeout=.3),
    data['ledger'], lambda *a, **kw: None, wait_timeout=.04,poll_interval=.005)
def factory(binding, request, mode, reserve, *, plan=None, gateway_plan_hash=None):
    adapter.register_approval(plan,build_plan_card(plan,'plan-1',0),gateway_plan_hash)
    return TeachingSession(TeachingLoop(request,Source(),plan=plan,
        verification=adapter.bind(plan,gateway_plan_hash)))
store=DurableStore(data['store'],'inst-a')
runtime=Runtime(store,'inst-a',factory,evidence_origin='fixture')
sid=runtime.create(data['body'],'create')['session_id']
import time
for _ in range(200):
    state=runtime.snapshot(sid,'learner')
    if state['status']=='WaitingApproval': break
    time.sleep(.01)
runtime.action(sid,'learner','execute_plan','execute',int(state['state_version']),grant_ref='grant-secret')
threading.Event().wait(5)
os._exit(24)
'''
        from dataclasses import asdict
        data = dict(baseline=asdict(self.baseline), body=self.body, port=self.f.server.server_port,
                    ledger=str(self.f.journal), store=str(self.path))
        child = subprocess.run([sys.executable, '-c', script], input=json.dumps(data),
                               capture_output=True, text=True, timeout=10)
        self.assertEqual(child.returncode, 23, child.stderr)
        self.store = DurableStore(self.path, 'inst-a')
        self.runtime = self.open_runtime()
        row = self.store.list_sessions()[0]
        sid = row['session_id']
        self.assertEqual(self.store.action(sid, 'execute')['status'], 'interrupted')
        self.assertEqual(row['state']['status'], 'RecoveryRequired')
        self.f.job_state = 'Succeeded'
        after = self.recover(sid)
        self.assertEqual(after['status'], 'WaitingApproval')
        self.assertEqual(len(self.f.posts), 1)

    def test_recovered_approval_metadata_retains_last_completed_index(self):
        sid, _ = self.pending()
        self.restart()
        self.f.job_state = 'Succeeded'
        after = self.recover(sid)
        self.assertEqual(after['verification']['last_observed_step_index'], 0)
        self.restart()
        after = self.recover(sid, 'recover-approval')
        self.assertEqual(after['verification']['last_observed_step_index'], 0)

    def test_restart_cancel_isolates_receipts_and_fences_with_reused_approval(self):
        f = self.f
        f.job_state = 'Running'
        f.grant['grant_id'] = 'other-grant'
        other_run = 'a' * 32
        other_key = f'{other_run}:{f.plan.fingerprint}:0'
        f.adapter.execute(f.plan, 0, 'other-grant', other_key)
        f.grant['grant_id'] = 'grant-secret'
        sid, _ = self.pending()
        self.restart()
        ack = self.runtime.cancel(sid, 'learner')
        self.assertTrue(ack['eda_cancel_scheduled'])
        self.runtime.close()
        self.assertEqual(f.cancel_posts, ['job-2'])
        f.grant['grant_id'] = 'other-grant'
        f.job_state = 'Succeeded'
        other = f.adapter.bind(f.plan, 'ui-approved-hash').bind_run(other_run)
        other.execute(f.plan, 0, 'other-grant', other_key)
        other.execute(f.plan, 1, 'other-grant', f'{other_run}:{f.plan.fingerprint}:1')
        self.assertEqual(len(f.posts), 3, 'other run must remain executable after cancellation')
        f.grant['grant_id'] = 'third-grant'
        third_run = 'b' * 32
        third = f.adapter.bind(f.plan, 'ui-approved-hash').bind_run(third_run)
        third.execute(f.plan, 0, 'third-grant', f'{third_run}:{f.plan.fingerprint}:0')
        self.assertEqual(len(f.posts), 4, 'new run must not inherit another run cancellation fence')

    def test_cancel_after_recovery_keeps_original_run_ownership(self):
        f = self.f
        f.job_state = 'Running'
        f.grant['grant_id'] = 'other-grant'
        f.adapter.execute(f.plan, 0, 'other-grant', 'foreign-key')
        f.grant['grant_id'] = 'grant-secret'
        sid, _ = self.pending()
        self.restart()
        after = self.recover(sid)
        self.assertEqual(after['status'], 'RecoveryRequired')
        self.runtime.cancel(sid, 'learner')
        self.runtime.close()
        self.assertEqual(f.cancel_posts, ['job-2'])


if __name__ == '__main__':
    unittest.main()
