"""A real U2 runtime projects frozen shared cards without inventing run states."""
from dataclasses import replace
import importlib
import importlib.util
from pathlib import Path
import tempfile
import time
import unittest

from sigflow_edu_agent.context.models import EvidenceAnchor
from sigflow_edu_agent.demo import FixtureSource
from sigflow_edu_agent.host.runtime import Runtime
from sigflow_edu_agent.host.store import DurableStore
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.runtime.session import TeachingSession


class SharedAdapterTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.find_spec('sigflow_edu_agent.host.shared_adapter')
        self.assertIsNotNone(spec, 'shared_adapter_missing')
        self.module = importlib.import_module(spec.name)
        self.directory = tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1])
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)/'host.sqlite'
        self.evidence = FixtureSource().collect('p','r')
        self.anchor = EvidenceAnchor(self.evidence.evidence_id,'p','r',self.evidence.job_id,'s','artifact','a'*64)
        self.evidence = replace(self.evidence, anchor=self.anchor)
        owner = self
        class Source:
            def collect(self, project_id, revision):
                return owner.evidence
        self.factory = lambda binding, request, mode, reserve: TeachingSession(TeachingLoop(request,Source()))
        self.resolve = lambda evidence_id: self.evidence if evidence_id == self.evidence.evidence_id else None
        self.context = self.module.SharedRunContext('sess-1','run-1','inst','learner','p','r','s',
            self.evidence.job_id,'diag','issue')
        self.open()
        self.addCleanup(self.close)

    def open(self):
        self.store = DurableStore(self.path,'inst')
        self.runtime = Runtime(self.store,'inst',self.factory,evidence_origin='fixture')
        self.adapter = self.module.SharedTeachingAdapter(self.runtime,self.resolve,secrets=('synthetic-ui-secret',))

    def close(self):
        self.runtime.close()
        self.store.close()

    def start(self,intent='diagnose'):
        handle = self.adapter.start(self.context,'Explain the observed evidence',idempotency_key='create',intent=intent)
        self.wait(handle,'create')
        return handle

    def wait(self,handle,action):
        deadline = time.monotonic()+4
        while time.monotonic()<deadline:
            if self.store.action(handle.teaching_session_id,action)['status']=='committed':
                return self.runtime.snapshot(handle.teaching_session_id,'learner')
            time.sleep(.01)
        self.fail('teaching operation did not commit')

    def validator(self):
        root = Path(__file__).resolve().parents[2]
        spec = importlib.util.spec_from_file_location('_u1_contract_validator',root/'agent/src/sigflow_edu_agent/contract_schema.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.ContractSchemaValidator(root/'contracts/edu-agent/v1/schemas')

    def action(self,handle,kind,aid,**extra):
        version = self.runtime.snapshot(handle.teaching_session_id,'learner')['state_version']
        body = dict(kind=kind,action_id=aid,expected_state_version=version,**extra)
        result = self.adapter.action(handle,body)
        self.wait(handle,aid)
        return result,body

    def test_explanation_projects_frozen_shared_card_and_terminal_run(self):
        view = self.adapter.project(self.start('explain'))
        self.assertEqual((view['phase'],view['teaching_status']),('ready','Completed'))
        self.assertEqual(view['run']['state'],'degraded')
        self.assertFalse(view['run']['model_used'])
        self.validator().validate(view['run'],'agent-run.schema.json')
        ref = view['cards'][0]['evidence'][0]['ref']
        self.assertEqual((ref['job_id'],ref['artifact_id'],ref['artifact_hash']),
            (self.anchor.job_id,'artifact','a'*64))
        self.assertTrue(any('离线' in text for text in view['cards'][0]['limitations']))

    def test_waiting_teaching_is_pending_and_hint_is_explicit_idempotent(self):
        handle = self.start()
        first = self.adapter.project(handle)
        self.assertEqual((first['phase'],first['teaching_status'],first['run']),('pending','WaitingStudent',None))
        self.validator().validate(first['cards'][0],'teaching-card.schema.json')
        _,body = self.action(handle,'hint_next','hint')
        second = self.adapter.project(handle)
        self.assertEqual([c['level'] for c in second['cards']],['L1','L2'])
        self.adapter.action(handle,body)
        self.assertEqual(self.adapter.project(handle),second)
        self.action(handle,'finish','finish')
        self.assertEqual(self.adapter.project(handle)['run']['state'],'degraded')

    def test_gateway_prefixed_sha256_is_preserved_in_shared_evidence(self):
        self.anchor = replace(self.anchor, artifact_hash='sha256:'+'a'*64)
        self.evidence = replace(self.evidence, anchor=self.anchor)
        view = self.adapter.project(self.start('explain'))
        self.assertEqual(view['cards'][0]['evidence'][0]['ref']['artifact_hash'],
            'sha256:'+'a'*64)
        self.validator().validate(view['run'], 'agent-run.schema.json')

    def test_direct_cancel_returns_public_acceptance_and_can_be_repeated(self):
        handle=self.start()
        for _ in range(2):
            ack=self.adapter.cancel(handle)
            self.assertTrue(ack['accepted'])
            self.assertEqual((ack['session_id'],ack['run_id']),('sess-1','run-1'))
            self.assertNotIn('result',ack)
        self.assertEqual(self.adapter.project(handle)['teaching_status'],'Cancelled')

    def test_create_replay_uses_original_teaching_session_and_rejects_changed_question(self):
        handle = self.start()
        same = self.adapter.start(self.context,'Explain the observed evidence',idempotency_key='create')
        self.assertEqual(handle,same)
        self.assertEqual(len(self.store.list_sessions()),1)
        with self.assertRaises(ValueError):
            self.adapter.start(self.context,'Changed question',idempotency_key='create')

    def test_foreign_learner_project_and_instance_handles_are_rejected(self):
        handle = self.start()
        for field in ('learner_id','project_id','instance_id'):
            with self.subTest(field=field),self.assertRaises(ValueError):
                self.adapter.project(replace(handle,context=replace(self.context,**{field:'foreign'})))

    def test_missing_expired_or_foreign_anchor_never_gets_a_fabricated_reference(self):
        handle = self.start()
        for anchor in (None,replace(self.anchor,expired=True),replace(self.anchor,project_id='foreign'),
                       replace(self.anchor,evidence_id='foreign'),replace(self.anchor,job_id='foreign-job')):
            with self.subTest(anchor=anchor):
                proof=replace(self.evidence,anchor=anchor) if anchor is not None else None
                adapter = self.module.SharedTeachingAdapter(self.runtime,lambda _:proof)
                with self.assertRaisesRegex(ValueError,'shared_evidence_unavailable'):
                    adapter.project(handle)

    def test_cancel_during_reference_resolution_fences_card_publication(self):
        handle = self.start()
        def resolve(_):
            self.runtime.cancel(handle.teaching_session_id,'learner')
            return self.evidence
        adapter = self.module.SharedTeachingAdapter(self.runtime,resolve)
        with self.assertRaisesRegex(ValueError,'shared_projection_stale'):
            adapter.project(handle)
        self.assertEqual(self.adapter.project(handle)['cards'],[])

    def test_unknown_action_fields_and_implicit_l4_are_denied(self):
        handle = self.start()
        state = self.adapter.project(handle)
        for extra in ({'level':'L3'},{'receipt_id':'ui-receipt-1'},{'grant_ref':'invented'},{'expected_version':'1'}):
            with self.subTest(extra=extra),self.assertRaises(ValueError):
                self.adapter.action(handle,dict(kind='hint_next',action_id='bad',
                    expected_state_version=state['state_version'],**extra))
        with self.assertRaises(ValueError):
            self.adapter.action(handle,dict(kind='reference_confirm',action_id='bad',
                expected_state_version=state['state_version']))

    def test_restart_quarantines_cards_until_explicit_recovery(self):
        handle = self.start()
        self.close();self.open()
        view = self.adapter.project(handle)
        self.assertEqual((view['phase'],view['teaching_status'],view['cards'],view['run']),
            ('pending','RecoveryRequired',[],None))
        self.action(handle,'recover','recover',question='Explain the observed evidence')
        self.assertEqual(len(self.adapter.project(handle)['cards']),1)

    def test_projection_rejects_sensitive_text_even_when_runtime_card_structure_is_valid(self):
        handle = self.start()
        # A trusted runtime's structural check alone is not an output boundary.
        for text in ('synthetic-ui-secret',r'C:\private\student.txt'):
            with self.subTest(text=text):
                original = self.runtime.snapshot
                def snapshot(sid,learner):
                    state = original(sid,learner)
                    state['cards'] = [dict(level=1,hint=text,question='What is observed?',
                        evidence_ids=[self.evidence.evidence_id],producer='model',limitations=['test boundary'],course_refs=[])]
                    return state
                self.runtime.snapshot = snapshot
                try:
                    with self.assertRaisesRegex(ValueError,'unsafe_shared_output'):
                        self.adapter.project(handle)
                finally:
                    self.runtime.snapshot = original

    def test_verification_exposes_bound_plan_while_waiting_and_cannot_execute_itself(self):
        owner = self
        class Source:
            def collect(self,project_id,revision):return owner.evidence
        class DeniedExecution:
            calls = 0
            def authorized(self,*args):return False
            def execute(self,*args):
                self.calls += 1
                raise AssertionError('unapproved execution')
        execution = DeniedExecution()
        def factory(binding,request,mode,reserve,*,plan=None,gateway_plan_hash=None):
            return TeachingSession(TeachingLoop(request,Source(),plan=plan,verification=execution))
        self.close();self.factory=factory;self.open()
        plan=dict(project_id='p',revision='r',snapshot_id='s',goal='Observe a scoped synthesis report',
            steps=[dict(step_id='synth',capability='eda.synth',provider='yosys',params={'top':'top'})],
            expected_observations=['Read diagnostics without inferring functional correctness'])
        handle=self.adapter.start(self.context,'Verify the observation',idempotency_key='verify',
            intent='verify',plan=plan,gateway_plan_hash='ui-approved-hash')
        self.wait(handle,'create')
        view=self.adapter.project(handle)
        self.assertEqual((view['phase'],view['teaching_status'],view['run']),('pending','WaitingApproval',None))
        self.assertIn('plan_card',view,'bound approval material was not projected')
        self.validator().validate(view['plan_card'],'plan-card.schema.json')
        self.assertTrue(view['plan_card']['requires_approval'])
        self.assertEqual(view['plan_card']['state_version'],view['state_version'])
        self.assertEqual(execution.calls,0)
        accepted=self.adapter.action(handle,dict(kind='execute_plan',action_id='execute',
            expected_state_version=view['state_version'],grant_ref='invented'))
        self.assertTrue(accepted['accepted'])
        denied=self.wait(handle,'execute')
        self.assertEqual((denied['status'],denied['reason']),('Failed','host_operation_failed'))
        self.assertEqual(execution.calls,0)

    def test_action_replay_returns_only_acceptance_metadata_not_private_runtime_cards(self):
        owner=self
        class Source:
            def collect(self,p,r):return owner.evidence
        class Candidate:
            def generate(self,context):
                return dict(level=context.level,hint='synthetic-ui-secret',question='Which observation supports this?',
                    evidence_ids=[context.evidence.evidence_id])
        self.close()
        self.factory=lambda binding,request,mode,reserve:TeachingSession(TeachingLoop(request,Source(),Candidate()))
        self.open()
        handle=self.start()
        _,body=self.action(handle,'hint_next','hint')
        ack=self.adapter.action(handle,body)
        import json
        self.assertNotIn('synthetic-ui-secret',json.dumps(ack))
        self.assertNotIn('result',ack)
        self.assertEqual((ack['session_id'],ack['run_id']),('sess-1','run-1'))

    def test_verified_successor_job_is_projected_only_from_committed_verification_checkpoint(self):
        from sigflow_edu_agent.domain import VerificationResult
        owner=self
        result_anchor=replace(self.anchor,evidence_id='result-evidence',job_id='result-job',artifact_id='result-artifact')
        result_evidence=replace(self.evidence,evidence_id='result-evidence',job_id='result-job',anchor=result_anchor)
        class Source:
            def collect(self,p,r):return owner.evidence
        class Execution:
            def authorized(self,plan,grant):return grant=='approved-grant'
            def execute(self,*args):return VerificationResult('Succeeded',result_evidence,None,'result-job')
        def factory(binding,request,mode,reserve,*,plan=None,gateway_plan_hash=None):
            return TeachingSession(TeachingLoop(request,Source(),plan=plan,verification=Execution()))
        self.close();self.factory=factory;self.open()
        self.adapter=self.module.SharedTeachingAdapter(self.runtime,lambda eid:
            result_evidence if eid=='result-evidence' else self.resolve(eid))
        plan=dict(project_id='p',revision='r',snapshot_id='s',goal='Observe the scoped result',
            steps=[dict(step_id='synth',capability='eda.synth',provider='yosys',params={'top':'top'})],
            expected_observations=['Read the new scoped tool report'])
        handle=self.adapter.start(self.context,'Compare result evidence',idempotency_key='verify',intent='verify',
            plan=plan,gateway_plan_hash='approved-plan-hash')
        self.wait(handle,'create')
        self.action(handle,'execute_plan','execute',grant_ref='approved-grant')
        view=self.adapter.project(handle)
        self.assertEqual((view['phase'],view['teaching_status']),('ready','Completed'))
        jobs={ref['ref']['job_id'] for card in view['cards'] for ref in card['evidence']}
        self.assertEqual(jobs,{self.evidence.job_id,'result-job'})
        self.validator().validate(view['run'],'agent-run.schema.json')

    def test_changed_proof_cannot_rebind_same_id_to_another_artifact_or_report(self):
        handle=self.start()
        for changed in (replace(self.evidence,summary='different report under the same ID'),
                replace(self.evidence,anchor=replace(self.anchor,artifact_id='other-artifact'))):
            with self.subTest(changed=changed):
                adapter=self.module.SharedTeachingAdapter(self.runtime,lambda _:changed)
                with self.assertRaisesRegex(ValueError,'shared_evidence_unavailable'):
                    adapter.project(handle)
