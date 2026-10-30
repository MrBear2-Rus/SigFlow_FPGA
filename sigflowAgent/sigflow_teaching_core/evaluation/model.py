"""Repeated, bounded L1-L3 candidate evaluation; semantic quality is unscored."""

from copy import deepcopy
from dataclasses import asdict, dataclass
import hashlib
import json
import re

from ..deepseek import DeepSeekProvider, SYSTEM_PROMPT
from ..domain import Card, CardContext, Evidence, ProviderError, RunRequest
from ..patterns import assess
from ..pedagogy import RuleProvider, checked_card, valid_evidence
from . import _source_hash


def _encode(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True, allow_nan=False)


def _hash(value):
    return hashlib.sha256(_encode(value).encode()).hexdigest()


def _version(value):
    return type(value) is str and re.fullmatch(r'[A-Za-z0-9_.:-]{1,128}', value) is not None


@dataclass(frozen=True)
class ModelCase:
    case_id: str
    question: str
    level: int
    evidence: Evidence

    def __post_init__(self):
        if (type(self.case_id) is not str or re.fullmatch(r'case-[a-z0-9-]{1,64}', self.case_id) is None
                or type(self.level) is not int or self.level not in (1, 2, 3)
                or type(self.question) is not str or not self.question.strip() or len(self.question) > 4096
                or not isinstance(self.evidence, Evidence)):
            raise ValueError('invalid_model_case')
        try:
            self.question.encode('utf-8')
        except UnicodeError:
            raise ValueError('invalid_model_case') from None


@dataclass(frozen=True)
class ReviewSample:
    sample_id: str
    case_id: str
    repetition: int
    evidence_sha256: str
    evidence_origin: str
    card: Card


@dataclass(frozen=True)
class EvaluationBatch:
    _report_json: str
    review_samples: tuple[ReviewSample, ...]

    @property
    def report(self):
        return json.loads(self._report_json)

    @property
    def report_sha256(self):
        return hashlib.sha256(self._report_json.encode()).hexdigest()


