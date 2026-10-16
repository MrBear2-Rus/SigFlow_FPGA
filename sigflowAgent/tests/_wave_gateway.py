"""Authenticated loopback DTO fixture; never counted as real EDA evidence."""
from copy import deepcopy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from threading import Thread
from urllib.parse import urlsplit,parse_qs

from sigflow_edu_agent.clients.gateway import GatewayConfig
from sigflow_edu_agent.runtime.gateway_session import SessionBinding


class WaveGateway:
    def __init__(self):
        self.requests=[]
        self.contract='whole-signals-v1'
        self.query_count=0
        self.metadata_count=0
        self.mutate_after_query=False
        self.binding=SessionBinding('inst-wave','session-wave','learner1','p1','r1','snap1','job1','job-report','issue1')
        self.report=dict(schema_version='edu.jobreport.v1',project_id='p1',revision='r1',snapshot_id='snap1',
            job_id='job1',origin='core',state='Succeeded',exit_code=0,completeness='complete',
            capability='eda.sim.run',plugin_id='eda-sim-verilator-run',input_fingerprint='trusted-input',
            artifacts=[dict(artifact_id='wave1',sha256='a'*64,schema='eda.wave.vcd.v1',role='primary')],
            diagnostics=[],metrics={})
        self.metadata=dict(artifact_id='wave1',project_id='p1',revision='r1',job_id='job1',sha256='a'*64,
            schema='eda.wave.vcd.v1',role='waveform',media_type='application/vcd',size='300',inline_readable=False)
        self.catalog=dict(artifact_id='wave1',timescale='1ns',total=1,has_more=False,next_cursor=None,
            time_range=dict(start_tick='0',end_tick='20'),
            signals=[dict(signal_id=0,name='q',scope='TOP',full_name='TOP.q',width=1,id_code='!')])
        self.page=dict(artifact_id='wave1',timescale='1ns',start_tick='10',end_tick='20',
            completeness='exact',has_more=False,next_cursor=None,transition_count=2,omitted_signals=0,
            signals=[dict(signal_id=0,initial_value='0',initial_known=True,
                          transitions=[dict(tick='12',value='1'),dict(tick='20',value='x')])])
        owner=self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def do_GET(self): self.handle_request()
            def do_POST(self): self.handle_request()
            def handle_request(self):
                body=None
                if self.command=='POST':body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                owner.requests.append((self.command,self.path,body))
                status=200
                identity=dict(instance_id='inst-wave',protocol_version='edu.api.v1',edition='edu',build='fixture')
                if self.path!='/api/v1/health' and self.headers.get('Authorization')!='Bearer wave-token':
                    status=401; data=None
                elif self.path=='/api/v1/health': data=identity
                elif self.path=='/api/v1/capabilities':
                    data=identity|dict(capabilities=[])
                    if owner.contract is not None:data['waveform_query_contract']=owner.contract
                elif self.path=='/api/v1/jobs/job1/report':data=deepcopy(owner.report)
                elif self.path=='/api/v1/artifacts/wave1':
                    owner.metadata_count+=1
                    data=deepcopy(owner.metadata)
                    if owner.mutate_after_query and owner.query_count:data['sha256']='b'*64
                elif urlsplit(self.path).path=='/api/v1/waves/wave1/signals':
                    data=deepcopy(owner.catalog)
                    limit=int(parse_qs(urlsplit(self.path).query).get('limit',['256'])[0])
                    data['signals']=data['signals'][:limit]
                    if len(data['signals'])<data['total']:
                        data.update(has_more=True,next_cursor='o:'+str(len(data['signals'])))
                elif self.path=='/api/v1/waves/wave1/query' and self.command=='POST':
                    owner.query_count+=1;data=deepcopy(owner.page)
                else:status=404;data=None
                envelope=dict(schema_version='edu.api.v1',request_id='req-wave',trace_id='trace-wave')
                if status==200:envelope['data']=data
                else:envelope['error']=dict(code='NOT_FOUND' if status==404 else 'UNAUTHENTICATED',message='fixture',retryable=False)
                raw=json.dumps(envelope).encode()
                self.send_response(status);self.send_header('Content-Type','application/json')
                self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
        self.server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        self.thread=Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        self.config=GatewayConfig(self.server.server_port,'wave-token','inst-wave')

    def close(self):
        self.server.shutdown();self.server.server_close();self.thread.join(2)
