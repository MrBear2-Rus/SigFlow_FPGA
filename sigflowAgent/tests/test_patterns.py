"""Synthetic host facts test policy routing, never hardware detector accuracy."""
import importlib.util
import unittest
from dataclasses import replace
from sigflow_edu_agent.domain import Evidence, CardContext
from sigflow_edu_agent.context.models import EvidenceAnchor
from sigflow_edu_agent.pedagogy import RuleProvider, checked_card

AVAILABLE = importlib.util.find_spec('sigflow_edu_agent.patterns') is not None
if AVAILABLE:
    from sigflow_edu_agent.patterns import PatternFacts, assess

CASES = (
    ('syntax', ('parser_diagnostic', 'source_location'), 'accepted_language_construct'),
    ('latch', ('storage_observed', 'combinational_goal'), 'intentional_storage'),
    ('width_signedness', ('operand_types', 'expression_width', 'expected_range'), 'intentional_conversion'),
    ('drivers', ('driver_inventory', 'net_kind'), 'legal_resolved_net'),
    ('combinational_feedback', ('dependency_cycle', 'combinational_path'), 'sequential_boundary'),
    ('ports', ('interface_declaration', 'connection_map'), 'intentional_open_port'),
    ('reset', ('reset_contract', 'exact', 'complete', 'all_pages', 'initial_values', 'timescale'), 'contract_allows_behavior'),
    ('simulation_mismatch', ('expected_result', 'observed_result', 'exact', 'complete', 'all_pages', 'initial_values', 'timescale'), 'comparison_matches'),
)

class PatternTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(AVAILABLE, 'versioned pattern registry is not implemented')
        self.anchor = EvidenceAnchor('e', 'p', 'r', 'j', 's', 'a', 'h')

    def pair(self, concept, observations):
        evidence = Evidence('e', 'p', 'r', 'j', concept, True, 'synthetic observation', anchor=self.anchor)
        return evidence, PatternFacts(concept, self.anchor, 'synthetic.v1', observations)

    def test_expired_anchor_cannot_certify_legacy_latch(self):
        evidence, _ = self.pair('latch', ())
        evidence = replace(evidence, combinational_goal=True,
                           anchor=replace(self.anchor, expired=True))
        self.assertEqual(assess(evidence).status, 'insufficient')

    def test_binding_changes_cannot_certify_facts(self):
        evidence, facts = self.pair('syntax', CASES[0][1])
        for field in ('project_id', 'revision', 'job_id', 'artifact_id', 'artifact_hash', 'snapshot_id', 'evidence_id'):
            with self.subTest(field=field):
                self.assertEqual(assess(evidence, replace(facts, anchor=replace(self.anchor, **{field:'other'}))).status, 'insufficient')

    def test_synthetic_source_cannot_certify_real_detector(self):
        evidence, facts = self.pair('syntax', CASES[0][1])
        self.assertEqual(assess(replace(evidence, origin='tool'), facts).status, 'unsupported')
        self.assertEqual(assess(evidence, replace(facts, detector='unknown')).status, 'unsupported')

    def test_malformed_facts_are_denied_without_crash(self):
        evidence, facts = self.pair('syntax', CASES[0][1])
        for bad in ({}, True, replace(facts, observations=['parser_diagnostic']), replace(facts, observations=(True,)), replace(facts, anchor=None), replace(facts, detector=[])):
            with self.subTest(bad=bad):
                self.assertEqual(assess(evidence, bad).status, 'insufficient')

    def test_waveform_precision_cannot_be_omitted(self):
        for concept, observations, _ in CASES[-2:]:
            for key in ('exact', 'complete', 'all_pages', 'initial_values', 'timescale'):
                evidence, facts = self.pair(concept, tuple(x for x in observations if x != key))
                self.assertEqual(assess(evidence, facts).status, 'insufficient')

    def test_default_provider_preserves_latch_and_unknown_is_conservative(self):
        evidence, _ = self.pair('latch', ())
        evidence = replace(evidence, combinational_goal=True)
        for level in (1, 2, 3):
            context = CardContext('why', level, evidence)
            self.assertIsNotNone(checked_card(RuleProvider().generate(context), context, 'rule'))
        unknown = replace(evidence, concept='unknown', summary='latch parser syntax reset')
        self.assertEqual(assess(unknown).status, 'unsupported')
        self.assertIn('不足', RuleProvider().generate(CardContext('why', 1, unknown))['hint'])

    def test_unsupported_real_facts_and_bad_levels_never_produce_card(self):
        evidence, facts = self.pair('syntax', CASES[0][1])
        provider = RuleProvider(facts_source=lambda _: facts)
        for level in (1, 2, 3):
            context = CardContext('why', level, evidence)
            self.assertIsNotNone(checked_card(provider.generate(context), context, 'rule'))
        for level in (-1, 0, 4, True):
            context = CardContext('why', level, evidence)
            self.assertIsNone(checked_card(provider.generate(context), context, 'rule'))


def make_case(concept, observations, exclusion, mode):
    def test(self):
        evidence, facts = self.pair(concept, observations)
        if mode == 'legal':
            facts = replace(facts, observations=observations + (exclusion,))
        elif mode == 'insufficient':
            facts = replace(facts, observations=())
        result = assess(evidence, facts)
        self.assertEqual(result.status, {'positive':'supported', 'legal':'excluded', 'insufficient':'insufficient'}[mode])
    return test

for concept, observations, exclusion in CASES:
    for mode in ('positive', 'legal', 'insufficient'):
        setattr(PatternTests, f'test_{concept}_{mode}', make_case(concept, observations, exclusion, mode))
