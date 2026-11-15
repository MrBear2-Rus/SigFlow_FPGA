"""U1 auth/transport and U2 workflow use one actual HTTP server."""
import http.client
import json
from threading import Thread
import unittest
import tests.test_teaching_service as fixture

from sigflow_edu_agent.http_server import EduAgentState,ServerConfig,create_server


class TeachingHTTPTests(unittest.TestCase):
    open=fixture.TeachingServiceTests.open
    close=fixture.TeachingServiceTests.close
    wait=fixture.TeachingServiceTests.wait

    def setUp(self):
        self.assertIn('teaching',ServerConfig.__dataclass_fields__,'U1 teaching transport not registered')
        fixture.TeachingServiceTests.setUp(self)
        self.server=create_server(EduAgentState(ServerConfig(instance_id='instance',ui_token='u1-ui',
            secrets=('u1-ui',),teaching=self.service)))
        self.thread=Thread(target=self.server.serve_forever,kwargs={'poll_interval':.01},daemon=True)
        self.thread.start()
        self.addCleanup(self.stop)

    def stop(self):
        self.server.shutdown();self.server.server_close();self.thread.join(2)

    def request(self,method,path,body=None,*,learner='learner',token='u1-ui',extra=None,key=None):
        conn=http.client.HTTPConnection('127.0.0.1',self.server.server_port,timeout=5)
        headers={'Content-Type':'application/json'}
        if token is not None:headers['Authorization']='Bearer '+token
        if learner is not None:headers['X-Learner-ID']=learner
        if key:headers['Idempotency-Key']=key
        headers.update(extra or {})
        try:
            conn.request(method,path,None if body is None else json.dumps(body),headers)
            response=conn.getresponse()
            return response.status,json.loads(response.read())
        finally:conn.close()

    def create(self):
        code,reply=self.request('POST','/api/v1/u2/sessions',dict(project_id='project',revision='revision'),key='session')
        self.assertEqual(code,201,reply)
        sid=reply['data']['session_id']
        code,reply=self.request('POST',f'/api/v1/u2/sessions/{sid}/runs',self.body,key='run')
        self.assertEqual(code,202,reply)
        return sid,reply['data']['run_id']

    def test_same_port_extension_discovery_pending_actions_events_and_terminal(self):
        code,caps=self.request('GET','/api/v1/capabilities')
        self.assertEqual(code,200)
        self.assertEqual(caps['data']['teaching']['schema_version'],'u1.u2-teaching.v1')
        self.assertFalse(caps['data']['teaching']['http_l4_available'])
        sid,rid=self.create();view=self.wait(rid,'WaitingStudent')
        code,read=self.request('GET',f'/api/v1/u2/runs/{rid}')
        self.assertEqual((code,read['data']['run']),(200,None))
        from sigflow_edu_agent.contract_schema import ContractSchemaValidator
        validator=ContractSchemaValidator(fixture.ROOT/'contracts/edu-agent/v1/schemas')
        validator.validate(read['data']['cards'][0],'teaching-card.schema.json')
        action=dict(kind='hint_next',action_id='hint',expected_version=view['state_version'])
        self.assertEqual(self.request('POST',f'/api/v1/u2/runs/{rid}/actions',action)[0],202)
        second=self.wait(rid,'WaitingStudent',view['state_version'])
        self.assertEqual(self.request('POST',f'/api/v1/u2/runs/{rid}/actions',action)[0],202)
        self.assertEqual(self.service.project(rid,'learner')['state_version'],second['state_version'])
        finish=dict(kind='finish',action_id='finish',expected_version=second['state_version'])
        self.assertEqual(self.request('POST',f'/api/v1/u2/runs/{rid}/actions',finish)[0],202)
        final=self.wait(rid,'Completed')
        validator.validate(final['run'],'agent-run.schema.json')
        code,events=self.request('GET',f'/api/v1/u2/runs/{rid}/events?after=0&limit=100')
        self.assertEqual(code,200)
        self.assertEqual(events['data']['run_id'],rid)
        self.assertNotIn('teaching_session_id',json.dumps(events))
        self.assertEqual(self.request('GET',f'/api/v1/u2/runs/{rid}/events?after=0&after=1')[0],400)
        self.assertEqual(self.request('GET',f'/api/v1/u2/runs/{rid}/events?after=999999')[0],410)
        self.assertEqual(self.request('POST','/api/v1/sessions',dict(project_id='legacy',revision='r'))[0],201)

    def test_auth_owner_origin_and_scope_are_checked_before_mutation(self):
        sid,rid=self.create();view=self.wait(rid,'WaitingStudent')
        path=f'/api/v1/u2/runs/{rid}/actions'
        action=dict(kind='hint_next',action_id='hint',expected_version=view['state_version'])
        for options,status in ((dict(token=None),401),(dict(learner=None),400),
                (dict(learner='foreign'),403),(dict(extra={'Origin':'https://evil.example'}),403),
                (dict(extra={'Host':'evil.example'}),403)):
            with self.subTest(options=options):self.assertEqual(self.request('POST',path,action,**options)[0],status)
        self.assertEqual(self.request('POST',path,action|dict(project_id='foreign'))[0],400)
        self.assertEqual(self.request('POST',path,action|dict(kind='reference_confirm'))[0],403)
        self.assertEqual(self.request('POST',f'/api/v1/u2/sessions/{sid}/runs',self.body)[0],400)
        self.assertEqual(self.service.project(rid,'learner')['state_version'],view['state_version'])
        self.assertEqual(self.request('POST',f'/api/v1/u2/runs/{rid}/cancel',{})[0],202)

    def test_timeline_returns_committed_facts_with_owner_and_cursor_isolation(self):
        _, rid = self.create()
        first = self.wait(rid, 'WaitingStudent')
        path = f'/api/v1/u2/runs/{rid}'
        action = dict(kind='hint_next', action_id='next-hint', expected_version=first['state_version'])
        self.assertEqual(self.request('POST', path + '/actions', action)[0], 202)
        self.wait(rid, 'WaitingStudent', first['state_version'])
        self.assertEqual(self.request('POST', path + '/actions', action)[0], 202)

        code, reply = self.request('GET', path + '/timeline?limit=1')
        self.assertEqual(code, 200, reply)
        latest, = reply['data']['events']
        self.assertEqual(reply['data']['run_id'], rid)
        self.assertTrue(reply['data']['older_records_omitted'])
        self.assertEqual(latest['fact']['action_kind'], 'hint_next')
        self.assertEqual(latest['fact']['action_phase'], 'committed')
        self.assertEqual(latest['fact']['revision'], 'revision')
        self.assertEqual(latest['fact']['hint_level'], 2)
        self.assertNotIn('teaching_session_id', json.dumps(reply))
        self.assertNotIn(self.body['question'], json.dumps(reply))
        code, reply = self.request('GET', path + '/timeline?after=0&limit=1')
        self.assertEqual(code, 200, reply)
        self.assertFalse(reply['data']['older_records_omitted'])
        earlier, = reply['data']['events']
        code, reply = self.request('GET', path + '/timeline?after=' + earlier['sequence'] + '&limit=100')
        self.assertEqual(code, 200, reply)
        self.assertEqual(reply['data']['events'], [latest])
        self.assertEqual(self.request('GET', path + '/timeline', learner='foreign')[0], 403)
        self.assertEqual(self.request('GET', path + '/timeline', token=None)[0], 401)
        self.assertEqual(self.request('GET', path + '/timeline?after=999999')[0], 410)
        self.assertEqual(self.request('GET', path + '/events')[0], 200)

    def test_records_route_keeps_auth_scope_and_explicit_disabled_state(self):
        _, rid = self.create()
        self.wait(rid, 'WaitingStudent')
        path = f'/api/v1/u2/runs/{rid}/records'
        code, reply = self.request('GET', path)
        self.assertEqual(code, 200, reply)
        self.assertEqual(reply['data']['status'], 'disabled')
        self.assertEqual(reply['data']['records'], [])
        from sigflow_edu_agent.contract_schema import ContractSchemaValidator
        ContractSchemaValidator(fixture.ROOT/'contracts/edu-agent/v1/schemas').validate(
            reply['data'], 'u2-learning-records.schema.json')
        self.assertEqual(self.request('GET', path, learner='foreign')[0], 403)
        self.assertEqual(self.request('GET', path, token=None)[0], 401)
        self.assertEqual(self.request('POST', path, {})[0], 404)
        self.current = False
        self.assertEqual(self.request('GET', path)[0], 409)

    def test_disabled_extension_is_404_and_does_not_import_a_second_server(self):
        state=EduAgentState(ServerConfig(instance_id='disabled',ui_token='u1-ui'))
        server=create_server(state)
        thread=Thread(target=server.serve_forever,kwargs={'poll_interval':.01},daemon=True);thread.start()
        original=self.server;self.server=server
        try:self.assertEqual(self.request('POST','/api/v1/u2/sessions',{})[0],404)
        finally:
            self.server=original;server.shutdown();server.server_close();thread.join(2)

    def test_extension_projection_uses_latest_u1_privacy_failure_envelope(self):
        from unittest.mock import patch
        from sigflow_edu_agent.errors import validate_response_envelope
        # Exercise the final boundary even if an optional adapter fails to
        # sanitize one of the path forms added by the latest U1 baseline.
        for unsafe in ('~/private.v', '~\\private.v', '/proc/private.v', '/etc/private.v', 'u1-ui'):
            with self.subTest(unsafe=unsafe), patch.object(self.service, 'project',
                    return_value={'cards': [{'content': unsafe}]}):
                code, reply = self.request('GET', '/api/v1/u2/runs/run-1')
                self.assertEqual(code, 503)
                validate_response_envelope(reply, code)
                self.assertEqual(reply['error']['code'], 'SERVICE_UNAVAILABLE')
                self.assertNotIn(unsafe, json.dumps(reply))
                self.assertNotIn('data', reply)
