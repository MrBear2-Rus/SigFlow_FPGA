"""Prepared L4 protocol port, not enabled by the current teaching HTTP host.

A reviewed content producer and durable publication transaction must be supplied
before activation. No receipt is issued here, and no model creates a reference.
Challenges are process-local; a restart requires a new explicit request.
"""
from dataclasses import dataclass
import hashlib,re,time
from uuid import uuid4
from ..runtime.gateway_session import SessionBinding

POLICY='u2-reviewed-reference.v1'

@dataclass(frozen=True)
class ReviewedReference:
    reference_id:str
    content:str
    sha256:str
    reviewers:tuple[str,...]

    def __post_init__(self):
        if (type(self.reference_id) is not str or re.fullmatch('[A-Za-z0-9_-]{1,128}',self.reference_id) is None
                or type(self.content) is not str or not 1<=len(self.content.encode('utf-8'))<=32768
                or hashlib.sha256(self.content.encode('utf-8')).hexdigest()!=self.sha256
                or type(self.reviewers) is not tuple or not 2<=len(self.reviewers)<=8
                or not all(type(r) is str and re.fullmatch('[A-Za-z0-9_-]{1,128}',r) for r in self.reviewers)
                or len(set(self.reviewers))!=len(self.reviewers)):
            raise ValueError('invalid_reviewed_reference')

class ReferenceGate:
    def __init__(self,binding,run_id,provider):
        if (not isinstance(binding,SessionBinding) or type(run_id) is not str
                or re.fullmatch('[A-Za-z0-9_-]{1,128}',run_id) is None or not callable(provider)):
            raise ValueError('invalid_reference_config')
        self.binding,self.run_id,self.provider=binding,run_id,provider
        self.invalidate()

    def invalidate(self):
        self._challenge=None;self._material=None;self._deadline=0

    def request(self,state_version):
        self.invalidate()
        if type(state_version) is not int or not 0<=state_version<2**63:raise ValueError('invalid_state_version')
        item=self.provider(self.binding)
        if not isinstance(item,ReviewedReference):raise ValueError('reference_material_unavailable')
        item.__post_init__()
        self._material=item
        self._challenge=dict(challenge_id=uuid4().hex,action_id='reference-'+uuid4().hex,
            session_id=self.binding.session_id,project_id=self.binding.project_id,revision=self.binding.revision,
            learner_id=self.binding.learner_id,issue_id=self.binding.issue_id,run_id=self.run_id,
            reference_id=item.reference_id,reference_sha256=item.sha256,level='l4',policy=POLICY,
            state_version=str(state_version),expires_at_epoch=str(int(time.time()+300)))
        self._deadline=time.monotonic()+300
        return dict(self._challenge)

    def confirm(self,receipt_id,challenge_id,state_version,port):
        c=self._challenge
        if (c is None or type(state_version) is not int or str(state_version)!=c['state_version']
                or challenge_id!=c['challenge_id'] or time.monotonic()>=self._deadline):
            raise ValueError('reference_challenge_stale')
        current=self.provider(self.binding)
        if current!=self._material or not isinstance(current,ReviewedReference):
            self.invalidate();raise ValueError('reference_material_unavailable')
        current.__post_init__()
        receipt=port.lookup(receipt_id)
        fields=('session_id','project_id','revision','challenge_id','action_id','policy','state_version','level')
        if (type(receipt) is not dict or receipt.get('receipt_id')!=receipt_id
                or any(receipt.get(k)!=c[k] for k in fields) or receipt.get('status')!='active'):
            raise ValueError('receipt_scope_mismatch')
        # Never return content before the authoritative atomic consume succeeds.
        acknowledged=port.consume(receipt_id,c['project_id'],self.run_id,c['action_id'],state_version)
        if (acknowledged.get('receipt_id')!=receipt_id or acknowledged.get('action_id')!=c['action_id']
                or acknowledged.get('consumed') is not True or acknowledged.get('status')!='consumed'):
            raise ValueError('receipt_scope_mismatch')
        result=dict(reference_id=current.reference_id,sha256=current.sha256,content=current.content,
                    policy=POLICY,reviewers=list(current.reviewers))
        self.invalidate()
        return result
