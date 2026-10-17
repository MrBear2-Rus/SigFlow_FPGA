"""Read-only continuation regressions; all HTTP/EDA observations are fixtures."""
from dataclasses import replace
import unittest
from threading import Event
import hashlib
import json

import test_host_verification as gateway_fixture
import test_host_runtime as runtime_fixture
from sigflow_edu_agent.clients.gateway import GatewayError
from sigflow_edu_agent.contracts.plan import build_plan_card
from sigflow_edu_agent.domain import VerificationResult


class GatewayObservationTests(unittest.TestCase):
    def setUp(self):
        self.fixture = gateway_fixture.VerificationHTTPFixture()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

    def observe(self, index=0, key='action-build'):
        f = self.fixture
        return f.adapter.observe(f.plan, index, key)

    def test_pending_job_can_be_observed_after_grant_expires_without_post(self):
        f = self.fixture
        f.job_state = 'Running'
        f.execute()
        f.grant['status'] = 'revoked'
        f.job_state = 'Succeeded'
        before = len(f.gets)
        result = self.observe()
        self.assertEqual((result.state, result.job_id), ('Succeeded', 'job-1'))
        self.assertIsNotNone(result.evidence)
        self.assertEqual(len(f.posts), 1)
        self.assertFalse(any('/grants/' in path for path, _ in f.gets[before:]))

    def test_no_reservation_is_denied_without_creating_a_job(self):
        with self.assertRaises(GatewayError):
            self.observe()
        self.assertEqual(self.fixture.posts, [])

    def test_unknown_submission_never_guesses_or_reposts(self):
        f = self.fixture
        f.post_mode = 'drop'
        f.execute()
        result = self.observe()
        self.assertEqual(result.state, 'Unknown')
        self.assertIsNone(result.job_id)
        self.assertEqual(len(f.posts), 1)

    def test_other_approval_step_or_plan_cannot_observe_original_record(self):
        f = self.fixture
        f.execute()
        for index, key in ((1, 'action-build'), (0, 'not-the-key'), (True, 'action-build')):
            with self.assertRaises(GatewayError):
                self.observe(index, key)
        f.adapter.register_approval(f.plan, f.package, 'other-approval')
        with self.assertRaises(GatewayError):
            f.adapter.bind(f.plan, 'other-approval').observe(f.plan, 0, 'action-build')
        foreign = replace(f.plan, goal='Another goal')
        f.adapter.register_approval(foreign, build_plan_card(foreign, 'plan-2', 0), 'ui-approved-hash')
        with self.assertRaises(GatewayError):
            f.adapter.observe(foreign, 0, 'action-build')
        self.assertEqual(len(f.posts), 1)

    def test_same_hash_different_approval_package_cannot_observe_or_cancel_old_job(self):
        f=self.fixture
        f.job_state='Running'
        f.execute()
        package=build_plan_card(f.plan,'plan-2',19)
        f.adapter.register_approval(f.plan,package,'ui-approved-hash')
        other=f.adapter.bind(f.plan,'ui-approved-hash')
        with self.assertRaises(GatewayError):
            other.observe(f.plan,0,'action-build')
        self.assertEqual(other.cancel_owned(f.plan),())
        self.assertFalse(f.cancel_posts)

    def test_transient_health_failure_keeps_the_known_job_observable(self):
        f=self.fixture
        f.job_state='Running'
        f.execute()
        original=f.adapter._client.health
        def unavailable():
            raise GatewayError('SERVICE_UNAVAILABLE')
        f.adapter._client.health=unavailable
        result=self.observe()
        self.assertEqual((result.state,result.job_id),('Unknown','job-1'))
        f.adapter._client.health=original
        f.job_state='Succeeded'
        self.assertEqual(self.observe().state,'Succeeded')
        self.assertEqual(len(f.posts),1)

    def test_legacy_receipt_needs_original_request_proof_before_readonly_observation(self):
        f=self.fixture
        f.execute()
        with f.adapter._db:
            f.adapter._db.execute("UPDATE verification_requests SET approval_digest=''")
        with self.assertRaises(GatewayError):
            self.observe()
        self.assertEqual(f.execute().state,'Succeeded')
        self.assertEqual(self.observe().state,'Succeeded')
        self.assertEqual(len(f.posts),1)

    def test_legacy_persistent_cancel_fence_is_not_bypassed_by_new_digest_scope(self):
        f=self.fixture
        material=['inst-a',f.plan.fingerprint,'ui-approved-hash']
        old_digest=hashlib.sha256(json.dumps(material,sort_keys=True,separators=(',',':'),
                                            ensure_ascii=True).encode()).hexdigest()
        with f.adapter._db:
            f.adapter._db.execute('INSERT INTO cancelled_plans VALUES (?)',(old_digest,))
        with self.assertRaises(GatewayError):
            f.execute()
        self.assertFalse(f.posts)

    def test_health_outage_cannot_erase_a_terminal_job_failure_or_enable_successor(self):
        f=self.fixture
        for state in ('Failed','Cancelled','TimedOut','Rejected'):
            with self.subTest(state=state):
                f.adapter.close()
                f.journal=__import__('pathlib').Path(f.temp.name)/(state+'-observe.sqlite')
                f.adapter=f.create_adapter()
                self.addCleanup(f.adapter.close)
                f.job_state=state
                self.assertEqual(f.execute().state,state)
                original=f.adapter._client.health
                def unavailable():
                    raise GatewayError('SERVICE_UNAVAILABLE')
                f.adapter._client.health=unavailable
                self.assertEqual(self.observe().state,state)
                f.adapter._client.health=original
                f.job_state='Succeeded'
                self.assertEqual(self.observe().state,state)
                with self.assertRaises(GatewayError):
                    f.execute(1,'after-terminal')
        self.assertEqual(len(f.posts),4)

    def test_job_success_without_anchor_stays_an_uncertified_observation(self):
        f = self.fixture
        f.execute()
        f.adapter.report_resolver = lambda *a, **k: None
        result = self.observe()
        self.assertEqual((result.state, result.job_id), ('Succeeded', 'job-1'))
        self.assertIsNone(result.evidence)
        self.assertIsNone(result.matches_expected)

    def test_observation_after_cancel_cannot_post_or_clear_the_plan_fence(self):
        f = self.fixture
        f.job_state = 'Running'
        f.execute()
        f.adapter.cancel_owned(f.plan)
        f.job_state = 'Cancelled'
        result = self.observe()
        self.assertEqual((result.state, result.job_id), ('Cancelled', 'job-1'))
        self.assertEqual(f.cancel_posts, ['job-1'])
        with self.assertRaises(GatewayError):
            f.execute(1, 'after-cancel')
        self.assertEqual(len(f.posts), 1)


