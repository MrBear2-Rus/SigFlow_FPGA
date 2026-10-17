"""Durable prediction workflow with loopback Gateway fixtures, no real EDA/model."""
from pathlib import Path
import tempfile,time,unittest
from unittest.mock import patch
from _wave_gateway import WaveGateway
from _model_isolation import isolate_model_settings
from sigflow_edu_agent.clients.waveform import GatewayWaveformReader
from sigflow_edu_agent.host.gateway_evidence import GatewayEvidenceCatalog
from sigflow_edu_agent.host.factory import make_factory
from sigflow_edu_agent.host.runtime import Runtime
from sigflow_edu_agent.host.store import DurableStore


class HostPredictionTests(unittest.TestCase):
    def setUp(self):
        isolate_model_settings(self)
        self.g=WaveGateway();self.addCleanup(self.g.close)
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name)/'host.sqlite'
        self.open();self.addCleanup(self.close)
        self.body=dict(learner_id='learner1',project_id='p1',revision='r1',snapshot_id='snap1',job_id='job1',
            diagnostic_id='job-report',issue_id='issue1',question='Predict a selected point',mode='rule',intent='diagnose')
        self.payload=dict(artifact_id='wave1',signal_id=0,start_tick='10',end_tick='20',sample_tick='12')

    def open(self):
        self.factory=make_factory(self.g.config,anchors=GatewayEvidenceCatalog(self.g.config),
                                  waveform_reader=GatewayWaveformReader(self.g.config))
        self.store=DurableStore(self.path,'inst-wave');self.runtime=Runtime(self.store,'inst-wave',self.factory)

    def close(self):
        self.runtime.close();self.store.close()

    def wait(self,sid,action):
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            if self.store.action(sid,action)['status']=='committed':return self.runtime.snapshot(sid,'learner1')
            time.sleep(.01)
        self.fail('worker did not commit')

    def create(self):
        sid=self.runtime.create(self.body,'create')['session_id']
        return sid,self.wait(sid,'create')

    def action(self,sid,state,kind,aid,payload):
        self.runtime.action(sid,'learner1',kind,aid,int(state['state_version']),prediction=payload)
        return self.wait(sid,aid)

    def begin(self):
        sid,state=self.create()
        return sid,self.action(sid,state,'prediction_request','predict',self.payload)

    def test_prediction_publishes_finite_result_without_level_or_job_advancement(self):
        sid,state=self.begin();self.assertEqual(self.g.query_count,0)
        result=self.action(sid,state,'prediction_submit','submit',dict(challenge_id=state['prediction']['challenge_id'],expected='1'))
        self.assertEqual((result['status'],result['level'],result['model_calls']),('WaitingStudent',1,0))
        self.assertEqual(result['prediction']['outcome'],'matches')
        self.assertEqual(len(result['cards']),1)
        self.assertEqual(self.g.query_count,1)
        self.assertFalse(any(method=='POST' and '/jobs' in path for method,path,_ in self.g.requests))

    def test_action_replay_does_not_read_again_and_changed_payload_conflicts(self):
        sid,state=self.begin();payload=dict(challenge_id=state['prediction']['challenge_id'],expected='1')
        result=self.action(sid,state,'prediction_submit','submit',payload)
        replay=self.runtime.action(sid,'learner1','prediction_submit','submit',int(state['state_version']),prediction=payload)
        self.assertEqual(replay['result']['state_version'],result['state_version']);self.assertEqual(self.g.query_count,1)
        with self.assertRaisesRegex(ValueError,'action_conflict'):
            self.runtime.action(sid,'learner1','prediction_submit','submit',int(state['state_version']),prediction=payload|dict(expected='0'))

    def test_hint_expires_challenge_and_cross_learner_is_denied(self):
        sid,state=self.begin();challenge=state['prediction']['challenge_id']
        self.runtime.action(sid,'learner1','hint_next','hint',int(state['state_version']))
        hinted=self.wait(sid,'hint');self.assertEqual(hinted['prediction']['status'],'expired')
        with self.assertRaisesRegex(ValueError,'prediction_challenge_stale'):
            self.runtime.action(sid,'learner1','prediction_submit','submit',int(hinted['state_version']),
                                prediction=dict(challenge_id=challenge,expected='1'))
        with self.assertRaisesRegex(ValueError,'scope_denied'):
            self.runtime.action(sid,'other','prediction_request','other',int(hinted['state_version']),prediction=self.payload)
        self.assertEqual(self.g.query_count,0)

    def test_restart_hides_old_result_and_recovery_requires_new_challenge(self):
        sid,state=self.begin();payload=dict(challenge_id=state['prediction']['challenge_id'],expected='1')
        self.action(sid,state,'prediction_submit','submit',payload)
        self.close();self.open()
        quarantined=self.runtime.snapshot(sid,'learner1')
        self.assertEqual(quarantined['prediction'],dict(status='revalidation_required'))
        self.runtime.recover(sid,'learner1','recover',int(quarantined['state_version']),self.body['question'])
        recovered=self.wait(sid,'recover')
        self.assertNotIn('observed',recovered['prediction'])
        replay=self.runtime.action(sid,'learner1','prediction_submit','submit',int(state['state_version']),prediction=payload)
        self.assertNotIn('observed',replay['result']['prediction'])
        self.assertEqual(self.g.query_count,1)

    def test_unavailable_wave_does_not_fail_other_teaching_actions(self):
        sid,state=self.create();self.g.contract=None
        state=self.action(sid,state,'prediction_request','predict',self.payload)
        self.assertEqual((state['status'],state['prediction']['status']),('WaitingStudent','unavailable'))
        self.runtime.action(sid,'learner1','hint_next','hint',int(state['state_version']))
        self.assertEqual(self.wait(sid,'hint')['level'],2)

    def test_challenge_expiring_after_reservation_keeps_teaching_available(self):
        sid,state=self.begin()
        payload=dict(challenge_id=state['prediction']['challenge_id'],expected='1')
        with patch.object(self.runtime,'_start') as start:
            self.runtime.action(sid,'learner1','prediction_submit','expired',int(state['state_version']),prediction=payload)
        self.runtime._live[sid][0]._prediction_controller._deadline=0
        self.runtime._work(*start.call_args.args)
        result=self.wait(sid,'expired')
        self.assertEqual(result['status'],'WaitingStudent')
        self.assertEqual(result['prediction'],dict(status='unavailable',reason='prediction_challenge_stale'))
        self.assertEqual(self.g.query_count,0)

    def test_http_prediction_contract_rejects_untrusted_fields_and_requires_learner(self):
        import http.client,json,threading
        from sigflow_edu_agent.host.http import HostServer
        server=HostServer(self.runtime,'test-ui')
        thread=threading.Thread(target=server.serve_forever,kwargs={'poll_interval':.01},daemon=True)
        thread.start()
        def request(method,path,body=None,learner='learner1'):
            conn=http.client.HTTPConnection('127.0.0.1',server.server_port,timeout=3)
            headers={'Authorization':'Bearer test-ui','X-Learner-ID':learner,'Content-Type':'application/json'}
            conn.request(method,path,None if body is None else json.dumps(body),headers)
            response=conn.getresponse();status=response.status;data=json.loads(response.read());conn.close()
            return status,data
        try:
            caps=request('GET','/api/v1/capabilities')[1]['data']
            self.assertEqual(caps['prediction'],dict(configured=True,requires_explicit_expectation=True,starts_jobs=False))
            sid,state=self.create();path='/api/v1/teaching/sessions/'+sid+'/actions'
            body=dict(kind='prediction_request',action_id='http',expected_version=state['state_version'],prediction=self.payload)
            for injected in ('anchor','origin','service_contract','path','waveform'):
                status,_=request('POST',path,body|dict(prediction=self.payload|{injected:'injected'}))
                self.assertEqual(status,400)
            self.assertEqual(request('POST',path,body,learner='other')[0],403)
            self.assertEqual(request('POST',path,body)[0],202)
            waiting=self.wait(sid,'http');self.assertEqual(self.g.query_count,0)
            body=dict(kind='prediction_submit',action_id='http-submit',expected_version=waiting['state_version'],
                      prediction=dict(challenge_id=waiting['prediction']['challenge_id'],expected='1'))
            self.assertEqual(request('POST',path,body)[0],202)
            self.assertEqual(self.wait(sid,'http-submit')['prediction']['outcome'],'matches')
        finally:
            server.shutdown();server.server_close();thread.join(2)
