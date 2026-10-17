"""Authenticated, scoped, single-page wave reading. No paths or EDA execution."""
from dataclasses import dataclass
import hashlib
import json
import re

from .gateway import GatewayClient,GatewayConfig,GatewayError
from ..context.models import EvidenceAnchor
from ..context.waveform import check_waveform,WaveformError,_tick,_signal_id,_text
from ..runtime.gateway_session import SessionBinding

CONTRACT='whole-signals-v1'


@dataclass(frozen=True)
class SignalMetadata:
    signal_id:int
    full_name:str
    width:int


@dataclass(frozen=True)
class WaveDescription:
    anchor:EvidenceAnchor
    timescale:str
    start_tick:int
    end_tick:int
    signals:tuple[SignalMetadata,...]
    total:int

    def catalog(self):
        return dict(artifact_id=self.anchor.artifact_id,timescale=self.timescale,total=self.total,
                    signals=[dict(signal_id=s.signal_id,full_name=s.full_name,width=s.width) for s in self.signals])


class GatewayWaveformReader:
    def __init__(self,config:GatewayConfig):
        if not isinstance(config,GatewayConfig):raise ValueError('invalid_waveform_config')
        self.config=config;self.client=GatewayClient(config)

    def describe(self,binding:SessionBinding,artifact_id):
        if not isinstance(binding,SessionBinding) or binding.instance_id!=self.config.instance_id:
            raise GatewayError('INSTANCE_MISMATCH')
        self.client._identifier(artifact_id)
        self.client.health()
        capabilities=self.client._get('capabilities')
        if capabilities.get('waveform_query_contract')!=CONTRACT:
            raise WaveformError('incomplete_waveform')
        report=self.client.job_report(binding.job_id,project_id=binding.project_id,
                                      revision=binding.revision,snapshot_id=binding.snapshot_id)
        if (report.get('completeness')!='complete' or report.get('state')!='Succeeded'
                or type(report.get('exit_code')) is not int or report['exit_code']!=0
                or report.get('capability')!='eda.sim.run' or report.get('plugin_id')!='eda-sim-verilator-run'):
            raise WaveformError('incomplete_waveform')
        artifacts=report.get('artifacts')
        if type(artifacts) is not list or not 1<=len(artifacts)<=16:
            raise WaveformError('incomplete_waveform')
        selected=[a for a in artifacts if type(a) is dict and a.get('artifact_id')==artifact_id]
        if len(selected)!=1 or selected[0].get('schema')!='eda.wave.vcd.v1':
            raise WaveformError('stale_waveform')
        metadata=self.client.artifact_metadata(artifact_id,project_id=binding.project_id,
            revision=binding.revision,job_id=binding.job_id,sha256=selected[0].get('sha256'))
        if metadata['schema']!='eda.wave.vcd.v1' or int(metadata['size'])==0:
            raise WaveformError('incomplete_waveform')
        material=[binding.instance_id,binding.project_id,binding.revision,binding.snapshot_id,
                  binding.job_id,artifact_id,metadata['sha256']]
        evidence_id='wave-'+hashlib.sha256(json.dumps(material,separators=(',',':')).encode()).hexdigest()
        anchor=EvidenceAnchor(evidence_id,binding.project_id,binding.revision,binding.job_id,
                              binding.snapshot_id,artifact_id,metadata['sha256'])
        catalog=self.client._exchange('GET','/api/v1/waves/'+artifact_id+'/signals?limit=4096')
        rows=catalog.get('signals');timescale=catalog.get('timescale');time_range=catalog.get('time_range')
        if (catalog.get('artifact_id')!=artifact_id or type(rows) is not list or not 1<=len(rows)<=4096
                or type(catalog.get('total')) is not int or catalog['total']<len(rows)
                or type(catalog.get('has_more')) is not bool
                or type(timescale) is not str or re.fullmatch(r'(?:1|10|100)(?:fs|ps|ns|us|ms|s)',timescale) is None
                or type(time_range) is not dict or set(time_range)!={'start_tick','end_tick'}):
            raise WaveformError()
        start,end=_tick(time_range['start_tick']),_tick(time_range['end_tick'])
        if start>end:raise WaveformError()
        signals=[];seen=set()
        for row in rows:
            if (type(row) is not dict or not _signal_id(row.get('signal_id')) or row['signal_id'] in seen
                    or not _text(row.get('full_name')) or type(row.get('width')) is not int
                    or not 1<=row['width']<=4096):raise WaveformError()
            seen.add(row['signal_id']);signals.append(SignalMetadata(row['signal_id'],row['full_name'],row['width']))
        return WaveDescription(anchor,timescale,start,end,tuple(signals),catalog['total'])

    def read(self,binding,artifact_id,*,signal_ids,start_tick,end_tick):
        start,end=_tick(start_tick),_tick(end_tick)
        if (type(signal_ids) is not tuple or not 1<=len(signal_ids)<=16
                or not all(_signal_id(s) for s in signal_ids) or len(set(signal_ids))!=len(signal_ids)):
            raise WaveformError()
        description=self.describe(binding,artifact_id)
        if (not description.start_tick<=start<=end<=description.end_tick
                or not set(signal_ids)<={s.signal_id for s in description.signals}):raise WaveformError()
        page=self.client._exchange('POST','/api/v1/waves/'+artifact_id+'/query',
            dict(signals=list(signal_ids),start_tick=start_tick,end_tick=end_tick),max_bytes=2*1024*1024)
        window=check_waveform(catalog=description.catalog(),page=page,anchor=description.anchor,
            project_id=binding.project_id,revision=binding.revision,job_id=binding.job_id,
            snapshot_id=binding.snapshot_id,signal_ids=signal_ids,start_tick=start_tick,end_tick=end_tick,
            origin='tool',service_contract=CONTRACT)
        self.client.artifact_metadata(artifact_id,project_id=binding.project_id,revision=binding.revision,
                                     job_id=binding.job_id,sha256=description.anchor.artifact_hash)
        return window
