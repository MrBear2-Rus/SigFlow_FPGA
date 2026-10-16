"""Protocol fixtures do not represent reviewed course answers or a GUI signoff."""
import hashlib,unittest
from unittest.mock import patch
from _wave_gateway import WaveGateway

class ReferenceProtocolTests(unittest.TestCase):
    def setUp(self):
        from sigflow_edu_agent.host.reference import ReferenceGate,ReviewedReference
        self.g=WaveGateway();self.addCleanup(self.g.close)
        self.content='Fixture teaching reference, not a course answer.'
        self.item=ReviewedReference('ref1',self.content,hashlib.sha256(self.content.encode()).hexdigest(),('reviewer1','reviewer2'))
        self.gate=ReferenceGate(self.g.binding,'run1',lambda binding:self.item)

    def test_missing_reviewed_material_never_creates_a_challenge(self):
        from sigflow_edu_agent.host.reference import ReferenceGate
        gate=ReferenceGate(self.g.binding,'run1',lambda binding:None)
        with self.assertRaisesRegex(ValueError,'reference_material_unavailable'):gate.request(3)

    def test_gate_binds_receipt_and_consumes_before_returning_reference(self):
        challenge=self.gate.request(3)
        self.assertNotIn('content',challenge)
        receipt={k:challenge[k] for k in ('session_id','project_id','revision','challenge_id','action_id','policy','state_version')}
        receipt.update(receipt_id='ui-receipt-1',level='l4',status='active')
        class Port:
            consumed=False
            def lookup(self,rid):return receipt
            def consume(inner,rid,project,run,action,version):
                self.assertEqual((project,run,action,version),('p1','run1',challenge['action_id'],3))
                inner.consumed=True
                return dict(receipt_id=rid,action_id=action,consumed=True,replayed=False,status='consumed')
        port=Port();result=self.gate.confirm('ui-receipt-1',challenge['challenge_id'],3,port)
        self.assertTrue(port.consumed);self.assertEqual(result['content'],self.content)
        with self.assertRaisesRegex(ValueError,'reference_challenge_stale'):
            self.gate.confirm('ui-receipt-1',challenge['challenge_id'],3,port)

    def test_wrong_scope_policy_action_and_version_are_not_consumed(self):
        challenge=self.gate.request(3)
        base={k:challenge[k] for k in ('session_id','project_id','revision','challenge_id','action_id','policy','state_version')}
        base.update(receipt_id='ui-receipt-1',level='l4',status='active')
        class Port:
            def lookup(inner,rid):return inner.value
            def consume(*args):self.fail('invalid receipt consumed')
        port=Port()
        for key in ('session_id','project_id','revision','challenge_id','action_id','policy','state_version','level','status'):
            port.value=base|{key:'other'}
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,'receipt_scope_mismatch'):
                self.gate.confirm('ui-receipt-1',challenge['challenge_id'],3,port)

    def test_expiry_invalidation_and_material_change_fail_closed(self):
        from sigflow_edu_agent.host.reference import ReferenceGate
        for change in ('expiry','invalidate','material'):
            gate=ReferenceGate(self.g.binding,'run1',lambda binding:self.item)
            challenge=gate.request(3)
            if change=='expiry':gate._deadline=0
            if change=='invalidate':gate.invalidate()
            if change=='material':self.item=None
            with self.subTest(change=change),self.assertRaises(ValueError):
                gate.confirm('ui-receipt-1',challenge['challenge_id'],3,None)

    def test_receipt_client_has_no_issuance_route_and_checks_core_identity(self):
        from sigflow_edu_agent.clients.receipts import GatewayReceiptClient
        client=GatewayReceiptClient(self.g.config)
        self.assertFalse(hasattr(client,'issue'))
        with patch.object(client.client,'health',side_effect=ValueError('wrong_instance')):
            with self.assertRaisesRegex(ValueError,'wrong_instance'):client.lookup('ui-receipt-1')
        for rid in ('../ui','x/y','http://other'):
            with self.assertRaises(ValueError):client.lookup(rid)
