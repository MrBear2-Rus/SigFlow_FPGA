"""Bounded operator inputs; pinned manifests declare provenance, not acceptance."""
from dataclasses import dataclass, fields
import hashlib
import json
from pathlib import Path
import re

from ..context.models import ContextItem, EvidenceAnchor, Omission, SourceRef
from ..domain import Evidence
from .model import ModelCase, _version


def _invalid():
    raise ValueError('invalid_frozen_model_suite')


def _pairs(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            _invalid()
        value[key] = item
    return value


def read_json(path, max_bytes):
    try:
        with Path(path).open('rb') as stream:
            raw = stream.read(max_bytes+1)
        if not raw or len(raw) > max_bytes:
            _invalid()
        return raw, json.loads(raw, object_pairs_hook=_pairs, parse_constant=lambda _: _invalid())
    except (OSError, ValueError, UnicodeError, RecursionError):
        _invalid()


def exact(value, cls):
    if type(value) is not dict or set(value) != {field.name for field in fields(cls)}:
        _invalid()
    return dict(value)


def text(value, maximum, *, empty=False):
    if type(value) is not str or (not empty and not value.strip()):
        _invalid()
    try:
        if len(value.encode('utf-8')) > maximum:
            _invalid()
    except UnicodeError:
        _invalid()


def _items(value, maximum):
    if type(value) is not list or len(value) > maximum:
        _invalid()
    return value


def _source(value):
    value = exact(value, SourceRef)
    for name in ('project_id', 'revision', 'source_id', 'file_hash'):
        text(value[name], 8192)
    for name in ('start_line', 'end_line', 'start_column', 'end_column'):
        number = value[name]
        if number is None and name.endswith('column'):
            continue
        if type(number) is not int or not 1 <= number <= 2**31-1:
            _invalid()
    return SourceRef(**value)


def _anchor(value):
    value = exact(value, EvidenceAnchor)
    for name in set(value)-{'source_ref', 'expired'}:
        text(value[name], 8192)
    if type(value['expired']) is not bool:
        _invalid()
    if value['source_ref'] is not None:
        value['source_ref'] = _source(value['source_ref'])
    return EvidenceAnchor(**value)


def decode_evidence(value):
    value = exact(value, Evidence)
    for name in ('evidence_id', 'project_id', 'revision', 'job_id', 'concept', 'summary'):
        text(value[name], 16384)
    if (type(value['complete']) is not bool or type(value['combinational_goal']) is not bool
            or value['origin'] not in ('fixture', 'tool', 'rule')
            or value['confidence_kind'] not in ('tool', 'rule', 'hypothesis')
            or type(value['course_diagnostics']) is not dict):
        _invalid()
    if value['anchor'] is not None:
        value['anchor'] = _anchor(value['anchor'])
    items = []
    for item in _items(value['context_items'], 32):
        item = exact(item, ContextItem)
        for entry in item.values():
            text(entry, 16384)
        items.append(ContextItem(**item))
    omissions = []
    for item in _items(value['omitted'], 128):
        item = exact(item, Omission)
        for entry in item.values():
            text(entry, 256)
        omissions.append(Omission(**item))
    value.update(context_items=tuple(items), omitted=tuple(omissions))
    return Evidence(**value)


@dataclass(frozen=True)
class FrozenModelSuite:
    suite_version: str
    cases: tuple[ModelCase, ...]
    reviewers: tuple[str, ...]
    manifest_sha256: str
    suite_sha256: str


def load_frozen_model_suite(manifest_path, cases_path, *, expected_manifest_sha256):
    """Only caller-selected paths are opened; malformed evidence may test rejection."""
    try:
        if type(expected_manifest_sha256) is not str or re.fullmatch('[a-f0-9]{64}', expected_manifest_sha256) is None:
            _invalid()
        raw, manifest = read_json(manifest_path, 16384)
        if (hashlib.sha256(raw).hexdigest() != expected_manifest_sha256 or type(manifest) is not dict
                or set(manifest) != {'schema_version', 'suite_version', 'suite_sha256', 'case_count', 'reviewers'}
                or manifest['schema_version'] != 'edu.frozen-model-suite.v1'
                or not _version(manifest['suite_version']) or type(manifest['case_count']) is not int
                or not 1 <= manifest['case_count'] <= 256 or type(manifest['suite_sha256']) is not str
                or re.fullmatch('[a-f0-9]{64}', manifest['suite_sha256']) is None):
            _invalid()
        reviewers = _items(manifest['reviewers'], 2)
        if (len(reviewers) != 2 or any(type(r) is not str or
                re.fullmatch('reviewer-[a-z0-9-]{1,64}', r) is None for r in reviewers)
                or len(set(reviewers)) != 2):
            _invalid()
        raw, rows = read_json(cases_path, 4*1024*1024)
        if (hashlib.sha256(raw).hexdigest() != manifest['suite_sha256'] or type(rows) is not list
                or len(rows) != manifest['case_count']):
            _invalid()
        cases, seen = [], set()
        for row in rows:
            row = exact(row, ModelCase)
            row['evidence'] = decode_evidence(row['evidence'])
            case = ModelCase(**row)
            if case.case_id in seen:
                _invalid()
            seen.add(case.case_id)
            cases.append(case)
        return FrozenModelSuite(manifest['suite_version'], tuple(cases), tuple(reviewers),
                                expected_manifest_sha256, manifest['suite_sha256'])
    except (ValueError, TypeError, KeyError, RecursionError, OverflowError):
        _invalid()
