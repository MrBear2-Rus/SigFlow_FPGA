"""Real loopback report/host contracts with generated course material."""
from dataclasses import asdict
from copy import deepcopy
import http.client
import json
from pathlib import Path
import tempfile
from threading import Thread
import time
import unittest

import test_gateway_source as fixture
from test_courses import Courses, material
from _model_isolation import isolate_model_settings
from sigflow_edu_agent.clients.gateway import GatewayConfig
from sigflow_edu_agent.domain import CourseBundle
from sigflow_edu_agent.host.factory import make_factory
from sigflow_edu_agent.host.http import HostServer
from sigflow_edu_agent.host.runtime import Runtime
from sigflow_edu_agent.host.store import DurableStore


class HostCoursesTests(unittest.TestCase):
    def setUp(self):
        isolate_model_settings(self)
        self.g = fixture.GatewaySourceHTTPTests(); self.g.setUp()
        self.addCleanup(self.g.doCleanups)
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'host.sqlite'
        self.courses = Courses(CourseBundle('ok', (material('one', 'private-course-passage'),)))
        self.open()
        self.addCleanup(self.close)
        self.body = dict(learner_id='learner', project_id='prj-1', revision='rev-12',
            snapshot_id='snap-12', job_id='job-21', diagnostic_id='diag-1', issue_id='issue',
            question='private-learner-question', mode='rule', intent='diagnose')

    def open(self):
        self.factory = make_factory(GatewayConfig(self.g.server.server_port, 'fixture-token', 'inst-1'),
            anchors=(self.g.anchor,), evidence_origin='fixture', course_provider=self.courses)
        self.store = DurableStore(self.path, 'inst-1')
        self.runtime = Runtime(self.store, 'inst-1', self.factory, evidence_origin='fixture')
        self.server = HostServer(self.runtime, 'ui-course')
        self.thread = Thread(target=self.server.serve_forever, kwargs={'poll_interval': .01}, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join(2)
        self.runtime.close(); self.store.close()

    def request(self, method, path, body=None, key=None, learner='learner'):
        connection = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=3)
        headers = {'Authorization': 'Bearer ui-course', 'X-Learner-ID': learner}
        if body is not None: headers['Content-Type'] = 'application/json'
        if key: headers['Idempotency-Key'] = key
        try:
            connection.request(method, path, json.dumps(body) if body is not None else None, headers)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def wait(self, sid, action='create'):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            if self.store.action(sid, action)['status'] == 'committed':
                return self.runtime.snapshot(sid, 'learner')
            time.sleep(.01)
        self.fail('worker did not finish')

    def create(self):
        status, reply = self.request('POST', '/api/v1/teaching/sessions', self.body, 'create')
        self.assertEqual(status, 202, reply)
        sid = reply['data']['session_id']
        return sid, self.wait(sid)

    def test_factory_supplies_courses_and_publishes_metadata_not_passage_text(self):
        sid, state = self.create()
        self.assertEqual((state['status'], state['course']['status']), ('WaitingStudent', 'ok'))
        self.assertEqual(state['course']['references'][0]['passage_id'], 'test:latch:one')
        self.assertEqual(state['course']['answerability'], 'not_assessed')
        self.assertEqual((state['course']['retrieval_status'], state['course']['packing_status']), ('ok', 'ok'))
        self.assertEqual(state['cards'][0]['course_refs'], [])
        self.assertNotIn('private-course-passage', json.dumps(state))
        self.assertNotIn('debug', state['course'])
        self.assertEqual(len(self.courses.calls), 1)
        self.assertEqual(self.runtime._live[sid][0]._loop._evidence.context_items, ())

    def test_http_preserves_verified_https_reference_and_keeps_learner_isolation(self):
        sid, state = self.create()
        status, reply = self.request('GET', '/api/v1/teaching/sessions/' + sid + '/state')
        self.assertEqual(status, 200)
        self.assertEqual(reply['data']['course']['references'][0]['source_url'], 'https://example.org/course')
        self.assertNotIn('private-course-passage', json.dumps(reply))
        self.assertEqual(self.request('GET', '/api/v1/teaching/sessions/' + sid + '/state', learner='other')[0], 403)

    def test_capabilities_report_configuration_without_claiming_retrieval_success(self):
        status, reply = self.request('GET', '/api/v1/capabilities')
        self.assertEqual(status, 200)
        self.assertEqual(reply['data']['course'], dict(available=True, reason='configured'))
        self.assertEqual(self.courses.calls, [])

    def test_restarted_get_and_create_replay_hide_old_courses_until_recovered(self):
        sid, before = self.create()
        self.close()
        self.courses = Courses(CourseBundle('ok', (material('new'),)))
        self.open()
        state = self.runtime.snapshot(sid, 'learner')
        self.assertEqual((state['status'], state['course']['status']), ('RecoveryRequired', 'revalidation_required'))
        self.assertEqual(state['course']['references'], [])
        self.assertFalse(state['cards'])
        status, replay = self.request('POST', '/api/v1/teaching/sessions', self.body, 'create')
        self.assertEqual(status, 202)
        self.assertEqual(self.courses.calls, [])
        self.runtime.recover(sid, 'learner', 'recover', int(state['state_version']), self.body['question'])
        after = self.wait(sid, 'recover')
        self.assertEqual(after['status'], 'WaitingStudent')
        self.assertEqual(after['course']['references'][0]['passage_id'], 'test:latch:new')
        self.assertEqual(after['model_calls'], before['model_calls'])

    def test_missing_database_is_unavailable_without_failing_teaching(self):
        from sigflow_edu_agent.courses import SQLiteCourseProvider
        self.close(); self.courses = SQLiteCourseProvider(Path(self.temp.name) / 'private-missing.sqlite'); self.open()
        _, state = self.create()
        self.assertEqual((state['status'], state['course']['status']), ('WaitingStudent', 'unavailable'))
        self.assertEqual(state['course']['references'], [])
        self.assertNotIn('private-missing', json.dumps(state))
        self.assertFalse((Path(self.temp.name) / 'private-missing.sqlite').exists())

    def test_pre_recovery_action_replays_current_projection_after_successful_recovery(self):
        sid, first = self.create()
        old_version = int(first['state_version'])
        self.runtime.action(sid, 'learner', 'hint_next', 'hint-old', old_version)
        self.wait(sid, 'hint-old')
        # A previously accepted model action may contain course interpretation.
        # Seed that valid historical result shape without making a live API call;
        # this test exercises the real durable replay boundary, not a model mock.
        result = self.store.action(sid, 'hint-old')['result']
        result['cards'][0].update(producer='model', hint='prior-model-course-interpretation',
                                   course_refs=result['course']['references'])
        result['model_calls'] = 1
        with self.store._transaction() as database:
            database.execute('UPDATE actions SET result=? WHERE session_id=? AND action_id=?',
                             (json.dumps(result), sid, 'hint-old'))
        self.close(); self.courses = Courses(CourseBundle('ok', (material('new'),))); self.open()
        state = self.runtime.snapshot(sid, 'learner')
        self.runtime.recover(sid, 'learner', 'recover', int(state['state_version']), self.body['question'])
        fresh = self.wait(sid, 'recover')
        count = len(self.courses.calls)
        for _ in range(2):
            replay = self.runtime.action(sid, 'learner', 'hint_next', 'hint-old', old_version)
            self.assertEqual(replay['result']['course']['references'][0]['passage_id'], 'test:latch:new')
            self.assertEqual(replay['result']['state_version'], fresh['state_version'])
            self.assertNotIn('prior-model-course-interpretation', repr(replay))
        self.assertEqual(len(self.courses.calls), count)
        self.assertEqual(self.runtime.snapshot(sid, 'learner')['level'], 2)
        # The barrier must survive another process/store lifetime as well.
        self.close(); self.open()
        state = self.runtime.snapshot(sid, 'learner')
        self.runtime.recover(sid, 'learner', 'recover-again', int(state['state_version']), self.body['question'])
        fresh = self.wait(sid, 'recover-again')
        replay = self.runtime.action(sid, 'learner', 'hint_next', 'hint-old', old_version)
        self.assertEqual(replay['result']['state_version'], fresh['state_version'])
        self.assertNotIn('prior-model-course-interpretation', repr(replay))

    def test_sqlite_neutral_task_uses_question_without_invented_hardware_concept(self):
        from sigflow_edu_agent.courses import SQLiteCourseProvider
        bundle = SQLiteCourseProvider().prepare('Yosys read_verilog', ('job_observation',))
        self.assertEqual(bundle.status, 'ok')
        self.assertTrue(bundle.materials)
        self.assertIn('yosys_047', bundle.required_sources)

    def test_no_match_and_cancel_are_explicit_and_clear_references(self):
        self.courses.bundle = CourseBundle('no_match')
        sid, state = self.create()
        self.assertEqual(state['course']['status'], 'no_match')
        self.runtime.cancel(sid, 'learner')
        state = self.runtime.snapshot(sid, 'learner')
        self.assertEqual((state['status'], state['course']['references']), ('Cancelled', []))

    def test_http_cannot_select_database_or_inject_course_material(self):
        for field in ('course_db', 'course_provider', 'course_refs'):
            status, reply = self.request('POST', '/api/v1/teaching/sessions',
                self.body | {field: 'untrusted'}, 'injection-' + field)
            self.assertEqual(status, 400, reply)
        self.assertEqual(self.courses.calls, [])


class CourseAcceptanceMetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from sigflow_edu_agent.courses import SQLiteCourseProvider
        cls.provider = SQLiteCourseProvider()
        bundle = cls.provider.prepare('Yosys read_verilog', ('job_observation',))
        cls.background = dict(status='partial', answerability='not_assessed',
            references=json.loads(json.dumps([asdict(bundle.materials[0].reference)])))

    def test_real_database_locators_are_verified_without_reporting_passage_body(self):
        from sigflow_edu_agent.host.course_acceptance import verify_course_background
        facts = verify_course_background(self.background, self.provider.db_path)
        self.assertTrue(facts['metadata_verified'])
        self.assertEqual(facts['reference_count'], 1)
        self.assertNotIn('references', facts)

    def test_changed_title_url_parent_or_line_cannot_pass_metadata_acceptance(self):
        from sigflow_edu_agent.host.course_acceptance import verify_course_background
        from sigflow_edu_agent.host.acceptance_support import RunFailure
        for key in ('title', 'source_url', 'parent_id', 'line_start'):
            with self.subTest(key=key):
                background = deepcopy(self.background)
                ref = background['references'][0]
                if key == 'line_start': ref['source_spans'][0]['line_start'] += 1
                else: ref[key] = 'invented'
                with self.assertRaises(RunFailure):
                    verify_course_background(background, self.provider.db_path)


if __name__ == '__main__':
    unittest.main()
