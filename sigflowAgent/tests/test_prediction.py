"""Prediction is supplied before a real transport query; data are HTTP fixtures."""
import importlib.util
import unittest
from unittest.mock import patch
from _wave_gateway import WaveGateway
from sigflow_edu_agent.clients.waveform import GatewayWaveformReader


class PredictionTests(unittest.TestCase):
    def setUp(self):
        self.gateway=WaveGateway();self.addCleanup(self.gateway.close)
        self.payload=dict(artifact_id='wave1',signal_id=0,start_tick='10',end_tick='20',sample_tick='12')

    def controller(self):
        self.assertIsNotNone(importlib.util.find_spec('sigflow_edu_agent.host.prediction'),'prediction controller missing')
        from sigflow_edu_agent.host.prediction import PredictionController
        return PredictionController(GatewayWaveformReader(self.gateway.config),self.gateway.binding)

    def test_request_reads_only_metadata_then_submit_compares_explicit_expected(self):
        c=self.controller();challenge=c.begin(self.payload)
        self.assertEqual(self.gateway.query_count,0)
        self.assertEqual(challenge['status'],'awaiting_prediction')
        self.assertNotIn('observed',challenge)
        result=c.submit(dict(challenge_id=challenge['challenge_id'],expected='1'))
        self.assertEqual((result['status'],result['outcome'],result['expected'],result['observed']),('observed','matches','1','1'))
        self.assertEqual(result['job_id'],'job1');self.assertEqual(result['artifact_hash'],'a'*64)
        self.assertEqual(result['origin'],'tool');self.assertEqual(self.gateway.query_count,1)

    def test_mismatch_and_four_state_unknown_remain_finite_point_conclusions(self):
        c=self.controller()
        for point,expected,outcome in (('12','0','differs'),('20','0','inconclusive')):
            challenge=c.begin(self.payload|dict(sample_tick=point))
            self.assertEqual(c.submit(dict(challenge_id=challenge['challenge_id'],expected=expected))['outcome'],outcome)

    def test_unknown_or_malformed_expected_is_denied_before_value_query(self):
        c=self.controller();challenge=c.begin(self.payload)
        for expected in ('x','z','11','',True,None):
            with self.subTest(expected=expected),self.assertRaises(ValueError):
                c.submit(dict(challenge_id=challenge['challenge_id'],expected=expected))
        self.assertEqual(self.gateway.query_count,0)

    def test_replaced_invalidated_and_reused_challenges_do_not_query(self):
        c=self.controller();old=c.begin(self.payload);new=c.begin(self.payload)
        with self.assertRaises(ValueError):c.submit(dict(challenge_id=old['challenge_id'],expected='1'))
        c.submit(dict(challenge_id=new['challenge_id'],expected='1'))
        with self.assertRaises(ValueError):c.submit(dict(challenge_id=new['challenge_id'],expected='1'))
        count=self.gateway.query_count
        new=c.begin(self.payload);c.invalidate('context_changed')
        with self.assertRaises(ValueError):c.submit(dict(challenge_id=new['challenge_id'],expected='1'))
        self.assertEqual(self.gateway.query_count,count)

    def test_incomplete_or_changed_wave_is_unavailable_not_guessed(self):
        c=self.controller();challenge=c.begin(self.payload)
        self.gateway.page['completeness']='partial'
        result=c.submit(dict(challenge_id=challenge['challenge_id'],expected='1'))
        self.assertEqual(result['status'],'unavailable');self.assertNotIn('observed',result)
        self.gateway.page['completeness']='exact'
        challenge=c.begin(self.payload)
        self.gateway.report['artifacts'][0]['sha256']='b'*64;self.gateway.metadata['sha256']='b'*64
        result=c.submit(dict(challenge_id=challenge['challenge_id'],expected='1'))
        self.assertEqual(result['status'],'unavailable');self.assertNotIn('observed',result)

    def test_caller_cannot_inject_page_anchor_or_contract(self):
        c=self.controller()
        for key in ('page','anchor','service_contract','job_id','path'):
            with self.subTest(key=key),self.assertRaises(ValueError):c.begin(self.payload|{key:'untrusted'})
        self.assertEqual(self.gateway.query_count,0)

    def test_expired_challenge_is_denied_without_query(self):
        c=self.controller();challenge=c.begin(self.payload)
        with patch('sigflow_edu_agent.host.prediction.time.monotonic',return_value=c._deadline):
            with self.assertRaisesRegex(ValueError,'prediction_challenge_stale'):
                c.submit(dict(challenge_id=challenge['challenge_id'],expected='1'))
        self.assertEqual(self.gateway.query_count,0)

    def test_uint64_sample_round_trip_uses_decimal_text(self):
        maximum='18446744073709551615'
        self.gateway.catalog['time_range']=dict(start_tick='0',end_tick=maximum)
        self.gateway.page.update(start_tick=maximum,end_tick=maximum,transition_count=1,
            signals=[dict(signal_id=0,initial_value='0',initial_known=True,transitions=[dict(tick=maximum,value='1')])])
        c=self.controller();challenge=c.begin(self.payload|dict(start_tick=maximum,end_tick=maximum,sample_tick=maximum))
        result=c.submit(dict(challenge_id=challenge['challenge_id'],expected='1'))
        self.assertEqual((result['outcome'],result['sample_tick']),('matches',maximum))