def run_model_evaluation(cases, provider, *, suite_version, mode='deepseek', repeats=3,
                         max_model_calls=256, runtime_version='self-developed-v1', rag_version='not_used'):
    """Evaluate frozen host cases. Only explicit deepseek mode labels real calls.

    Transport substitutes must use mock mode, even with a DeepSeekProvider.
    Raw candidates never enter reports or review samples. Final checked text
    exists only in memory samples and still needs human semantic review.
    """
    if (mode not in ('deepseek', 'mock', 'rule') or type(repeats) is not int or not 3 <= repeats <= 10
            or type(max_model_calls) is not int or not 0 <= max_model_calls <= 512
            or not all(_version(v) for v in (suite_version, runtime_version, rag_version))
            or not isinstance(cases, (tuple, list)) or not 1 <= len(cases) <= 256):
        raise ValueError('invalid_evaluation_config')
    if ((mode == 'deepseek' and not isinstance(provider, DeepSeekProvider))
            or (mode == 'rule' and provider is not None)
            or (mode != 'rule' and not callable(getattr(provider, 'generate', None)))):
        raise ValueError('invalid_evaluation_provider')
    frozen = deepcopy(tuple(cases))
    ids = set()
    for case in frozen:
        if not isinstance(case, ModelCase):
            raise ValueError('invalid_model_case')
        case.__post_init__()
        if case.case_id in ids:
            raise ValueError('duplicate_model_case')
        ids.add(case.case_id)
        if (rag_version == 'not_used' and isinstance(case.evidence.context_items, (tuple, list))
                and any(getattr(i, 'kind', None) == 'course' for i in case.evidence.context_items)):
            raise ValueError('rag_version_required')
    try:
        suite_json = _encode([asdict(c) for c in frozen])
        if len(suite_json) > 4 * 1024 * 1024:
            raise ValueError()
    except (TypeError, ValueError):
        raise ValueError('invalid_model_case') from None
    is_deepseek = isinstance(provider, DeepSeekProvider)
    versions = dict(runtime=runtime_version, workflow='u2-e0-e7', policy='l1-l3-l4-denied',
                    prompt_sha256=hashlib.sha256(SYSTEM_PROMPT.encode()).hexdigest() if is_deepseek else 'not_used',
                    model=provider.config.model if is_deepseek else mode, rag=rag_version,
                    source_sha256=_source_hash())
    parameters = dict(repeats=repeats, max_model_calls=max_model_calls, temperature=None,
                      temperature_status='provider_default_unavailable' if is_deepseek else 'not_used',
                      max_tokens=provider.config.max_tokens if is_deepseek else None,
                      base_url=provider.config.base_url if is_deepseek else None,
                      timeout=provider.config.timeout if is_deepseek else None,
                      stream=False if is_deepseek else None)
    rows, samples, calls = [], [], 0
    for case in frozen:
        evidence_hash = _hash(asdict(case.evidence))
        try:
            request = RunRequest(case.evidence.project_id, case.evidence.revision, case.case_id, case.question)
            valid = valid_evidence(case.evidence, request)
        except (TypeError, ValueError):
            valid = False
        assessment = assess(case.evidence) if valid else None
        for repetition in range(1, repeats + 1):
            sample_id = f'{case.case_id}:r{repetition}'
            context = CardContext(case.question, case.level, deepcopy(case.evidence))
            card, reason, rejected, row_calls = None, '', 0, 0
            if not valid:
                reason = 'evidence_rejected'
            else:
                if assessment.status != 'supported':
                    reason = 'unsupported_pattern'
                elif mode == 'rule':
                    reason = 'rule_mode'
                else:
                    for attempt in range(2):
                        if calls >= max_model_calls:
                            reason = 'model_budget_exhausted'
                            break
                        calls += 1
                        row_calls += 1
                        trial = CardContext(context.question, context.level, deepcopy(context.evidence), attempt == 1)
                        try:
                            candidate = provider.generate(trial)
                            card = checked_card(candidate, context, 'model')
                        except ProviderError as error:
                            reason = error.code
                            if error.code == 'invalid_output':
                                rejected += 1
                                continue
                            break
                        except Exception:
                            reason = 'model_unavailable'
                            break
                        if card is not None:
                            reason = 'checked_model_output'
                            break
                        rejected += 1
                        reason = 'invalid_output'
                if card is None:
                    card = checked_card(RuleProvider().generate(context), context, 'rule')
                if card is not None:
                    samples.append(ReviewSample(sample_id, case.case_id, repetition,
                                                evidence_hash, case.evidence.origin, card))
            rows.append(dict(sample_id=sample_id, case_id=case.case_id, repetition=repetition,
                             level=case.level, evidence_origin=case.evidence.origin if
                             type(case.evidence.origin) is str and case.evidence.origin in ('fixture', 'tool', 'rule')
                             else 'unavailable', evidence_sha256=evidence_hash,
                             input_sha256=_hash(asdict(context)), output_sha256=_hash(asdict(card)) if card else None,
                             producer=card.producer if card else None, model_calls=row_calls,
                             rejected_candidates=rejected, reason=reason))
    variability = []
    for case in frozen:
        selected = [r for r in rows if r['case_id'] == case.case_id]
        variability.append(dict(case_id=case.case_id,
                                distinct_output_count=len({r['output_sha256'] for r in selected if r['output_sha256']}),
                                attempted_repetitions=sum(r['model_calls'] > 0 for r in selected),
                                model_outputs=sum(r['producer'] == 'model' for r in selected),
                                distinct_reason_count=len({r['reason'] for r in selected})))
    report = dict(schema_version='edu.model-evaluation.v1', suite_version=suite_version,
                  suite_sha256=hashlib.sha256(suite_json.encode()).hexdigest(), versions=versions, parameters=parameters,
                  execution_kind={'deepseek': 'real_model', 'mock': 'mock_provider', 'rule': 'rule'}[mode],
                  real_model='attempted' if mode == 'deepseek' and calls else 'not_run',
                  real_tools='not_run', teacher_review='pending', semantic_quality='not_scored',
                  model_repetitions_complete=all(v['attempted_repetitions'] == repeats for v in variability),
                  token_usage='unavailable', model_service_version='unavailable',
                  total=len(rows), model_calls=calls, model_passed=sum(r['producer'] == 'model' for r in rows),
                  rule_fallbacks=sum(r['producer'] == 'rule' for r in rows),
                  budget_exhausted=sum(r['reason'] == 'model_budget_exhausted' for r in rows),
                  evidence_rejected=sum(r['reason'] == 'evidence_rejected' for r in rows),
                  variability=variability, results=rows)
    return EvaluationBatch(_encode(report), tuple(samples))
