"""Typed waveform boundary, with an explicit HTTP fixture instead of EDA."""
from dataclasses import replace
import importlib.util
import unittest

from _wave_gateway import WaveGateway
from sigflow_edu_agent.context.waveform import WaveformError,check_waveform
from sigflow_edu_agent.clients.gateway import GatewayError


class GatewayWaveformTests(unittest.TestCase):
    def setUp(self):
        self.gateway=WaveGateway();self.addCleanup(self.gateway.close)

    def reader(self):
        self.assertIsNotNone(importlib.util.find_spec('sigflow_edu_agent.clients.waveform'),
                             'trusted waveform reader missing')
        from sigflow_edu_agent.clients.waveform import GatewayWaveformReader
        return GatewayWaveformReader(self.gateway.config)

    def test_describe_exposes_signal_metadata_without_querying_values(self):
        description=self.reader().describe(self.gateway.binding,'wave1')
        self.assertEqual((description.signals[0].full_name,description.signals[0].width),('TOP.q',1))
        self.assertEqual(self.gateway.query_count,0)
        self.assertEqual(description.anchor.artifact_hash,'a'*64)

    def test_complete_authenticated_window_keeps_tool_origin_and_binding(self):
        wave=self.reader().read(self.gateway.binding,'wave1',signal_ids=(0,),start_tick='10',end_tick='20')
        self.assertEqual((wave.origin,wave.value_at(0,'12')),('tool','1'))
        self.assertEqual((wave.anchor.job_id,wave.anchor.snapshot_id),('job1','snap1'))
        self.assertGreaterEqual(self.gateway.metadata_count,2)
        self.assertEqual([r[2] for r in self.gateway.requests if r[0]=='POST'],
                         [dict(signals=[0],start_tick='10',end_tick='20')])

    def test_old_or_unknown_gateway_contract_stays_closed(self):
        reader=self.reader()
        for contract in (None,'old-v0',True):
            self.gateway.contract=contract
            with self.subTest(contract=contract),self.assertRaises((GatewayError,WaveformError)):
                reader.read(self.gateway.binding,'wave1',signal_ids=(0,),start_tick='10',end_tick='20')
        self.assertEqual(self.gateway.query_count,0)

    def test_partial_last_signal_never_certifies_even_without_cursor(self):
        self.gateway.page.update(completeness='partial',has_more=False,next_cursor=None)
        with self.assertRaisesRegex(WaveformError,'incomplete_waveform'):
            self.reader().read(self.gateway.binding,'wave1',signal_ids=(0,),start_tick='10',end_tick='20')

    def test_report_scope_and_artifact_hash_tampering_are_denied(self):
        reader=self.reader()
        for binding in (replace(self.gateway.binding,revision='r2'),replace(self.gateway.binding,job_id='job2')):
            with self.assertRaises((GatewayError,WaveformError)):reader.describe(binding,'wave1')
        self.gateway.metadata['sha256']='b'*64
        with self.assertRaises((GatewayError,WaveformError)):reader.describe(self.gateway.binding,'wave1')
        self.assertEqual(self.gateway.query_count,0)

    def test_file_changed_during_query_cannot_publish_window(self):
        self.gateway.mutate_after_query=True
        with self.assertRaises((GatewayError,WaveformError)):
            self.reader().read(self.gateway.binding,'wave1',signal_ids=(0,),start_tick='10',end_tick='20')

    def test_invalid_selection_and_out_of_artifact_range_rejected_before_query(self):
        reader=self.reader()
        for changes in (dict(signal_ids=(True,)),dict(signal_ids=(1,)),dict(signal_ids=(0,0)),
                        dict(start_tick='-1'),dict(end_tick='21'),dict(end_tick='18446744073709551616')):
            args=dict(signal_ids=(0,),start_tick='10',end_tick='20')|changes
            with self.subTest(changes=changes),self.assertRaises((GatewayError,WaveformError)):
                reader.read(self.gateway.binding,'wave1',**args)
        self.assertEqual(self.gateway.query_count,0)

    def test_tool_checker_requires_explicit_trusted_service_contract(self):
        reader=self.reader()
        description=reader.describe(self.gateway.binding,'wave1')
        args=dict(catalog=self.gateway.catalog,page=self.gateway.page,anchor=description.anchor,
                  project_id='p1',revision='r1',job_id='job1',snapshot_id='snap1',signal_ids=(0,),
                  start_tick='10',end_tick='20',origin='tool')
        with self.assertRaises(WaveformError):check_waveform(**args)
        wave=check_waveform(**args,service_contract='whole-signals-v1')
        self.assertEqual(wave.origin,'tool')

    def test_metadata_budget_includes_signals_beyond_gateway_default_page(self):
        self.gateway.catalog.update(total=300,signals=[dict(signal_id=i,full_name='TOP.s'+str(i),width=1) for i in range(300)])
        description=self.reader().describe(self.gateway.binding,'wave1')
        self.assertEqual(len(description.signals),300)
        self.assertEqual(description.signals[-1].signal_id,299)