class RuntimeObservationTests(unittest.TestCase):
    def setUp(self):
        self.f = runtime_fixture.HostRuntimeTests()
        self.f.setUp()
        self.addCleanup(self.f.doCleanups)
        self.f.verification_setup('Running')
        self.executed, self.observed = [], []
        original = self.f.verification.execute
        self.f.verification.execute = lambda *a: self.executed.append(a) or original(*a)
        self.result = 'Succeeded'
        def observe(plan, index, key):
            self.observed.append((plan.fingerprint, index, key))
            if self.result != 'Succeeded':
                return VerificationResult(self.result, None, None)
            evidence = self.f.source.collect('p', 'r')
            evidence = replace(evidence, evidence_id=f'observed-{index}', job_id=f'observed-job-{index}',
                anchor=replace(evidence.anchor, evidence_id=f'observed-{index}', job_id=f'observed-job-{index}'))
            return VerificationResult('Succeeded', evidence, None)
        self.f.verification.observe = observe

    def pending(self):
        f = self.f
        sid = f.create(intent='verify', plan=f.plan_body, gateway_plan_hash='ui-hash')
        state = f.wait(sid)
        f.runtime.action(sid, 'learner', 'execute_plan', 'execute', int(state['state_version']), grant_ref='fixture-grant')
        return sid, f.wait(sid, 'execute')

    def test_explicit_observation_finishes_pending_job_without_execution_or_model(self):
        f = self.f
        sid, pending = self.pending()
        self.assertEqual(pending['status'], 'RecoveryRequired')
        self.assertTrue(pending['verification']['can_observe'])
        f.verification.authorized = lambda *a: False
        f.runtime.action(sid, 'learner', 'observe_verification', 'observe-1', int(pending['state_version']))
        result = f.wait(sid, 'observe-1')
        self.assertEqual((result['status'], result['reason']), ('Completed', 'outcome_indeterminate'))
        self.assertEqual((len(self.executed), len(self.observed)), (1, 1))
        self.assertEqual(result['model_calls'], 0)
        replay = f.runtime.action(sid, 'learner', 'observe_verification', 'observe-1', int(pending['state_version']))
        self.assertEqual(replay['result']['status'], 'Completed')
        self.assertEqual(len(self.observed), 1)
        self.assertTrue(result['trace'])

    def test_nonterminal_observation_keeps_cards_quarantined_and_versioned(self):
        f = self.f
        sid, pending = self.pending()
        self.result = 'Running'
        f.runtime.action(sid, 'learner', 'observe_verification', 'observe-1', int(pending['state_version']))
        state = f.wait(sid, 'observe-1')
        self.assertEqual((state['status'], state['cards']), ('RecoveryRequired', []))
        self.assertGreater(int(state['state_version']), int(pending['state_version']))
        with self.assertRaises(ValueError):
            f.runtime.action(sid, 'learner', 'observe_verification', 'stale', int(pending['state_version']))
        self.assertEqual(len(self.executed), 1)

    def test_success_without_result_evidence_can_wait_then_observe(self):
        f = self.f
        f.verification.execute = lambda *a: self.executed.append(a) or VerificationResult('Succeeded', None, None)
        sid, pending = self.pending()
        self.assertEqual((pending['status'], pending['reason']), ('RecoveryRequired', 'verification_evidence_pending'))
        f.runtime.action(sid, 'learner', 'observe_verification', 'with-evidence', int(pending['state_version']))
        self.assertEqual(f.wait(sid, 'with-evidence')['status'], 'Completed')
        self.assertEqual(len(self.executed), 1)

    def test_observation_cannot_automatically_execute_a_successor_step(self):
        f = self.f
        f.plan_body['steps'].append(dict(step_id='s2', capability='eda.synth', provider='yosys', params={'top':'other'}))
        f.plan_body['max_jobs'] = 2
        sid, pending = self.pending()
        f.runtime.action(sid, 'learner', 'observe_verification', 'first-result', int(pending['state_version']))
        state = f.wait(sid, 'first-result')
        self.assertEqual((state['status'], state['stage']), ('WaitingApproval', 'E5'))
        self.assertEqual(len(self.executed), 1)
        self.assertEqual(state['verification']['completed_steps'], 1)
        f.runtime.action(sid, 'learner', 'execute_plan', 'approve-next', int(state['state_version']), grant_ref='fixture-grant')
        self.assertEqual(f.wait(sid, 'approve-next')['status'], 'RecoveryRequired')
        self.assertEqual(len(self.executed), 2)

    def test_cancel_and_restart_never_enable_old_execution_observation(self):
        f = self.f
        sid, pending = self.pending()
        f.restart()
        self.assertFalse(f.runtime.snapshot(sid, 'learner')['verification']['can_observe'])
        with self.assertRaises(ValueError):
            f.runtime.action(sid, 'learner', 'observe_verification', 'after-restart', int(pending['state_version']))
        f.runtime.cancel(sid, 'learner')
        with self.assertRaises(ValueError):
            f.runtime.action(sid, 'learner', 'observe_verification', 'after-cancel', int(f.runtime.snapshot(sid,'learner')['state_version']))
        self.assertEqual((len(self.executed), len(self.observed)), (1, 0))

    def test_legacy_port_without_observe_does_not_advertise_continuation(self):
        del self.f.verification.observe
        _, pending = self.pending()
        self.assertFalse(pending['verification']['can_observe'])

    def test_action_cancel_is_available_for_live_pending_verification(self):
        f=self.f
        cancelled=Event()
        f.verification.cancel_owned=lambda p: cancelled.set()
        sid,pending=self.pending()
        f.runtime.action(sid,'learner','cancel','cancel-pending',int(pending['state_version']))
        self.assertEqual(f.wait(sid,'cancel-pending')['status'],'Cancelled')
        self.assertTrue(cancelled.wait(1))

    def test_shutdown_during_e6_collection_cannot_submit_a_new_job(self):
        f = self.f
        sid=f.create(intent='verify',plan=f.plan_body,gateway_plan_hash='ui-hash')
        state=f.wait(sid)
        entered,release=Event(),Event()
        self.addCleanup(release.set)
        original=f.source.collect
        def blocked(*args):
            entered.set()
            release.wait(2)
            return original(*args)
        f.source.collect=blocked
        f.runtime.action(sid,'learner','execute_plan','execute',int(state['state_version']),grant_ref='fixture-grant')
        self.assertTrue(entered.wait(1))
        f.runtime.close(timeout=0)
        release.set()
        f.runtime.close(timeout=2)
        self.assertEqual(self.executed,[])


if __name__ == '__main__':
    unittest.main()
