from copy import deepcopy
from dataclasses import replace
import json
from pathlib import Path
import unittest

from sigflow_edu_agent.context.models import EvidenceAnchor
from sigflow_edu_agent.context.waveform import WaveformError, check_waveform, compare_sample


def data():
    catalog = dict(artifact_id='wave-1', timescale='1ns', total=1, has_more=False,
                   signals=[dict(signal_id=0, full_name='TOP.q', width=4)])
    page = dict(artifact_id='wave-1', start_tick='10', end_tick='20', timescale='1ns',
                signals=[dict(signal_id=0, initial_value='0010', initial_known=True,
                              transitions=[dict(tick='12', value='0011'), dict(tick='20', value='xxxx')])],
                completeness='exact', has_more=False, next_cursor=None, transition_count=2, omitted_signals=0)
    anchor = EvidenceAnchor('wave-ev', 'p1', 'r1', 'job-1', 'snap-1', 'wave-1', 'a'*64)
    return catalog, page, anchor


class WaveformTests(unittest.TestCase):
    def setUp(self):
        self.catalog, self.page, self.anchor = data()

    def check(self, **changes):
        args = dict(catalog=self.catalog, page=self.page, anchor=self.anchor, project_id='p1',
                    revision='r1', job_id='job-1', snapshot_id='snap-1',
                    signal_ids=(0,), start_tick='10', end_tick='20', origin='fixture')
        return check_waveform(**(args | changes))

    def test_inclusive_sampling_uses_previous_value_and_retains_unknown(self):
        wave = self.check()
        self.assertEqual(wave.value_at(0, '10'), '0010')
        self.assertEqual(wave.value_at(0, '12'), '0011')
        self.assertEqual(wave.value_at(0, '20'), 'xxxx')
        self.assertEqual(wave.timescale, '1ns')
        result = compare_sample(wave, 0, '12', '0011')
        self.assertEqual(result.status, 'matches')
        self.assertEqual(result.anchor, self.anchor)
        self.assertEqual(compare_sample(wave, 0, '13', '0000').status, 'differs')
        self.assertEqual(compare_sample(wave, 0, '20', '0000').status, 'inconclusive')

    def test_partial_or_unfinished_pages_never_certify(self):
        for change in (dict(completeness='partial'), dict(has_more=True), dict(next_cursor='cursor'),
                       dict(omitted_signals=1), dict(transition_count=3)):
            with self.subTest(change=change), self.assertRaises(WaveformError):
                self.check(page=self.page | change)

    def test_source_scope_and_hash_cannot_be_fabricated_by_page(self):
        for change in (dict(project_id='other'), dict(revision='old'), dict(job_id='other'),
                       dict(snapshot_id='old'), dict(artifact_id='other'), dict(artifact_hash='bad'), dict(expired=True)):
            with self.subTest(change=change), self.assertRaises(WaveformError):
                self.check(anchor=replace(self.anchor, **change))
        with self.assertRaises(WaveformError):
            self.check(page=self.page | dict(artifact_id='other'))

    def test_uint64_ticks_above_double_precision_are_exact(self):
        start, end = '18446744073709551614', '18446744073709551615'
        page = deepcopy(self.page)
        page.update(start_tick=start, end_tick=end, transition_count=1)
        page['signals'][0]['transitions'] = [dict(tick=end, value='0001')]
        wave = self.check(page=page, start_tick=start, end_tick=end)
        self.assertEqual(wave.value_at(0, start), '0010')
        self.assertEqual(wave.value_at(0, end), '0001')
        for tick in ('18446744073709551616', '-1', 10, True, '1.0', '1e3'):
            with self.assertRaises(WaveformError):
                self.check(start_tick=tick)

    def test_zero_start_has_no_assumed_initial_value(self):
        page = deepcopy(self.page)
        page.update(start_tick='0')
        page['signals'][0].update(initial_value=None, initial_known=False)
        wave = self.check(page=page, start_tick='0')
        self.assertIsNone(wave.value_at(0, '0'))
        self.assertEqual(compare_sample(wave, 0, '0', '0000').status, 'inconclusive')
        page['signals'][0].update(initial_value='0000', initial_known=True)
        with self.assertRaises(WaveformError):
            self.check(page=page, start_tick='0')

    def test_xz_initial_known_means_exists_not_binary_known(self):
        self.page['signals'][0]['initial_value'] = 'z'
        wave = self.check()
        self.assertEqual(wave.value_at(0, '10'), 'zzzz')
        self.assertEqual(compare_sample(wave, 0, '10', '0000').status, 'inconclusive')

    def test_metadata_timescale_selection_and_width_must_agree(self):
        for change in (dict(timescale='10ns'), dict(timescale=None), dict(timescale='0ns')):
            with self.assertRaises(WaveformError):
                self.check(catalog=self.catalog | change)
        for change in (dict(width=None), dict(width=0), dict(width=True), dict(full_name='')):
            bad = self.catalog | dict(signals=[self.catalog['signals'][0] | change])
            with self.assertRaises(WaveformError):
                self.check(catalog=bad)
        for ids in ((), (0, 0), (1,), (True,), tuple(range(17))):
            with self.assertRaises(WaveformError):
                self.check(signal_ids=ids)

    def test_transitions_must_be_ordered_and_in_window(self):
        for events in ([dict(tick='9', value='0000')], [dict(tick='21', value='0000')],
                       [dict(tick='14', value='0000'), dict(tick='13', value='0001')]):
            bad = deepcopy(self.page)
            bad['signals'][0]['transitions'] = events
            bad['transition_count'] = len(events)
            with self.assertRaises(WaveformError):
                self.check(page=bad)

    def test_same_tick_events_keep_wire_order(self):
        self.page['signals'][0]['transitions'] = [dict(tick='12', value='0000'), dict(tick='12', value='0001')]
        self.assertEqual(self.check().value_at(0, '12'), '0001')

    def test_binary_and_sized_hex_preserve_width_without_truncation(self):
        self.page['signals'][0]['initial_value'] = "4'h2"
        self.assertEqual(self.check().value_at(0, '10'), '0010')
        for value in ('11111', "8'h02", 'real:1.5', '', '2', 'secret text'):
            self.page['signals'][0]['initial_value'] = value
            with self.assertRaises(WaveformError) as caught:
                self.check()
            self.assertEqual(str(caught.exception), 'unsupported_value')

    def test_mutation_after_check_cannot_change_window(self):
        wave = self.check()
        self.page['signals'][0]['initial_value'] = '1111'
        self.catalog['signals'][0]['full_name'] = 'changed'
        self.assertEqual(wave.value_at(0, '10'), '0010')
        self.assertEqual(wave.signals[0].full_name, 'TOP.q')

    def test_prediction_cannot_use_unknown_expected_or_outside_range(self):
        wave = self.check()
        for tick, expected in (('9', '0000'), ('21', '0000'), ('12', 'xxxx'), ('12', '11111')):
            with self.assertRaises(WaveformError):
                compare_sample(wave, 0, tick, expected)

    def test_contract_exact_fixture_works_with_catalog_metadata(self):
        path = Path(__file__).resolve().parents[2] / 'contracts/edu-agent/v1/fixtures/golden.json'
        cases = json.loads(path.read_text(encoding='utf-8'))
        page = next(c['document'] for c in cases if c['name'] == 'wave-query.exact')
        catalog = dict(artifact_id=page['artifact_id'], timescale=page['timescale'], total=1,
                       signals=[dict(signal_id=0, full_name='TOP.clk', width=1)])
        anchor = replace(self.anchor, artifact_id=page['artifact_id'])
        wave = self.check(page=page, catalog=catalog, anchor=anchor, start_tick='0', end_tick='100')
        self.assertEqual(wave.value_at(0, '0'), '1')

    def test_limits_reject_excess_transitions_and_expanded_payload(self):
        for width, event_count in ((4, 10001), (4096, 600)):
            catalog, page = deepcopy(self.catalog), deepcopy(self.page)
            catalog['signals'][0]['width'] = width
            page['signals'][0]['transitions'] = [dict(tick='12', value='0')] * event_count
            page['transition_count'] = event_count
            with self.assertRaises(WaveformError):
                self.check(catalog=catalog, page=page)

    def test_boolean_width_metadata_is_not_integer_width(self):
        catalog, page = deepcopy(self.catalog), deepcopy(self.page)
        catalog['signals'][0]['width'] = 1
        page['signals'][0].update(width=True, initial_value='0', transitions=[])
        page['transition_count'] = 0
        with self.assertRaises(WaveformError):
            self.check(catalog=catalog, page=page)

    def test_real_gateway_completeness_is_gated_until_upstream_repair(self):
        with self.assertRaisesRegex(WaveformError, 'incomplete_waveform'):
            self.check(origin='tool')
        with self.assertRaises(WaveformError):
            self.check(origin='model')
        result = compare_sample(self.check(), 0, '12', '0011')
        self.assertEqual(result.origin, 'fixture')

    def test_invalid_utf8_metadata_raises_only_redacted_waveform_error(self):
        self.catalog['signals'][0]['full_name'] = '\ud800'
        with self.assertRaises(WaveformError):
            self.check()
