"""Offline development evaluation with bounded suites and redacted reports."""

import hashlib
import json
from pathlib import Path
import re

from .probes import PROBES, run_probe


def load_suite(path):
    try:
        with Path(path).open('rb') as stream:
            raw = stream.read(65537)
        if len(raw) > 65536:
            raise ValueError
        return json.loads(raw)
    except (OSError, ValueError, UnicodeError):
        raise ValueError('invalid_suite') from None


def _validate(suite):
    if (type(suite) is not dict or set(suite) != {'suite_version', 'cases'}
            or suite.get('suite_version') != 'development-v1'
            or type(suite.get('cases')) is not list or not 1 <= len(suite['cases']) <= 256):
        raise ValueError('invalid_suite')
    seen = set()
    for case in suite['cases']:
        if (type(case) is not dict or set(case) != {'case_id', 'probe', 'expected'}
                or type(case['case_id']) is not str
                or re.fullmatch(r'dev-[0-9]{3}', case['case_id']) is None
                or case['case_id'] in seen or type(case['probe']) is not str
                or case['probe'] not in PROBES or type(case['expected']) is not bool):
            raise ValueError('invalid_suite')
        seen.add(case['case_id'])


def _source_hash():
    root = Path(__file__).resolve().parents[1]
    digest = hashlib.sha256()
    for path in sorted(root.rglob('*.py')):
        digest.update(path.relative_to(root).as_posix().encode('utf-8'))
        digest.update(b'\0')
        digest.update(path.read_bytes().replace(b'\r\n', b'\n'))
        digest.update(b'\0')
    return digest.hexdigest()


def evaluate(suite):
    _validate(suite)
    rows = []
    for case in suite['cases']:
        try:
            actual = run_probe(case['probe'])
            if type(actual) is not bool:
                actual, passed, reason = None, False, 'invalid_probe_result'
            else:
                passed = actual is case['expected']
                reason = 'matched' if passed else 'expectation_mismatch'
        except Exception:
            # Exceptions can carry external data. Never echo their text.
            actual, passed, reason = None, False, 'probe_error'
        kind = 'mock_provider' if case['probe'] in {'model_budget', 'repair_limit', 'provider_error'} else 'rule'
        rows.append({'case_id': case['case_id'], 'probe': case['probe'], 'execution_kind': kind,
                     'expected': case['expected'], 'actual': actual,
                     'passed': passed, 'reason': reason})
    passed = sum(row['passed'] for row in rows)
    groups = {}
    for kind in ('rule', 'mock_provider', 'real_tool', 'real_model', 'teacher_review'):
        selected = [row for row in rows if row['execution_kind'] == kind]
        count = sum(row['passed'] for row in selected)
        groups[kind] = {'total': len(selected), 'passed': count, 'failed': len(selected) - count,
                        'status': 'run' if selected else 'not_run'}
    encoded = json.dumps(suite, sort_keys=True, separators=(',', ':')).encode()
    return {'schema_version': 'edu.development-evaluation.v1',
            'suite_version': suite['suite_version'], 'suite_sha256': hashlib.sha256(encoded).hexdigest(),
            'versions': {'source_sha256': _source_hash(), 'workflow': 'u2-e0-e7',
                         'policy': 'l1-l3-l4-denied', 'prompt': 'not_used',
                         'upstream': 'not_used', 'model': 'not_used', 'rag': 'not_used'},
            'evidence_type': 'synthetic_development', 'real_model': 'not_run',
            'real_tools': 'not_run', 'teacher_review': 'not_run', 'temperature': None,
            'by_execution_kind': groups,
            'total': len(rows), 'passed': passed, 'failed': len(rows) - passed, 'results': rows}
