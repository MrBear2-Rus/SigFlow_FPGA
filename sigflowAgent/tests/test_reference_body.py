"""Internal body return against real Runtime, actions and reference journal."""
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import tempfile
from threading import Event, Thread
import time
import unittest

from sigflow_edu_agent.demo import FixtureSource
from sigflow_edu_agent.host.reference import ReviewedReference
from sigflow_edu_agent.host.runtime import Runtime
from sigflow_edu_agent.host.store import DurableStore
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.runtime.session import TeachingSession
from test_host_reference import ReceiptFixture


class ReferenceBodyTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(callable(getattr(Runtime,'reference_body',None)),'internal reference body port missing')
        self.temp=tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1])
        self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name)/'host.sqlite'
        self.port=ReceiptFixture();self.reads=0
        self.content='PRIVATE synthetic reference body 170462'
        self.item=ReviewedReference('synthetic-ref',self.content,hashlib.sha256(self.content.encode()).hexdigest(),
                                    ('synthetic-a','synthetic-b'))
        self.hold=lambda:None
        self.body=dict(learner_id='learner',project_id='p',revision='r',snapshot_id='snap',
            job_id='fixture-job-1',diagnostic_id='diag',issue_id='issue',
            question='Explain latch',mode='rule',intent='diagnose')
        self.open();self.addCleanup(self.close)

    def open(self,configured=True,reference_provider=None):
        def factory(binding,request,mode,reserve):
            return TeachingSession(TeachingLoop(request,FixtureSource()))
        def provider(binding):
            self.reads+=1;self.hold()
            return self.item
        self.store=DurableStore(self.path,'inst')
        options=dict(reference_provider=reference_provider or provider,receipt_port=self.port) if configured else {}
        self.runtime=Runtime(self.store,'inst',factory,evidence_origin='fixture',**options)

    def close(self):
        self.runtime.close();self.store.close()

    def wait(self,sid,aid):
        until=time.monotonic()+4
        while time.monotonic()<until:
            if self.store.action(sid,aid)['status'] in ('committed','interrupted'):
                return self.runtime.snapshot(sid,'learner')
            time.sleep(.01)
        self.fail('action did not finish')

    def action(self,sid,state,kind,aid,payload=None):
        self.runtime.action(sid,'learner',kind,aid,int(state['state_version']),reference=payload)
        return self.wait(sid,aid)

    def create(self,level=3):
        sid=self.runtime.create(self.body,'create')['session_id'];state=self.wait(sid,'create')
        for n in range(1,level):state=self.action(sid,state,'hint_next','hint-'+str(n))
        return sid,state

    def confirm(self):
        sid,state=self.create()
        requested=self.action(sid,state,'reference_request','request',{})
        challenge=requested['reference']['challenge'];self.port.issue(challenge)
        state=self.action(sid,requested,'reference_confirm','confirm',
            dict(challenge_id=challenge['challenge_id'],receipt_id='ui-receipt-1'))
        self.assertEqual(state['reference']['status'],'confirmed')
        return sid,state

    def read(self,sid,state):return self.runtime.reference_body(sid,'learner',int(state['state_version']))

    def test_unconfirmed_wrong_scope_wrong_version_and_disabled_reject_before_provider(self):
        sid,state=self.create(1);before=self.reads
        with self.assertRaisesRegex(ValueError,'reference_body_unavailable'):self.read(sid,state)
        self.assertEqual(self.reads,before)
        for n in (1,2):state=self.action(sid,state,'hint_next','extra-hint-'+str(n))
        with self.assertRaisesRegex(ValueError,'reference_body_unavailable'):self.read(sid,state)
        state=self.action(sid,state,'reference_request','request',{})
        before=self.reads
        with self.assertRaisesRegex(ValueError,'reference_body_unavailable'):self.read(sid,state)
        self.assertEqual(self.reads,before)
        self.close();self.open(False)
        current=self.runtime.snapshot(sid,'learner')
        with self.assertRaisesRegex(ValueError,'receipt_unavailable'):self.read(sid,current)

    def test_confirmed_body_returns_without_state_db_event_budget_or_receipt_changes(self):
        sid,state=self.confirm();row=self.store.get(sid);events=self.runtime.events(sid,'learner',0)
        calls=list(self.port.calls);facts=self.store.pending_teaching_facts(32)
        with self.store._transaction() as db:
            actions=[dict(r) for r in db.execute('SELECT * FROM actions')]
        for _ in range(2):
            result=self.read(sid,state)
            self.assertEqual(result['content'],self.content)
            self.assertEqual(result['reference_id'],self.item.reference_id)
            result['reviewers'].clear()
        self.assertEqual(self.store.get(sid),row)
        self.assertEqual(self.runtime.events(sid,'learner',0),events)
        self.assertEqual(self.store.pending_teaching_facts(32),facts)
        self.assertEqual(self.port.calls,calls)
        with self.store._transaction() as db:
            self.assertEqual([dict(r) for r in db.execute('SELECT * FROM actions')],actions)
        self.assertNotIn(self.content,json.dumps([row,events,actions]))
        self.assertNotIn(self.content.encode(),self.path.read_bytes())
        self.assertEqual(self.runtime.snapshot(sid,'learner'),state)
        for learner,version,reason in (('other',int(state['state_version']),'scope_denied'),
            ('learner',int(state['state_version'])-1,'stale_state'),('learner',True,'invalid_state_version')):
            before=self.reads
            with self.assertRaisesRegex(ValueError,reason):self.runtime.reference_body(sid,learner,version)
            self.assertEqual(self.reads,before)

    def test_real_pinned_provider_rechecks_review_revocation_after_confirmation(self):
        from sigflow_edu_agent.host.materials import ReviewedMaterialProvider
        from sigflow_edu_agent.host.reference import POLICY
        root=Path(self.temp.name)/'materials';root.mkdir()
        (root/'synthetic-ref.txt').write_text(self.content,encoding='utf-8')
        manifest=root/'manifest.json'
        entry=dict(reference_id='synthetic-ref',project_id='p',revision='r',issue_id='issue',policy=POLICY,
            sha256=self.item.sha256,reviewers=list(self.item.reviewers),
            not_before_epoch=int(time.time())-1,expires_at_epoch=int(time.time())+600)
        raw=json.dumps(dict(schema_version='u2.reviewed-materials.v1',references=[entry])).encode()
        manifest.write_bytes(raw);allowed=[True]
        provider=ReviewedMaterialProvider(manifest,hashlib.sha256(raw).hexdigest(),root,lambda entry:allowed[0])
        self.close();self.open(reference_provider=provider)
        sid,state=self.confirm()
        self.assertEqual(self.read(sid,state)['content'],self.content)
        allowed[0]=False
        with self.assertRaisesRegex(ValueError,'reference_material_unavailable'):self.read(sid,state)

    def test_pending_before_read_is_fenced_before_provider(self):
        sid,state=self.confirm()
        self.store.claim(sid,'pending','fingerprint',int(state['state_version']),kind='reference_request')
        before=self.reads
        with self.assertRaisesRegex(ValueError,'session_busy'):self.read(sid,state)
        self.assertEqual(self.reads,before)

    def test_worker_commit_cannot_mix_old_row_with_new_pending_or_journal_snapshot(self):
        from unittest.mock import patch
        sid,state=self.confirm();before=self.store.get(sid)
        entered,release,after_get,committed=Event(),Event(),Event(),Event()
        results,errors=[],[]
        self.hold=lambda:(entered.set(),release.wait(3))
        original_get=self.store.get
        def hooked_get(key):
            row=original_get(key)
            if release.is_set():
                after_get.set()
                # A real worker commits independently of Runtime._guard. With a
                # single snapshot mutex it waits and this read sees pending.
                committed.wait(.3)
            return row
        def read():
            try:results.append(self.read(sid,state))
            except ValueError as error:errors.append(str(error))
        def commit():
            if after_get.wait(2):
                updated=dict(state,state_version=str(int(state['state_version'])+1))
                self.store.commit(sid,'racing-reconcile',updated,before['checkpoint'],updated)
                committed.set()
        reader=Thread(target=read);writer=Thread(target=commit)
        with patch.object(self.store,'get',side_effect=hooked_get):
            reader.start()
            try:
                self.assertTrue(entered.wait(1))
                self.store.claim(sid,'racing-reconcile','fingerprint',int(state['state_version']),
                                 kind='reference_reconcile')
                writer.start();release.set();reader.join(3);writer.join(3)
            finally:
                release.set();reader.join(3)
                if writer.ident is not None:writer.join(3)
        self.assertTrue(committed.is_set())
        self.assertEqual(results,[],'body returned for a version superseded during metadata reads')
        self.assertTrue(errors)

    def test_metadata_and_released_journal_are_required(self):
        sid,state=self.confirm()
        with self.store._transaction() as db:
            db.execute("UPDATE reference_records SET phase='consuming'")
        before=self.reads
        with self.assertRaisesRegex(ValueError,'reference_body_unavailable'):self.read(sid,state)
        self.assertEqual(self.reads,before)

    def test_changed_review_or_body_and_private_provider_failure_are_rejected(self):
        sid,state=self.confirm();original=self.item
        for changed in (replace(original,reviewers=('changed-a','changed-b')),
            ReviewedReference('new-ref',original.content,original.sha256,original.reviewers),
            ReviewedReference(original.reference_id,'changed body',hashlib.sha256(b'changed body').hexdigest(),
                              original.reviewers)):
            self.item=changed
            with self.assertRaisesRegex(ValueError,'reference_material_unavailable'):self.read(sid,state)
        self.item=original
        def fail():raise OSError('private material token')
        self.hold=fail
        with self.assertRaisesRegex(ValueError,'^reference_material_unavailable$'):self.read(sid,state)

    def test_slow_provider_does_not_block_cancel_or_snapshot_and_cancel_fences_return(self):
        sid,state=self.confirm();entered,release,done=Event(),Event(),Event();errors=[]
        self.hold=lambda:(entered.set(),release.wait(3))
        def read():
            try:self.read(sid,state)
            except ValueError as error:errors.append(str(error))
        thread=Thread(target=read);thread.start()
        def cancel():
            self.runtime.snapshot(sid,'learner');self.runtime.cancel(sid,'learner');done.set()
        canceller=Thread(target=cancel)
        try:
            self.assertTrue(entered.wait(1));canceller.start()
            self.assertTrue(done.wait(.5),'provider holds runtime global guard')
        finally:
            release.set();thread.join(3)
            if canceller.ident is not None:canceller.join(3)
        self.assertEqual(errors,['reference_body_unavailable'])

    def test_version_change_pending_action_and_host_close_during_provider_fence_return(self):
        for operation in ('version','pending','record','close'):
            with self.subTest(operation=operation):
                if operation!='version':
                    self.close();self.path=Path(self.temp.name)/(operation+'.sqlite');self.open()
                sid,state=self.confirm();entered,release=Event(),Event();errors=[]
                self.hold=lambda:(entered.set(),release.wait(3))
                def read():
                    try:self.read(sid,state)
                    except ValueError as error:errors.append(str(error))
                thread=Thread(target=read);thread.start()
                try:
                    self.assertTrue(entered.wait(1))
                    if operation=='version':
                        self.action(sid,state,'finish','finish')
                    elif operation=='pending':
                        self.store.claim(sid,'pending','fingerprint',int(state['state_version']),kind='reference_request')
                    elif operation=='record':
                        with self.store._transaction() as db:
                            row=db.execute('SELECT challenge FROM reference_records').fetchone()
                            changed=json.loads(row['challenge']);changed['reference_id']='changed-ref'
                            db.execute('UPDATE reference_records SET challenge=?',(json.dumps(changed),))
                    else:self.runtime.close()
                finally:release.set();thread.join(3);self.hold=lambda:None
                self.assertTrue(errors)
                self.assertNotIn(self.content,str(errors))

    def test_restart_requires_recovery_then_explicit_reconcile_without_reconsume(self):
        sid,state=self.confirm();calls=list(self.port.calls)
        self.close();self.open();state=self.runtime.snapshot(sid,'learner')
        with self.assertRaisesRegex(ValueError,'reference_body_unavailable'):self.read(sid,state)
        self.runtime.recover(sid,'learner','recover',int(state['state_version']),self.body['question'])
        state=self.wait(sid,'recover')
        with self.assertRaisesRegex(ValueError,'reference_body_unavailable'):self.read(sid,state)
        state=self.action(sid,state,'reference_reconcile','reconcile',{})
        self.assertEqual(self.read(sid,state)['content'],self.content)
        self.assertEqual(self.port.calls,calls)
