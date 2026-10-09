from dataclasses import replace
import json
from pathlib import Path
import unittest

from sigflow_edu_agent.context.models import ContextItem, EvidenceAnchor, SourceRef
from sigflow_edu_agent.context.builder import build_context
from sigflow_edu_agent.context.report import ReportEvidenceSource
from sigflow_edu_agent.domain import EvidenceError, RunRequest, Status
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.pedagogy import RuleProvider


def sample():
    root = Path(__file__).resolve().parents[2]
    cases = json.loads((root / 'contracts/edu-agent/v1/fixtures/golden.json').read_text(encoding='utf-8'))
    report = next(c['document'] for c in cases if c['name'] == 'job-report-view.core')
    diag = next(c['document'] for c in cases if c['name'] == 'diagnostic.latch')
    report['diagnostics'] = [diag]
    report['artifacts'] = [{'id': 'artifact-1', 'hash': 'sha256:' + 'a' * 64}]
    anchor = EvidenceAnchor('ev-1', 'prj-1', 'rev-12', 'job-21', 'snap-12',
                            'artifact-1', 'sha256:' + 'a' * 64)
    return report, anchor


def source(report=None, anchor=None, **kwargs):
    default_report, default_anchor = sample()
    return ReportEvidenceSource(report if report is not None else default_report,
                                [anchor if anchor is not None else default_anchor],
                                job_id='job-21', diagnostic_id='diag-1',
                                combinational_goal=True, **kwargs)


class ContextTests(unittest.TestCase):
    def test_priority_and_omission_are_deterministic_and_do_not_split_text(self):
        items = (ContextItem('rtl', 'rtl', 'p', 'r', '12345'),
                 ContextItem('sel', 'selection', 'p', 'r', '中文'),
                 ContextItem('rep', 'report', 'p', 'r', 'ok'))
        result = build_context(items, 'p', 'r', max_bytes=8, max_tokens=100)
        self.assertEqual([item.item_id for item in result.items], ['sel', 'rep'])
        self.assertEqual(result.omitted[0].item_id, 'rtl')
        self.assertEqual(result.omitted[0].reason, 'byte_budget')
        self.assertEqual(result.bytes_used, 8)

    def test_token_budget_and_mixed_revision_are_checked(self):
        item = ContextItem('x', 'selection', 'p', 'r', 'abc')
        self.assertEqual(build_context((item,), 'p', 'r', max_tokens=2).omitted[0].reason, 'token_budget')
        with self.assertRaisesRegex(EvidenceError, 'mixed_context'):
            build_context((replace(item, revision='old'),), 'p', 'r')

    def test_duplicates_and_invalid_unicode_are_rejected_before_pack(self):
        item = ContextItem('x', 'rtl', 'p', 'r', 'abc')
        for items in ((item, item), (replace(item, text='\ud800'),)):
            with self.assertRaises(EvidenceError):
                build_context(items, 'p', 'r')

    def test_golden_report_binds_specific_job_and_preserves_origin(self):
        adapter = source()
        evidence = adapter.collect('prj-1', 'rev-12')
        self.assertEqual(evidence.job_id, 'job-21')
        self.assertEqual(evidence.evidence_id, 'ev-1')
        self.assertEqual(evidence.origin, 'fixture')
        self.assertEqual(evidence.confidence_kind, 'tool')
        self.assertEqual(evidence.anchor.snapshot_id, 'snap-12')
        state = TeachingLoop(RunRequest('prj-1', 'rev-12', 'i', '解释锁存器'), adapter).run_until_pause()
        self.assertEqual(state.status, Status.WAITING_STUDENT)
        self.assertEqual(state.cards[0].evidence_ids, ('ev-1',))

    def test_report_refuses_old_incomplete_legacy_or_hypothesis(self):
        report, anchor = sample()
        variants = []
        for changes in ({'revision': 'old'}, {'completeness': 'partial'},
                        {'completeness': 'legacy_unverified'}, {'input_fingerprint': None},
                        {'schema_version': 'edu.jobreport.v99'}, {'job_id': 'other'}):
            variants.append(dict(report, **changes))
        variants.append(dict(report, diagnostics=[dict(report['diagnostics'][0], confidence_kind='hypothesis')]))
        for bad in variants:
            with self.subTest(bad=bad):
                loop = TeachingLoop(RunRequest('prj-1', 'rev-12', 'i', '解释'), source(bad, anchor))
                self.assertEqual(loop.run_until_pause().status, Status.WAITING_EVIDENCE)
                self.assertEqual(loop.state.cards, [])

    def test_every_evidence_ref_must_resolve_and_have_same_binding(self):
        report, anchor = sample()
        for bad_anchor in (replace(anchor, expired=True), replace(anchor, job_id='old'),
                           replace(anchor, revision='old'), replace(anchor, artifact_hash='wrong')):
            with self.subTest(anchor=bad_anchor), self.assertRaises(EvidenceError):
                source(report, bad_anchor).collect('prj-1', 'rev-12')
        report['diagnostics'][0]['evidence_refs'].append('unresolved')
        with self.assertRaisesRegex(EvidenceError, 'unresolved_evidence'):
            source(report, anchor).collect('prj-1', 'rev-12')

    def test_source_ref_range_and_version_are_checked(self):
        report, anchor = sample()
        location = SourceRef('prj-1', 'rev-12', 'src1', 'sha256:' + 'b' * 64, 2, 5)
        good = replace(anchor, source_ref=location)
        self.assertIsNotNone(source(report, good).collect('prj-1', 'rev-12'))
        for bad in (replace(location, start_line=0), replace(location, end_line=1), replace(location, revision='old')):
            with self.assertRaises(EvidenceError):
                source(report, replace(anchor, source_ref=bad)).collect('prj-1', 'rev-12')

    def test_report_is_snapshotted_and_context_reaches_provider(self):
        report, anchor = sample()
        adapter = source(report, anchor, context_items=(ContextItem('sel', 'selection', 'prj-1', 'rev-12', '局部RTL'),))
        report['job_id'] = 'mutated'
        evidence = adapter.collect('prj-1', 'rev-12')
        self.assertEqual(evidence.job_id, 'job-21')
        self.assertEqual(evidence.context_items[0].text, '局部RTL')
        seen = []
        class CapturingProvider:
            def generate(self, context):
                seen.append(context)
                return RuleProvider().generate(context)
        state = TeachingLoop(RunRequest('prj-1', 'rev-12', 'i', '解释'), adapter,
                             CapturingProvider()).run_until_pause()
        self.assertEqual(state.status, Status.WAITING_STUDENT)
        self.assertEqual(seen[0].evidence.context_items[0].text, '局部RTL')


if __name__ == '__main__':
    unittest.main()
