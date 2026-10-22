"""Real U2 Runtime composed into U1; fixture evidence is explicitly synthetic."""
from dataclasses import replace
import importlib.util
from pathlib import Path
import tempfile,time,unittest

from sigflow_edu_agent.errors import AgentError
from sigflow_edu_agent.teaching_store import BindingStore

ROOT=Path(__file__).resolve().parents[2]
import sys
if sys.version_info < (3, 11):
    raise unittest.SkipTest('optional U2 composition requires Python 3.11+')
sys.path.append(str(ROOT/'sigflowAgent'))
try:
    _u2_spec = importlib.util.find_spec('sigflow_u2')
except ModuleNotFoundError as error:
    if error.name != 'sigflow_u2':
        raise
    _u2_spec = None
if _u2_spec is None:
    raise unittest.SkipTest('optional U2 source/package is not installed')
from sigflow_u2.context.models import EvidenceAnchor
from sigflow_u2.demo import FixtureSource
from sigflow_u2.host.runtime import Runtime
from sigflow_u2.host.shared_adapter import SharedTeachingAdapter
from sigflow_u2.host.store import DurableStore
from sigflow_u2.loop import TeachingLoop
from sigflow_u2.runtime.session import TeachingSession


class TeachingServiceTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(importlib.util.find_spec('sigflow_edu_agent.teaching'),'U1 composition missing')
        from sigflow_edu_agent.teaching import TeachingService
        self.type=TeachingService
        owned=ROOT/'.superpowers/sdd/u1-teaching-tests/service'
        owned.mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=owned)
        self.addCleanup(self.temp.cleanup)
        self.directory=Path(self.temp.name)
        evidence=FixtureSource().collect('project','revision')
        self.evidence=replace(evidence,anchor=EvidenceAnchor(evidence.evidence_id,'project','revision',
            evidence.job_id,'snapshot','artifact','a'*64))
        self.current=True
        self.open()
        self.addCleanup(self.close)
        self.body=dict(question='Explain the checked tool observation',job_id=evidence.job_id,
                       issue_id='issue',mode='rule',intent='diagnose')

    def open(self):
        owner=self
        class Source:
            def collect(self,*args):return owner.evidence
        factory=lambda b,r,m,reserve:TeachingSession(TeachingLoop(r,Source()))
        self.u2store=DurableStore(self.directory/'u2.sqlite','instance')
        self.runtime=Runtime(self.u2store,'instance',factory,evidence_origin='fixture')
        self.bindings=BindingStore(self.directory/'bindings.sqlite','instance')
        def current(session):
            if not self.current:raise AgentError('STALE_REVISION','revision changed',http_status=409)
        def prepare(session,body):
            return dict(job_id=self.evidence.job_id,snapshot_id='snapshot',diagnostic_id='job-report',issue_id=body['issue_id'])
        adapter=lambda c,sid=None:SharedTeachingAdapter(self.runtime,lambda eid:self.evidence)
        self.service=self.type(self.runtime,self.bindings,prepare,adapter,current)

    def close(self):
        self.service.close()
        self.u2store.close()

    def create(self):
        sid=self.service.create_session('learner',dict(project_id='project',revision='revision'),'session')['session_id']
        view=self.service.start(sid,'learner',self.body,'create')
        return sid,view['run_id']

    def wait(self,rid,status,after=None):
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            try:view=self.service.project(rid,'learner')
            except AgentError as error:
                # A worker may commit between the projection's two reads.
                # A new GET is safe; no action or create is repeated here.
                if error.http_status!=409:raise
                time.sleep(.01);continue
            if view['teaching_status']==status and (after is None or int(view['state_version'])>int(after)):return view
            time.sleep(.01)
        self.fail('composed runtime did not commit expected state')

    def test_explicit_hint_and_finish_project_pending_then_real_terminal(self):
        sid,rid=self.create();first=self.wait(rid,'WaitingStudent')
        self.assertIsNone(first['run'])
        self.assertNotIn('teaching_session_id',str(first))
        action=dict(kind='hint_next',action_id='hint',expected_state_version=first['state_version'])
        ack=self.service.action(rid,'learner',action)
        self.assertTrue(ack['accepted'])
        second=self.wait(rid,'WaitingStudent',first['state_version'])
        self.assertEqual([c['level'] for c in second['cards']],['L1','L2'])
        self.service.action(rid,'learner',action)
        self.assertEqual(self.service.project(rid,'learner')['state_version'],second['state_version'])
        self.service.action(rid,'learner',dict(kind='finish',action_id='finish',expected_state_version=second['state_version']))
        self.assertEqual(self.wait(rid,'Completed')['run']['state'],'degraded')
        events=self.service.events(rid,'learner',0,100)
        self.assertTrue(events['events'])
        self.assertIsInstance(events['events'][0]['sequence'],str)
        self.assertNotIn('session-',str(events))

    def test_external_reservation_retries_after_attach_failure_without_new_u2_session(self):
        from unittest.mock import patch
        sid=self.service.create_session('learner',dict(project_id='project',revision='revision'),'session')['session_id']
        with patch.object(self.bindings,'attach',side_effect=OSError('synthetic failed publication')):
            with self.assertRaises(AgentError):self.service.start(sid,'learner',self.body,'create')
        view=self.service.start(sid,'learner',self.body,'create')
        self.wait(view['run_id'],'WaitingStudent')
        self.assertEqual(len(self.u2store.list_sessions()),1)
        with self.assertRaises(AgentError) as error:
            self.service.start(sid,'learner',self.body|dict(question='different'),'create')
        self.assertEqual(error.exception.code,'IDEMPOTENCY_CONFLICT')

    def test_restart_quarantines_old_cards_and_needs_explicit_recovery(self):
        _,rid=self.create();self.wait(rid,'WaitingStudent')
        self.close();self.open()
        view=self.service.project(rid,'learner')
        self.assertEqual((view['teaching_status'],view['cards'],view['run']),('RecoveryRequired',[],None))
        self.service.action(rid,'learner',dict(kind='recover',action_id='recover',
            expected_state_version=view['state_version'],question=self.body['question']))
        self.assertTrue(self.wait(rid,'WaitingStudent')['cards'])

    def test_unpublished_handle_retry_after_restart_reuses_quarantined_session(self):
        from unittest.mock import patch
        sid=self.service.create_session('learner',dict(project_id='project',revision='revision'),'session')['session_id']
        with patch.object(self.bindings,'attach',side_effect=OSError('publication interrupted')):
            with self.assertRaises(AgentError):self.service.start(sid,'learner',self.body,'create')
        deadline=time.monotonic()+5
        while self.u2store.list_sessions()[0]['state']['status']!='WaitingStudent':
            self.assertLess(time.monotonic(),deadline)
            time.sleep(.01)
        self.close();self.open()
        result=self.service.start(sid,'learner',self.body,'create')
        self.assertEqual(result['run_id'],'run-1')
        self.assertEqual(len(self.u2store.list_sessions()),1)
        self.assertEqual(self.service.project('run-1','learner')['teaching_status'],'RecoveryRequired')

    def test_shutdown_releases_other_resources_when_one_closer_fails(self):
        from unittest.mock import patch
        marker=self.directory/'closed'
        self.service._closers=(lambda:(_ for _ in ()).throw(OSError('close failed')),
                               lambda:marker.write_text('closed'))
        self.addCleanup(self.bindings.close)
        self.addCleanup(self.runtime.close)
        with patch.object(self.runtime,'close',side_effect=OSError('runtime close failed')):
            with self.assertRaises(OSError):self.service.close()
        self.assertTrue(marker.exists(),'remaining owned resources must still close')
        with self.assertRaises(AgentError):self.bindings.create_session('learner','project','revision','closed')

    def test_ready_failure_and_early_exit_release_bound_socket_and_teaching(self):
        import io,socket
        from unittest.mock import patch
        from sigflow_edu_agent import __main__ as entry
        from sigflow_edu_agent.bootstrap import Bootstrap
        from sigflow_edu_agent.http_server import create_server
        bootstrap=Bootstrap('instance','a'*64,'http://127.0.0.1:1','gateway-test','u1-ui',str(self.directory))
        for mode in ('exit-after-ready','none'):
            if mode=='none':self.close();self.open()
            servers=[]
            def bind(state):
                server=create_server(state);servers.append(server);return server
            def output(*args):
                if mode=='none':raise OSError('stdout closed')
            try:
                with patch.object(entry,'read_from_stream',return_value=bootstrap), \
                     patch('sigflow_edu_agent.teaching_factory.compose',return_value=self.service), \
                     patch.object(entry,'create_server',side_effect=bind), \
                     patch.object(entry,'_install_signal_handlers'), \
                     patch.object(entry,'emit_ready',side_effect=output), \
                     patch.object(entry.sys,'stdout',io.StringIO()):
                    if mode=='none':
                        with self.assertRaises(OSError):entry.main(['--bootstrap-stdin','--u2-teaching'])
                    else:self.assertEqual(entry.main(['--bootstrap-stdin','--u2-teaching','--selftest-fail',mode]),9)
                with socket.socket() as probe:
                    self.assertNotEqual(probe.connect_ex(('127.0.0.1',servers[0].server_port)),0)
                with self.assertRaises(AgentError) as error:
                    self.bindings.create_session('learner','project','revision','closed')
                self.assertEqual(error.exception.http_status,503)
            finally:
                for server in servers:server.server_close()

    def test_foreign_owner_denied_and_cancel_still_works_after_revision_changes(self):
        _,rid=self.create();self.wait(rid,'WaitingStudent')
        with self.assertRaises(AgentError) as error:self.service.project(rid,'foreign')
        self.assertEqual(error.exception.http_status,403)
        self.current=False
        with self.assertRaises(AgentError) as error:self.service.project(rid,'learner')
        self.assertEqual(error.exception.code,'STALE_REVISION')
        self.assertTrue(self.service.cancel(rid,'learner')['accepted'])
        handle=self.bindings.run(rid,'learner')['handle']
        self.assertEqual(self.runtime.snapshot(handle['teaching_session_id'],'learner')['status'],'Cancelled')
