"""Pinned local material with synthetic review authority; no teacher identity proof."""
from copy import deepcopy
from dataclasses import replace
import hashlib
from importlib.util import find_spec
import json
from pathlib import Path
import tempfile
import time
import unittest

from sigflow_edu_agent.host.reference import POLICY
from sigflow_edu_agent.runtime.gateway_session import SessionBinding


class MaterialsTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(find_spec('sigflow_edu_agent.host.materials'),'reviewed material provider missing')
        from sigflow_edu_agent.host.materials import ReviewedMaterialProvider
        self.Provider=ReviewedMaterialProvider
        self.temp=tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1])
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.body_root=self.root/'bodies';self.body_root.mkdir()
        self.content='Synthetic body only for material contract tests.'
        self.entry=dict(reference_id='ref-one',project_id='p',revision='r',issue_id='issue',
            policy=POLICY,sha256=hashlib.sha256(self.content.encode()).hexdigest(),
            reviewers=['synthetic-a','synthetic-b'],not_before_epoch=int(time.time())-60,
            expires_at_epoch=int(time.time())+600)
        self.binding=SessionBinding('inst','session','learner','p','r','snap','job','diag','issue')
        (self.body_root/'ref-one.txt').write_text(self.content,encoding='utf-8')
        self.allowed=True;self.reviews=[]
        self.after=lambda:None
        def verify(entry):
            self.reviews.append(deepcopy(entry));entry.clear();self.after()
            return self.allowed
        self.verify=verify
        self.write_manifest()

    def write_manifest(self, entry=None):
        raw=json.dumps(dict(schema_version='u2.reviewed-materials.v1',references=[entry or self.entry])).encode()
        self.manifest=self.root/'manifest.json';self.manifest.write_bytes(raw)
        self.pinned=hashlib.sha256(raw).hexdigest()
        self.provider=self.Provider(self.manifest,self.pinned,self.body_root,self.verify)

    def test_each_read_rechecks_manifest_body_and_live_review_authority(self):
        item=self.provider(self.binding)
        self.assertEqual((item.reference_id,item.content,item.reviewers),
                         ('ref-one',self.content,('synthetic-a','synthetic-b')))
        self.assertGreaterEqual(len(self.reviews),2)
        self.assertEqual(self.reviews[0],self.entry)
        self.allowed=False
        with self.assertRaisesRegex(ValueError,'^reference_material_unavailable$'):self.provider(self.binding)
        self.allowed=True
        (self.body_root/'ref-one.txt').write_text('changed',encoding='utf-8')
        with self.assertRaisesRegex(ValueError,'reference_material_unavailable'):self.provider(self.binding)
        (self.body_root/'ref-one.txt').write_text(self.content,encoding='utf-8')
        self.manifest.write_bytes(self.manifest.read_bytes()+b' ')
        with self.assertRaisesRegex(ValueError,'reference_material_unavailable'):self.provider(self.binding)

    def test_scope_validity_reviewers_policy_and_manifest_fields_are_strict(self):
        for name,value in (('project_id','foreign'),('revision','other'),('issue_id','other')):
            with self.subTest(scope=name),self.assertRaisesRegex(ValueError,'reference_material_unavailable'):
                self.provider(replace(self.binding,**{name:value}))
        for change in (dict(policy='unreviewed'),dict(reference_id='../escape'),dict(reviewers=['a']),
                       dict(reviewers=['a','a']),dict(reviewers=[str(i) for i in range(9)]),
                       dict(expires_at_epoch=int(time.time())-1),dict(not_before_epoch=int(time.time())+20),
                       dict(expires_at_epoch=True),dict(body_path='escape.txt')):
            with self.subTest(change=change):
                self.write_manifest(self.entry|change)
                with self.assertRaisesRegex(ValueError,'reference_material_unavailable'):self.provider(self.binding)

    def test_strings_do_not_prove_review_authority_and_callback_errors_are_private(self):
        def fail(entry):raise OSError('private teacher directory and token')
        for verify in (lambda entry:False,lambda entry:1,fail):
            provider=self.Provider(self.manifest,self.pinned,self.body_root,verify)
            with self.assertRaisesRegex(ValueError,'^reference_material_unavailable$'):provider(self.binding)
        with self.assertRaisesRegex(ValueError,'invalid_material_config'):
            self.Provider(self.manifest,self.pinned,self.body_root,None)

    def test_revocation_during_read_is_rechecked(self):
        self.after=lambda:setattr(self,'allowed',len(self.reviews)<2)
        with self.assertRaisesRegex(ValueError,'reference_material_unavailable'):self.provider(self.binding)

    def test_final_review_callback_cannot_return_body_after_expiry(self):
        from unittest.mock import patch
        self.write_manifest(self.entry|dict(not_before_epoch=50,expires_at_epoch=150))
        clock=[100]
        def verify(entry):
            self.reviews.append(deepcopy(entry))
            if len(self.reviews)==2:clock[0]=200
            return True
        provider=self.Provider(self.manifest,self.pinned,self.body_root,verify)
        with patch('sigflow_edu_agent.host.materials.time.time',side_effect=lambda:clock[0]):
            with self.assertRaisesRegex(ValueError,'reference_material_unavailable'):provider(self.binding)

    def test_symlink_escape_is_rejected(self):
        outside=self.root/'outside.txt';outside.write_text(self.content,encoding='utf-8')
        body=self.body_root/'ref-one.txt';body.unlink()
        try:body.symlink_to(outside)
        except OSError as error:self.skipTest('Windows symlink creation not permitted: '+str(error.winerror))
        with self.assertRaisesRegex(ValueError,'reference_material_unavailable'):self.provider(self.binding)

    def test_pinned_manifest_rejects_ambiguous_json_and_noncanonical_metadata(self):
        for raw in (
            b'{"schema_version":"u2.reviewed-materials.v1","references":[],"references":[]}',
            json.dumps(dict(schema_version='u2.reviewed-materials.v1',references=[self.entry,self.entry])).encode(),
            b'x'*262145):
            self.manifest.write_bytes(raw)
            provider=self.Provider(self.manifest,hashlib.sha256(raw).hexdigest(),self.body_root,self.verify)
            with self.assertRaisesRegex(ValueError,'reference_material_unavailable'):provider(self.binding)

    def test_link_refusal_has_unprivileged_branch_coverage(self):
        from unittest.mock import patch
        # OS symlink exercise above can be unavailable on Windows. This checks
        # the explicit refusal branch without requiring elevation or a fake body.
        with patch.object(Path,'is_symlink',return_value=True):
            with self.assertRaisesRegex(ValueError,'reference_material_unavailable'):self.provider(self.binding)
