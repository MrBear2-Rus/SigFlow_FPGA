"""Fixture HTTP integration of late anchors, pending Jobs and event recovery."""
from dataclasses import asdict, replace
import http.client
import json
from pathlib import Path
import tempfile
from threading import Thread
import time
import unittest
from unittest.mock import patch

import test_host_verification as fixture_module
from _model_isolation import isolate_model_settings
from sigflow_edu_agent.checkers.plan import DetailedPlan, PlanStep
from sigflow_edu_agent.clients.gateway import GatewayConfig
from sigflow_edu_agent.clients.gateway import GatewayError
from sigflow_edu_agent.context.models import EvidenceAnchor
from sigflow_edu_agent.contracts.plan import build_plan_card
from sigflow_edu_agent.host.factory import make_factory, make_report_resolver
from sigflow_edu_agent.host.http import HostServer
from sigflow_edu_agent.host.runtime import Runtime
from sigflow_edu_agent.host.store import DurableStore


class CoupledHostTests(unittest.TestCase):
    def setUp(self):
        isolate_model_settings(self)
        from sigflow_edu_agent.host.catalog import AnchorCatalog
        self.g = fixture_module.VerificationHTTPFixture()
        self.g.setUp()
        self.addCleanup(self.g.doCleanups)
        self.g.adapter.wait_timeout=.3  # Allow Windows fixture HTTP scheduling under combined suites.
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        directory = Path(self.temp.name)
        self.path = directory / 'anchors.json'
        self.seed = EvidenceAnchor('ev-seed', 'project-a', 'rev-a', 'seed-job', 'snap-a', 'artifact-seed-job', 'a'*64)
        self.write_anchors([self.seed])
        self.catalog = AnchorCatalog(self.path)
        self.g.plan = DetailedPlan('project-a', 'rev-a', 'snap-a', 'Observe synthesis',
            (PlanStep('synth', 'eda.synth', 'yosys', {'top':'counter'}),), ('inspect report',))
        self.g.package = build_plan_card(self.g.plan, 'plan-1', 0)
        self.g.grant.update(steps=self.g.package.card['steps'])
        self.g.jobs['seed-job'] = dict(expected_revision='rev-a', snapshot_id='snap-a',
                                     capability='eda.synth', plugin_id='eda-synth-yosys')
        self.g.report_overrides['seed-job'] = self.report('ev-seed')
        self.g.job_state = 'Running'
        config = GatewayConfig(self.g.server.server_port, 'token-secret', 'inst-a', timeout=.3)
        self.g.adapter.report_resolver = make_report_resolver(config, self.catalog, evidence_origin='fixture')
        self.store = DurableStore(directory / 'state.sqlite', 'inst-a')
        self.addCleanup(self.store.close)
        factory = make_factory(config, anchors=self.catalog, evidence_origin='fixture', verification=self.g.adapter)
        self.runtime = Runtime(self.store, 'inst-a', factory, evidence_origin='fixture')
        self.addCleanup(self.runtime.close)
        self.server = HostServer(self.runtime, 'ui-fixture')
        self.thread = Thread(target=self.server.serve_forever, kwargs={'poll_interval':.01}, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop)
        self.body = dict(learner_id='learner', project_id='project-a', revision='rev-a', snapshot_id='snap-a',
            job_id='seed-job', diagnostic_id='diag-1', issue_id='issue-1', question='解释综合观察', mode='rule',
            intent='verify', gateway_plan_hash='ui-approved-hash', plan=dict(project_id='project-a', revision='rev-a',
                snapshot_id='snap-a', goal='Observe synthesis', steps=[dict(step_id='synth', capability='eda.synth',
                    provider='yosys', params={'top':'counter'})], expected_observations=['inspect report']))

    @staticmethod
    def report(evidence_id):
        return dict(state='Succeeded', input_fingerprint='fixture-input', diagnostics=[dict(id='diag-1',
            code='EDU_LATCH_INFERRED', summary='Fixture storage observation', stage='synth', origin='core',
            severity='warning', confidence_kind='tool', evidence_refs=[evidence_id])])

    def write_anchors(self, anchors):
        raw = json.dumps({'anchors':[asdict(a) for a in anchors]})
        temporary = self.path.with_suffix('.next')
        temporary.write_text(raw, encoding='utf-8')
        temporary.replace(self.path)

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)

    def request(self, method, path, body=None, **extra):
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=3)
        headers = {'Authorization':'Bearer ui-fixture','X-Learner-ID':'learner',**extra}
        if body is not None:
            body = json.dumps(body).encode('utf-8')
            headers['Content-Type'] = 'application/json'
        try:
            connection.request(method, path, body, headers)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def create(self):
        code, response = self.request('POST','/api/v1/teaching/sessions',self.body, **{'Idempotency-Key':'create-1'})
        self.assertEqual(code,202,response)
        return response['data']['session_id']

    def wait(self, sid, status, reason=None, *, after=None):
        deadline = time.monotonic()+3
        while time.monotonic()<deadline:
            code, response = self.request('GET',f'/api/v1/teaching/sessions/{sid}/state')
            self.assertEqual(code,200,response)
            state = response['data']
            if (state['status']==status and (reason is None or state['reason']==reason)
                    and (after is None or int(state['state_version'])>int(after))):
                return state
            time.sleep(.01)
        self.fail(f'host did not reach {status}/{reason}: {state}')

    def action(self, sid, kind, action_id, state, **extra):
        code, response = self.request('POST',f'/api/v1/teaching/sessions/{sid}/actions',dict(kind=kind,
            action_id=action_id,expected_version=state['state_version'],**extra))
        self.assertEqual(code,202,response)
        return response

    def pending(self):
        sid = self.create()
        waiting = self.wait(sid,'WaitingApproval')
        self.action(sid,'execute_plan','execute',waiting,grant_ref='grant-secret')
        return sid,self.wait(sid,'RecoveryRequired','verification_status_pending')

    def test_late_result_anchor_reaches_feedback_through_get_only_observation(self):
        sid,pending = self.pending()
        self.assertEqual((pending['verification']['job_id'],pending['verification']['job_state']),('job-1','Running'))
        self.g.job_state = 'Succeeded'
        self.g.report_overrides['job-1'] = self.report('ev-result')
        self.g.grant['status'] = 'revoked'
        self.action(sid,'observe_verification','no-anchor',pending)
        no_anchor = self.wait(sid,'RecoveryRequired','verification_evidence_pending')
        self.assertEqual(no_anchor['cards'],[])
        result_anchor = replace(self.seed,evidence_id='ev-result',job_id='job-1',artifact_id='artifact-job-1')
        self.write_anchors([self.seed,result_anchor])
        self.action(sid,'observe_verification','with-anchor',no_anchor)
        completed = self.wait(sid,'Completed')
        self.assertEqual(completed['reason'],'outcome_indeterminate')
        self.assertEqual(completed['cards'][-1]['evidence_ids'],['ev-result'])
        self.assertEqual(len(self.g.posts),1)
        self.assertEqual(completed['model_calls'],0)
        self.assertTrue(completed['verification']['result_evidence_ready'])
        self.assertFalse(completed['verification']['can_observe'])
        self.assertTrue(any(t['stage']=='E7' for t in completed['trace']))

    def test_observe_rejects_grant_injection_and_cross_learner_before_mutation(self):
        sid,pending = self.pending()
        body=dict(kind='observe_verification',action_id='bad',expected_version=pending['state_version'],grant_ref='grant-secret')
        path=f'/api/v1/teaching/sessions/{sid}/actions'
        self.assertEqual(self.request('POST',path,body)[0],400)
        body.pop('grant_ref')
        self.assertEqual(self.request('POST',path,body,**{'X-Learner-ID':'other'})[0],403)
        self.assertEqual(self.wait(sid,'RecoveryRequired')['state_version'],pending['state_version'])
        self.assertEqual(len(self.g.posts),1)

    def test_temporary_observation_gateway_failure_can_be_explicitly_retried(self):
        sid,pending=self.pending()
        original=self.g.adapter._client.health
        def unavailable():
            raise GatewayError('SERVICE_UNAVAILABLE')
        self.g.adapter._client.health=unavailable
        self.action(sid,'observe_verification','offline',pending)
        offline=self.wait(sid,'RecoveryRequired')
        deadline=time.monotonic()+3
        while offline['state_version']==pending['state_version'] and time.monotonic()<deadline:
            time.sleep(.01)
            offline=self.wait(sid,'RecoveryRequired')
        self.assertGreater(int(offline['state_version']),int(pending['state_version']))
        self.assertEqual(offline['verification']['job_id'],'job-1')
        self.assertEqual(offline['verification']['job_state'],'Unknown')
        self.g.adapter._client.health=original
        self.action(sid,'observe_verification','online',offline)
        deadline=time.monotonic()+3
        while time.monotonic()<deadline:
            online=self.wait(sid,'RecoveryRequired')
            if int(online['state_version'])>int(offline['state_version']):
                break
            time.sleep(.01)
        self.assertEqual(online['verification']['job_state'],'Running')
        self.assertEqual(len(self.g.posts),1)

    def test_baseline_gateway_outage_does_not_permanently_end_readonly_continuation(self):
        sid,pending=self.pending()
        source=self.runtime._live[sid][0]._loop.source.source
        original=source.client.health
        def unavailable():
            raise GatewayError('SERVICE_UNAVAILABLE')
        source.client.health=unavailable
        self.action(sid,'observe_verification','baseline-offline',pending)
        offline=self.wait(sid,'RecoveryRequired',after=pending['state_version'])
        self.assertTrue(offline['verification']['can_observe'])
        self.assertEqual(offline['verification']['job_id'],'job-1')
        source.client.health=original
        self.action(sid,'observe_verification','baseline-online',offline)
        online=self.wait(sid,'RecoveryRequired',after=offline['state_version'])
        self.assertEqual(online['verification']['job_state'],'Running')
        self.assertEqual(len(self.g.posts),1)

    def test_trimmed_cursor_recovers_from_atomic_state_watermark(self):
        self.body.pop('plan')
        self.body.pop('gateway_plan_hash')
        self.body['intent']='diagnose'
        with patch('sigflow_edu_agent.host.store.MAX_EVENTS',1):
            sid=self.create()
            state=self.wait(sid,'WaitingStudent')
            self.action(sid,'hint_next','hint-1',state)
            deadline=time.monotonic()+3
            while time.monotonic()<deadline:
                state=self.wait(sid,'WaitingStudent')
                if state['level']==2:
                    break
                time.sleep(.01)
            self.assertEqual(state['level'],2)
            self.assertEqual(self.request('GET',f'/api/v1/teaching/sessions/{sid}/events?after=0&limit=1')[0],410)
            watermark=state['event_high_watermark']
            self.assertIsInstance(watermark,str)
            code,response=self.request('GET',f'/api/v1/teaching/sessions/{sid}/events?after={watermark}&limit=1')
            self.assertEqual((code,response['data']['events']),(200,[]))
            self.assertEqual(response['data']['high_watermark'],int(watermark))
