"""Production composition; evidence and model quotas are host-owned."""

from ..clients.gateway import GatewayClient, GatewayConfig
from ..context.gateway_report import GatewayReportSource
from ..deepseek import DeepSeekProvider
from ..domain import EvidenceError, ProviderError
from ..loop import TeachingLoop
from ..contracts.plan import build_plan_card
from ..runtime.gateway_session import GatewayTeachingSession, _ObservedSource
from .catalog import AnchorCatalog, load_anchors
from .gateway_evidence import GatewayEvidenceCatalog


class BudgetedProvider:
    def __init__(self, provider, reserve):
        self.provider, self.reserve = provider, reserve

    def generate(self, context):
        try:
            self.reserve()
        except Exception:
            raise ProviderError('model_budget_exceeded') from None
        return self.provider.generate(context)


def _anchor_input(anchors):
    if isinstance(anchors, (AnchorCatalog, GatewayEvidenceCatalog)):
        return anchors
    anchors = tuple(anchors)
    if len(anchors) > 256:
        raise ValueError('invalid_anchor_catalog')
    return anchors


def _select(anchors, project_id, revision, job_id, snapshot_id):
    snapshot = anchors.snapshot() if isinstance(anchors, AnchorCatalog) else anchors
    return tuple(a for a in snapshot if (a.project_id, a.revision, a.job_id, a.snapshot_id) ==
                 (project_id, revision, job_id, snapshot_id))


class _CatalogReportSource:
    """A session keeps its scope while collecting from fresh trusted anchors."""

    def __init__(self, client, binding, anchors, origin):
        self.client, self.binding = client, binding
        self.anchors, self.origin = anchors, origin

    def collect(self, project_id, revision):
        binding = self.binding
        if (project_id, revision) != (binding.project_id, binding.revision):
            raise EvidenceError('stale_report')
        if isinstance(self.anchors, GatewayEvidenceCatalog):
            return self.anchors.collect(binding.job_id, project_id=project_id, revision=revision,
                snapshot_id=binding.snapshot_id, diagnostic_id=binding.diagnostic_id)
        try:
            selected = _select(self.anchors, project_id, revision, binding.job_id, binding.snapshot_id)
        except ValueError:
            raise EvidenceError('unresolved_evidence') from None
        return GatewayReportSource(self.client, job_id=binding.job_id,
            project_id=project_id, revision=revision, snapshot_id=binding.snapshot_id,
            diagnostic_id=binding.diagnostic_id, anchors=selected, origin=self.origin).collect(project_id, revision)


def make_report_resolver(gateway, anchors, *, evidence_origin='tool'):
    """No matching host anchor/diagnostic means no certified observation."""
    anchors = _anchor_input(anchors)
    client = GatewayClient(gateway)
    if isinstance(anchors, GatewayEvidenceCatalog):
        if anchors.gateway != gateway or evidence_origin != 'tool':
            raise ValueError('invalid_gateway_evidence_config')
        def resolve_observation(job_id, *, project_id, revision, snapshot_id):
            return anchors.collect(job_id, project_id=project_id, revision=revision, snapshot_id=snapshot_id)
        return resolve_observation
    def resolve(job_id, *, project_id, revision, snapshot_id):
        selected = _select(anchors, project_id, revision, job_id, snapshot_id)
        if not selected:
            return None
        report = client.job_report(job_id, project_id=project_id, revision=revision, snapshot_id=snapshot_id)
        ids = {a.evidence_id for a in selected}
        diagnostics = [d for d in report.get('diagnostics', []) if type(d) is dict
                       and type(d.get('evidence_refs')) is list and d['evidence_refs']
                       and all(type(ref) is str and ref in ids for ref in d['evidence_refs'])]
        if len(diagnostics) != 1:
            return None
        return GatewayReportSource(client, job_id=job_id, project_id=project_id, revision=revision,
            snapshot_id=snapshot_id, diagnostic_id=diagnostics[0].get('id'), anchors=selected,
            origin=evidence_origin).collect(project_id, revision)
    return resolve


def make_factory(gateway: GatewayConfig, *, anchors=(), evidence_origin='tool', verification=None,
                 course_provider=None,waveform_reader=None):
    if not isinstance(gateway, GatewayConfig) or evidence_origin not in ('tool', 'fixture'):
        raise ValueError('invalid_host_config')
    if course_provider is not None and not callable(getattr(course_provider, 'prepare', None)):
        raise ValueError('invalid_course_provider')
    if waveform_reader is not None and getattr(waveform_reader,'config',None)!=gateway:
        raise ValueError('invalid_waveform_config')
    anchors = _anchor_input(anchors)
    if isinstance(anchors, GatewayEvidenceCatalog) and (anchors.gateway != gateway or evidence_origin != 'tool'):
        raise ValueError('invalid_gateway_evidence_config')
    try:
        provider = DeepSeekProvider.from_settings()
    except ProviderError:
        provider = None

    def factory(binding, request, mode, reserve, *, plan=None, gateway_plan_hash=None):
        if binding.instance_id != gateway.instance_id or mode not in ('rule', 'deepseek'):
            raise ValueError('session_scope_mismatch')
        session_verification = verification
        if request.intent == 'verify':
            if verification is None:
                raise ValueError('verification_unavailable')
            if plan is None or (plan.project_id, plan.revision, plan.snapshot_id) != (
                    binding.project_id, binding.revision, binding.snapshot_id):
                raise ValueError('session_scope_mismatch')
            package = build_plan_card(plan, 'plan-1', 0)
            verification.register_approval(plan, package, gateway_plan_hash)
            session_verification = verification.bind(plan, gateway_plan_hash)
        elif plan is not None or gateway_plan_hash is not None:
            raise ValueError('readonly_intent_required')
        source = _CatalogReportSource(GatewayClient(gateway), binding, anchors, evidence_origin)
        observed = _ObservedSource(source, binding, request)
        budgeted = BudgetedProvider(provider, reserve) if mode == 'deepseek' and provider else None
        return GatewayTeachingSession(TeachingLoop(request, observed, budgeted, plan=plan,
            verification=session_verification, course_provider=course_provider), observed)

    factory.model_available = provider is not None
    factory.model_secrets = (provider.config.api_key,) if provider is not None else ()
    factory.verification_available = verification is not None
    factory.course_enabled = course_provider is not None
    factory.waveform_reader = waveform_reader
    factory.prediction_available = waveform_reader is not None
    return factory
