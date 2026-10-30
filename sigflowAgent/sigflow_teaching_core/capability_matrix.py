"""Product capability coverage shared by U2 capability reporting and docs.

The matrix is deliberately descriptive.  A ``ready`` Gateway capability is
still required before an execution plan can run; unavailable rows never turn
into synthetic jobs or a model-authored workaround.
"""

from copy import deepcopy
import re


_BASE = (
    dict(id='eda.synth', category='execution', status='gateway_dependent',
         enabled=True, requires=('gateway_plugin', 'immutable_snapshot', 'grant')),
    dict(id='eda.sim.build', category='execution', status='gateway_dependent',
         enabled=True, requires=('gateway_plugin', 'immutable_snapshot', 'grant')),
    dict(id='eda.sim.run', category='execution', status='gateway_dependent',
         enabled=True, requires=('successful_build', 'immutable_snapshot', 'grant')),
    dict(id='edu.course.rag', category='teaching', status='runtime_configured',
         enabled=True, requires=('course_database', 'retrieval_budget')),
    dict(id='edu.prediction', category='teaching', status='gateway_dependent',
         enabled=True, requires=('whole_signals_v1', 'explicit_expectation')),
    dict(id='edu.reference.l4', category='teaching', status='disabled', enabled=False,
         requires=('native_dual_confirmation', 'reviewed_reference',
                   'version_bound_receipt', 'formal_teaching_action')),
    dict(id='eda.pnr', category='hardware', status='unavailable', enabled=False,
         requires=('native_confirmation', 'toolchain', 'hardware_acceptance')),
    dict(id='eda.pack', category='hardware', status='unavailable', enabled=False,
         requires=('native_confirmation', 'toolchain', 'hardware_acceptance')),
    dict(id='eda.flash', category='hardware', status='unavailable', enabled=False,
         requires=('native_confirmation', 'board_target', 'hardware_acceptance')),
    dict(id='eda.debug', category='hardware', status='unavailable', enabled=False,
         requires=('native_confirmation', 'debug_probe', 'hardware_acceptance')),
    dict(id='eda.patch', category='engineering', status='unavailable', enabled=False,
         requires=('reviewed_patch', 'native_confirmation', 'rollback')),
)


def _safe_reason(value):
    value = str(value or '').replace('\r', ' ').replace('\n', ' ')
    return re.sub(r'(?:[A-Za-z]:[\\/]|\\\\|(?<![\w:])/(?:[^\s/]+/))[^\s"<>]*', '[path]', value)[:512]


def coverage(*, course_readiness=None, prediction_available=False,
             gateway_capabilities=None):
    """Return a redacted, current capability coverage view.

    Gateway readiness is copied into the three execution rows.  Course and
    prediction rows retain their product-level policy and are never inferred
    from a model key or from the dsh/OpenCode environment.
    """
    result = [deepcopy(item) for item in _BASE]
    by_id = {item['id']: item for item in result}
    for item in gateway_capabilities or ():
        if not isinstance(item, dict) or item.get('id') not in by_id:
            continue
        row = by_id[item['id']]
        if row['category'] != 'execution':
            continue
        if item.get('disabled') is True:
            row.update(enabled=False, status='disabled', reason=_safe_reason(item.get('reason') or 'policy_disabled'))
        elif item.get('ready') is True:
            row.update(enabled=True, status='ready', reason='gateway_ready')
        else:
            row.update(enabled=False, status='unavailable', reason=_safe_reason(item.get('reason') or 'gateway_unavailable'))
    course = by_id['edu.course.rag']
    if course_readiness:
        course.update(enabled=(course_readiness.get('database') in ('ready', 'fixture')),
                      status=course_readiness.get('retrieval', 'unavailable'),
                      reason=_safe_reason(course_readiness.get('reason', 'course_unavailable')))
    prediction = by_id['edu.prediction']
    if not prediction_available:
        prediction.update(enabled=False, status='unavailable', reason='waveform_contract_unavailable')
    return result


def public_matrix(**kwargs):
    """Return JSON-safe rows without exposing internal paths or evidence."""
    rows = coverage(**kwargs)
    for row in rows:
        row['requires'] = list(row['requires'])
    return rows
