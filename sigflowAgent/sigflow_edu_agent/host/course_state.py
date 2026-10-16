"""Metadata-only view of currently admitted course background, not card citations."""
from collections import Counter
from dataclasses import asdict


def course_state(loop):
    state = loop.state
    diagnostics = state.course_diagnostics
    retrieval = diagnostics.get('retrieval_status', state.course_status if state.course_status in (
        'disabled', 'unavailable', 'no_match') else 'ok')
    return dict(status=state.course_status, answerability='not_assessed',
        retrieval_status=retrieval, packing_status=state.course_status,
        references=[asdict(ref) for ref in loop._course_refs],
        omitted=[asdict(item) for item in state.course_omitted],
        omission_counts=dict(Counter(item.reason for item in state.course_omitted)),
        missing_required_sources=list(diagnostics.get('missing_required_sources', ())),
        missing_required_evidence=list(diagnostics.get('missing_required_evidence', ())))


def quarantined_course(status):
    return dict(status=status, answerability='not_assessed', references=[], omitted=[],
                retrieval_status='not_checked', packing_status='not_checked',
                omission_counts={}, missing_required_sources=[], missing_required_evidence=[])
