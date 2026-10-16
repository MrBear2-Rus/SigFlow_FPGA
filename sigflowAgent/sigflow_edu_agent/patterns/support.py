"""Production support is evidence provenance, never a log-keyword root cause."""
from . import PATTERNS

_MISSING={
 'syntax':'Pinned language mode, parser diagnostic and source location are required.',
 'width_signedness':'WIDTH alone does not establish operand types, expected range or unintended conversion.',
 'drivers':'MULTIDRIVEN alone does not distinguish illegal drivers from legal resolved nets.',
 'combinational_feedback':'UNOPTFLAT alone does not prove a hardware cycle without a sequential boundary.',
 'ports':'A trusted interface declaration and complete connection map are required.',
 'reset':'An independently supplied reset contract and controlled clock/reset stimulus are required.',
 'simulation_mismatch':'Explicit point comparison is available; general dynamic testbench expectations are not yet supplied.',
}

def support_matrix():
    return [dict(concept=name,detector=pattern.detector,production_detector=name=='latch',
        required_facts=list(pattern.requirements),exclusions=list(pattern.exclusions),
        bounded_observation='explicit_point_comparison' if name=='simulation_mismatch' else
                            'certified_storage_report' if name=='latch' else 'report_label_only',
        limitation=_MISSING.get(name,'Storage observation still requires an independently stated combinational goal.'))
            for name,pattern in PATTERNS.items()]
