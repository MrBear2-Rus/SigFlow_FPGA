"""Trusted-host composition of a read-only, fixed-report teaching session.

Bindings label host-authenticated scope; they are not authentication credentials.
No learner history, grant persistence or Gateway execution is implemented here.
"""

from dataclasses import asdict, dataclass
import hashlib
import json
import re

from ..clients.gateway import GatewayClient, GatewayConfig
from ..context.gateway_report import GatewayReportSource
from ..deepseek import DeepSeekProvider
from ..domain import EvidenceError, RunRequest
from ..loop import TeachingLoop
from ..pedagogy import valid_evidence
from .session import TeachingSession


@dataclass(frozen=True)
class SessionBinding:
    instance_id: str
    session_id: str
    learner_id: str
    project_id: str
    revision: str
    snapshot_id: str
    job_id: str
    diagnostic_id: str
    issue_id: str
    policy: str = 'l1-l3-l4-denied'

    def __post_init__(self):
        for value in (self.instance_id, self.session_id, self.learner_id, self.project_id,
                      self.revision, self.snapshot_id, self.job_id, self.diagnostic_id, self.issue_id):
            if (type(value) is not str or len(value) > 256
                    or re.fullmatch(r'[A-Za-z0-9_-]+(?:[.:][A-Za-z0-9_-]+)*', value) is None):
                raise ValueError('invalid_session_binding')
        if self.policy != 'l1-l3-l4-denied':
            raise ValueError('unsupported_policy')


@dataclass(frozen=True)
class Occurrence:
    binding: SessionBinding
    evidence_id: str
    evidence_sha256: str
    origin: str


class _ObservedSource:
    def __init__(self, source, binding, request):
        self.source, self.binding, self.request = source, binding, request
        self.record = None

    def collect(self, project_id, revision):
        evidence = self.source.collect(project_id, revision)
        if not valid_evidence(evidence, self.request):
            raise EvidenceError('invalid_report')
        digest = hashlib.sha256(json.dumps(asdict(evidence), sort_keys=True,
                                          separators=(',', ':'), ensure_ascii=True).encode()).hexdigest()
        record = Occurrence(self.binding, evidence.evidence_id, digest, evidence.origin)
        if self.record is not None and record != self.record:
            # A fixed Job/diagnostic must not silently replace its observation.
            raise EvidenceError('stale_report')
        self.record = record
        return evidence


class GatewayTeachingSession(TeachingSession):
    def __init__(self, loop, observed_source):
        super().__init__(loop)
        self._observed_source = observed_source

    @property
    def binding(self):
        return self._observed_source.binding

    @property
    def occurrences(self):
        with self._lock:
            record = self._observed_source.record
            return () if record is None else (record,)


def create_gateway_session(*, binding: SessionBinding, request: RunRequest,
                           gateway: GatewayConfig, anchors, mode='deepseek',
                           evidence_origin='tool', combinational_goal=False,
                           context_items=(), max_steps=32, max_model_calls=4):
    """Construct without network I/O; host drives advance/host_action afterwards.

    DeepSeek receives question and bounded evidence when E4 is reached. Explicit
    rule mode avoids model requests. Host owns authentication and object routing.
    """
    if not isinstance(binding, SessionBinding) or not isinstance(request, RunRequest) or not isinstance(gateway, GatewayConfig):
        raise ValueError('invalid_session_binding')
    if ((request.project_id, request.revision, request.issue_id) !=
            (binding.project_id, binding.revision, binding.issue_id)
            or gateway.instance_id != binding.instance_id):
        raise ValueError('session_scope_mismatch')
    if request.intent not in ('explain', 'diagnose'):
        raise ValueError('readonly_intent_required')
    if mode not in ('deepseek', 'rule'):
        raise ValueError('invalid_provider_mode')
    source = GatewayReportSource(
        GatewayClient(gateway), job_id=binding.job_id, project_id=binding.project_id,
        revision=binding.revision, snapshot_id=binding.snapshot_id,
        diagnostic_id=binding.diagnostic_id, anchors=anchors,
        origin=evidence_origin, combinational_goal=combinational_goal, context_items=context_items)
    observed = _ObservedSource(source, binding, request)
    provider = DeepSeekProvider.from_env() if mode == 'deepseek' else None
    loop = TeachingLoop(request, observed, provider, max_steps=max_steps, max_model_calls=max_model_calls)
    return GatewayTeachingSession(loop, observed)
