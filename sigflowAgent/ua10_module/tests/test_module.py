"""Runs against installed ua10; fixtures are synthetic and all network is denied."""
from pathlib import Path
from dataclasses import replace
import unittest,tempfile,importlib.util,json,subprocess,sys,socket,threading,sqlite3,shutil
from ua10 import MemoryHost,MemoryConfig,LearningEvent,Observation,JobEvidence,Scope,TeachingTask
from ua10.errors import ScopeError,ConflictError,DeletedScopeError,EvidenceViolation,ThreadOwnershipError
from ua10.service import MemoryService

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('ua10_example',ROOT/'examples/basic_usage.py');example=importlib.util.module_from_spec(spec);spec.loader.exec_module(example)
SCOPE=Scope(example.LEARNER,example.PROJECT,example.INSTANCE,'session-two')

class FakePatternProvider:
    is_network=False
    def generate(self,messages,*,max_tokens):
        facts=json.loads(messages[1]['content'])['events']
        refs=[e for e in facts if e['event_type']=='issue_observed']
        ids=[e['event_id'] for e in refs]
        return dict(observations=[dict(kind='recurrence',text='不同练习分别出现赋值路径相关诊断；原问题保持独立。',event_ids=ids)],
            difficulties=[],teaching_strategies=[dict(kind='recommendation',suggestion='建议先让学生逐条列举路径，再安排同范围复测。',event_ids=ids,course_links=['complete_assignment'])],
            unsupported_conclusions=['mastery','independence','current_pass_from_history'],uncertainty=['掌握与独立性未知'],
            evidence=[{k:e[k] for k in ('event_id','run_id','revision','source_kind','created_at')} for e in refs])

class ModuleTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.path=Path(self.temp.name);self.host=MemoryHost(MemoryConfig(self.path,default_token_budget=2048))
        example.seed(self.host);self.api=self.host.bind(SCOPE)
    def tearDown(self):self.host.close();self.temp.cleanup()
    def ids(self,api=None):
        api=api or self.api;ids=[];offset=0
        while True:
            page=api.get_learning_timeline(offset=offset);ids.extend(x.event_id for x in page.facts)
            if not page.has_more:break
            offset=page.next_offset
        return ids
    def test_installed_module_and_no_u2_dependency(self):
        import ua10
        self.assertNotIn(str(ROOT/'src'),ua10.__file__)
        self.assertNotIn('sigflow_edu_agent',sys.modules)
        self.assertNotIn('lightmem',sys.modules)
        self.assertFalse(self.host.encoder.available)
    def test_cross_session_search_and_typed_bundle(self):
        self.assertEqual(set(self.ids()),{'event-one','event-modified','event-passed','event-three'})
        result=self.api.search_memories('锁存器诊断');self.assertEqual({f.event_id for f in result.facts},{'event-one','event-three'})
        task=TeachingTask('锁存器怎么检查','latch','current-report','r3',example.stamp())
        bundle=self.api.get_teaching_memory_bundle(task);self.assertTrue(bundle.facts);self.assertFalse(bundle.patterns)
        self.assertLessEqual(bundle.measured_tokens,2048);self.assertIn('facts',bundle.to_dict())
    def test_scope_is_bound_and_foreign_identity_denied(self):
        self.host.create_profile('foreign');foreign=self.host.create_session('foreign',example.PROJECT,example.INSTANCE,'foreign-session')
        other=self.host.bind(foreign)
        self.assertFalse(other.search_memories('锁存器诊断').facts)
        with self.assertRaises(ScopeError):other.revoke_learning_event('event-one')
        with self.assertRaises(ScopeError):MemoryService(self.host,object())
        with self.assertRaises(TypeError):self.api.search_memories('latch',learner_id='foreign')
    def test_project_and_instance_isolation(self):
        for p,i,s in [('another-project',example.INSTANCE,'p'),(example.PROJECT,'another-board','i')]:
            scope=self.host.create_session(example.LEARNER,p,i,s)
            self.assertFalse(self.host.bind(scope).get_learning_timeline().facts)
    def test_idempotent_replay_and_immutable_payload(self):
        scope=Scope(example.LEARNER,example.PROJECT,example.INSTANCE,'session-one');api=self.host.bind(scope)
        event=LearningEvent('event-one','run-one','issue_observed',('latch',),job=example.job('job-one','r1','snapshot-one'))
        result=api.append_learning_event(event,observation=Observation('LATCH_INFERRED','exercise-mux','top.mux'),synthetic=True)
        self.assertFalse(result.recorded);self.assertEqual(len(self.ids()),4)
        with self.assertRaises(ConflictError):api.append_learning_event(replace(event,hint_level=2),observation=Observation('LATCH_INFERRED','exercise-mux','top.mux'),synthetic=True)
    def test_future_and_availability_cutoff(self):
        self.assertFalse(self.api.get_learning_timeline(as_of='2020-01-01T00:00:00Z').facts)
        self.assertFalse(self.api.search_memories('锁存器诊断',as_of='2020-01-01T00:00:00Z').facts)
        with self.assertRaises(ValueError):self.api.get_learning_timeline(as_of='2020-01-01')
    def test_source_is_authority_not_projection(self):
        self.api.search_memories('锁存器诊断')
        with self.host.projection.db:
            self.host.projection.db.execute("UPDATE events SET data=json_set(data,'$.summary','forged') WHERE id='event-one'")
        with self.assertRaises(EvidenceViolation):self.api.search_memories('锁存器诊断')
        self.api.rebuild_index()
        self.assertTrue(self.api.search_memories('锁存器诊断').facts)
    def test_mock_source_search_cannot_enter_default_bundle(self):
        scope=self.host.create_session(example.LEARNER,example.PROJECT,example.INSTANCE,'mock-session');api=self.host.bind(scope,source_kind='mock')
        api.create_run('mock-r1','mock-snapshot',run_id='mock-run')
        job=JobEvidence('mock-job',example.PROJECT,example.INSTANCE,'mock-r1','mock-snapshot','mock-report','sha256:'+'0'*64,kind='mock',outcome='failed')
        api.append_learning_event(LearningEvent('mock-event','mock-run','issue_observed',('latch',),job=job),observation=Observation('LATCH_INFERRED','mock-exercise'),synthetic=True)
        self.assertEqual({x.source_kind for x in api.search_memories('锁存器诊断').facts},{'mock'})
        bundle=api.get_teaching_memory_bundle(TeachingTask('锁存器','latch','current','r2',example.stamp()))
        self.assertFalse(bundle.facts)
        with self.assertRaises(ScopeError):api.build_learning_patterns(('latch',),provider=FakePatternProvider())
    def test_pattern_evidence_separate_from_facts_and_revoke(self):
        before=set(self.ids());pattern=self.api.build_learning_patterns(('latch',),provider=FakePatternProvider())
        self.assertTrue(pattern.needs_teacher_review);self.assertEqual(set(self.ids()),before)
        found=self.api.get_learning_patterns(concept='latch');self.assertTrue(found.patterns)
        bundle=self.api.get_teaching_memory_bundle(TeachingTask('锁存器','latch','current','r2',example.stamp()))
        self.assertTrue(bundle.patterns);self.assertTrue(bundle.recommendations)
        self.api.revoke_learning_event('event-one');self.assertFalse(self.api.get_learning_patterns(concept='latch').patterns)
    def test_pattern_reference_forgery_rejected(self):
        class Bad(FakePatternProvider):
            def generate(self,*args,**kwargs):
                value=super().generate(*args,**kwargs);value['evidence'][0]['revision']='r999';return value
        with self.assertRaises(EvidenceViolation):self.api.build_learning_patterns(('latch',),provider=Bad())
        self.assertFalse(self.api.get_learning_patterns().patterns)
    def test_new_event_invalidates_pattern(self):
        self.api.build_learning_patterns(('latch',),provider=FakePatternProvider())
        self.api.create_run('r4','snapshot-new',run_id='new-run',execution_mode='real')
        self.api.append_learning_event(LearningEvent('new-record','new-run','concept_exposed',('latch',)),synthetic=True)
        self.assertFalse(self.api.get_learning_patterns().patterns)
    def test_same_concept_does_not_merge_issue(self):
        related=self.api.get_related_attempts('latch');self.assertTrue(related.attempts)
        self.assertTrue(all(not x.fusion_allowed for x in related.attempts))
        observations=self.api.search_memories('锁存器诊断').facts
        self.assertEqual(len({f.issue_id for f in observations}),2)
    def test_revoke_reopen_and_rebuild_no_resurrection(self):
        self.api.revoke_learning_event('event-one');self.api.rebuild_index();self.host.close()
        self.host=MemoryHost(MemoryConfig(self.path));self.api=self.host.bind(SCOPE)
        self.assertNotIn('event-one',self.ids());self.assertNotIn('event-one',[f.event_id for f in self.api.search_memories('锁存器诊断').facts])
        self.assertTrue(self.host.authority.v1.repo.receipt('event-one'))
    def test_delete_session_and_stale_binding_fail_closed(self):
        status=self.api.delete_session();self.assertEqual(status['status'],'completed')
        with self.assertRaises(DeletedScopeError):self.api.get_learning_timeline()
        self.host.close();self.host=MemoryHost(MemoryConfig(self.path))
        with self.assertRaises(DeletedScopeError):self.host.bind(SCOPE)
    def test_thread_ownership_is_explicit(self):
        errors=[]
        def f():
            try:self.api.get_learning_timeline()
            except Exception as exc:errors.append(exc)
        thread=threading.Thread(target=f);thread.start();thread.join()
        self.assertIsInstance(errors[0],ThreadOwnershipError)
    def test_physical_delete_redacts_replay_journal(self):
        self.assertEqual(self.api.delete_session()['status'],'completed')
        row=self.host.authority.control.execute("SELECT event,observation,result,state FROM intents WHERE id='event-three'").fetchone()
        self.assertEqual(tuple(row),('{}',None,None,'erased'))
        self.host.close();self.host=MemoryHost(MemoryConfig(self.path))
        row=self.host.authority.control.execute("SELECT event,state FROM intents WHERE id='event-three'").fetchone()
        self.assertEqual(tuple(row),('{}','erased'))
    def test_default_never_uses_provider(self):
        self.assertFalse(self.host.config.enable_real_api)
        class Network(FakePatternProvider):is_network=True
        with self.assertRaises(PermissionError):self.api.build_learning_patterns(('latch',),provider=Network())

if __name__=='__main__':unittest.main(verbosity=2)
