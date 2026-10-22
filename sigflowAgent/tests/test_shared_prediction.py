"""Formal shared projection includes only actionable, committed prediction metadata."""
import unittest
import test_host_prediction as fixture
from sigflow_edu_agent.host.shared_adapter import SharedRunContext,TeachingHandle,SharedTeachingAdapter
from sigflow_edu_agent.host.gateway_evidence import GatewayEvidenceCatalog


class SharedPredictionTests(unittest.TestCase):
    setUp=fixture.HostPredictionTests.setUp
    open=fixture.HostPredictionTests.open
    close=fixture.HostPredictionTests.close
    wait=fixture.HostPredictionTests.wait
    create=fixture.HostPredictionTests.create
    action=fixture.HostPredictionTests.action
    begin=fixture.HostPredictionTests.begin

    def test_shared_client_can_receive_challenge_then_compare_without_private_state(self):
        sid,state=self.begin()
        context=SharedRunContext('sess-1','run-1','inst-wave','learner1','p1','r1','snap1','job1','job-report','issue1')
        handle=TeachingHandle(context,sid,'report_review')
        catalog=GatewayEvidenceCatalog(self.g.config)
        adapter=SharedTeachingAdapter(self.runtime,lambda eid:catalog.collect('job1',project_id='p1',
            revision='r1',snapshot_id='snap1'))
        view=adapter.project(handle)
        self.assertEqual(view['prediction']['challenge_id'],state['prediction']['challenge_id'])
        self.assertNotIn('observed',view['prediction'])
        adapter.action(handle,dict(kind='prediction_submit',action_id='submit',expected_state_version=view['state_version'],
            prediction=dict(challenge_id=view['prediction']['challenge_id'],expected='1')))
        self.wait(sid,'submit')
        self.assertEqual(adapter.project(handle)['prediction']['outcome'],'matches')
        adapter.cancel(handle)
        self.assertNotIn('observed',adapter.project(handle)['prediction'])

    def shared(self,sid,*,secrets=(),resolve=None):
        context=SharedRunContext('sess-1','run-1','inst-wave','learner1','p1','r1','snap1','job1','job-report','issue1')
        catalog=GatewayEvidenceCatalog(self.g.config)
        resolver=resolve or (lambda eid:catalog.collect('job1',project_id='p1',revision='r1',snapshot_id='snap1'))
        return SharedTeachingAdapter(self.runtime,resolver,secrets=secrets),TeachingHandle(context,sid,'report_review')

    def test_shared_request_and_replay_ack_never_publish_private_runtime_state(self):
        import json
        sid,state=self.create();adapter,handle=self.shared(sid)
        payload=dict(kind='prediction_request',action_id='request',expected_state_version=state['state_version'],prediction=self.payload)
        accepted=adapter.action(handle,payload)
        self.assertTrue(accepted['pending'])
        self.wait(sid,'request')
        view=adapter.project(handle)
        self.assertEqual(view['prediction']['status'],'awaiting_prediction')
        self.assertEqual(self.g.query_count,0)
        self.assertIsNone(view['run'])
        self.assertEqual(view['phase'],'pending')
        ack=adapter.action(handle,payload)
        self.assertTrue(ack['replay'])
        self.assertEqual(set(ack),{'accepted','session_id','run_id','replay','pending'})
        self.assertNotIn(sid,json.dumps(ack))
        self.assertNotIn(sid,json.dumps(view))
        self.assertNotIn('observed',view['prediction'])

    def test_cancel_during_evidence_resolution_rejects_stale_challenge(self):
        sid,state=self.begin();catalog=GatewayEvidenceCatalog(self.g.config)
        def resolve(eid):
            proof=catalog.collect('job1',project_id='p1',revision='r1',snapshot_id='snap1')
            self.runtime.cancel(sid,'learner1')
            return proof
        adapter,handle=self.shared(sid,resolve=resolve)
        with self.assertRaisesRegex(ValueError,'shared_projection_stale'):
            adapter.project(handle)
        self.assertEqual(adapter.project(handle)['prediction'],dict(status='unavailable'))
        self.assertEqual(self.g.query_count,0)

    def test_restart_hides_observation_and_replay_ack_until_explicit_recovery(self):
        sid,state=self.begin();adapter,handle=self.shared(sid)
        payload=dict(kind='prediction_submit',action_id='submit',expected_state_version=state['state_version'],
                     prediction=dict(challenge_id=state['prediction']['challenge_id'],expected='1'))
        adapter.action(handle,payload);self.wait(sid,'submit')
        self.assertEqual(adapter.project(handle)['prediction']['observed'],'1')
        self.close();self.open();adapter,handle=self.shared(sid)
        view=adapter.project(handle)
        self.assertEqual(view['prediction'],dict(status='revalidation_required'))
        self.assertEqual(view['cards'],[])
        self.assertIsNone(view['run'])
        ack=adapter.action(handle,payload)
        self.assertTrue(ack['replay']);self.assertNotIn('result',ack)
        self.assertEqual(self.g.query_count,1)

    def test_prediction_metadata_rejects_secrets_paths_and_unlisted_private_fields(self):
        from copy import deepcopy
        from unittest.mock import patch
        sid,state=self.begin();adapter,handle=self.shared(sid,secrets=('prediction-secret',))
        for field,value,error in (('full_name','prediction-secret','unsafe_shared_output'),
                                  ('full_name','C:/private/source.v','unsafe_shared_output'),
                                  ('raw_runtime',{'session_id':sid},'invalid_shared_prediction')):
            with self.subTest(field=field,value=value):
                changed=deepcopy(state);changed['prediction'][field]=value
                with patch.object(adapter,'_state',return_value=changed):
                    with self.assertRaisesRegex(ValueError,error):adapter.project(handle)
