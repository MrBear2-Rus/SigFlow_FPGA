"""Trusted teaching host. Student JSONL/HTTP APIs never receive private plans.

Deploy this process and its course directory on the trusted host. Local desktop
mode is educational blindness, not protection against the owner of that machine.
"""
import argparse,copy,hashlib,hmac,json,os,queue,re,secrets,subprocess,sys,threading,uuid
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from cryptography.hazmat.primitives import serialization
from build_virtual_project import read,write
ROOT=Path(__file__).resolve().parents[2]
CAPABILITIES=['virtual.session.read','virtual.evidence.query','teaching.hint.request','teaching.grade.submit']
def bounce_for_seed(profile,seed):
    result=copy.deepcopy(profile);result['rng']['seed']=seed
    for f in result['faults']:
        if f['effect']['type']=='button_bounce':
            # Independent deterministic stream-derived bounded jitter. All edges
            # remain in the signed lesson's duration and preserve ordering.
            material=('sigflow.pcg32-v1\n'+str(int(seed))+'\n'+f['id']).encode();digest=hashlib.sha256(material).digest();state=0;inc=((int.from_bytes(digest[8:16],'little')<<1)|1)&((1<<64)-1)
            def next32():
                nonlocal state
                old=state;state=(old*6364136223846793005+inc)&((1<<64)-1);x=(((old>>18)^old)>>27)&0xffffffff;rot=old>>59;return ((x>>rot)|(x<<((-rot)&31)))&0xffffffff
            next32();state=(state+int.from_bytes(digest[:8],'little'))&((1<<64)-1);next32()
            edges=f['effect']['relative_edges']
            for edge in edges[1:-1]:edge['dt']=str(int(edge['dt'])+next32()%100000001-50000000)
    return result
class Runner:
    def __init__(self,result,config,folder):
        folder.mkdir(parents=True,exist_ok=True);self.seq=0;self.pending=queue.Queue();self.stderr=[];self.folder=folder
        cfg=copy.deepcopy(config);cfg['journal_path']=(folder/'private.journal.jsonl').as_posix();cfg['run_id']='private-'+uuid.uuid4().hex;cfg['durability']='durable';write(folder/'private-session.json',cfg)
        env=dict(os.environ);env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+env.get('PATH','')
        self.p=subprocess.Popen([result['runner_path'],str(folder/'private-session.json')],cwd=ROOT,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',bufsize=1)
        def output():
            for line in self.p.stdout:self.pending.put(line)
            self.pending.put(None)
        def errors():
            for line in self.p.stderr:self.stderr.append(line)
        threading.Thread(target=output,daemon=True).start();threading.Thread(target=errors,daemon=True).start()
    def call(self,op,**values):
        request=dict(op=op,**values)
        if op not in ['get','journal','trace.commits','replay.current','replay.file']:
            self.seq+=1;request['command_id']='host-'+str(self.seq)
        self.p.stdin.write(json.dumps(request)+'\n');self.p.stdin.flush();line=self.pending.get(timeout=120)
        if line is None:raise RuntimeError('E_PRIVATE_RUNNER_EXIT')
        response=json.loads(line)
        if response.get('status')=='rejected':raise RuntimeError('E_PRIVATE_RUNNER_COMMAND: '+str(response.get('error',response.get('reason',''))))
        return response
    def close(self):
        if self.p.poll() is None:
            self.p.stdin.close()
            try:self.p.wait(timeout=5)
            except subprocess.TimeoutExpired:self.p.kill();self.p.wait(timeout=5)
def assemble(project,folder,build):
    folder.mkdir(parents=True,exist_ok=True);request=copy.deepcopy(project);request.update(repo_root=ROOT.as_posix(),build_dir=Path(build).resolve().as_posix());write(folder/'build-request.private.json',request)
    env=dict(os.environ);env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+env.get('PATH','');r=subprocess.run([sys.executable,str(ROOT/'tools/virtual/build_virtual_project.py'),'--request',str(folder/'build-request.private.json'),'--result',str(folder/'build-result.private.json')],cwd=ROOT,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=600);(folder/'build.private.log').write_text(r.stdout+r.stderr,encoding='utf-8')
    result=read(folder/'build-result.private.json')
    if r.returncode or not result.get('ok'):raise RuntimeError('E_REPAIR_BUILD_REJECTED')
    return result,read(result['session_config'])
def verify_package(folder):
    manifest=read(folder/'manifest.private.json');signature=bytes.fromhex(manifest.pop('signature'));public=serialization.load_pem_public_key((ROOT/'registry/teaching/course-signing-key.pub').read_bytes());public.verify(signature,json.dumps(manifest,sort_keys=True,separators=(',',':')).encode())
    for name,digest in manifest['files'].items():
        if Path(name).name!=name or hashlib.sha256((folder/name).read_bytes()).hexdigest()!=digest:raise RuntimeError('E_COURSE_INTEGRITY')
    return read(folder/'task.public.json'),read(folder/'faults.private.json'),read(folder/'rubric.private.json')
def safe_state(node):
    result={k:node[k] for k in ['state_code','logic','valid','contention'] if k in node};explanation=node.get('explanation')
    if explanation:
        ids={d['driver_id']:'observed-source-'+str(i+1) for i,d in enumerate(explanation['drivers'])}
        result['explanation']=dict(schema='sigflow.resolution-explanation.public.v1',resolver=explanation['resolver'],state_code=explanation['state_code'],logic=explanation['logic'],valid=explanation['valid'],contention=explanation['contention'],drivers=[dict(driver_id=ids[d['driver_id']],state_code=d['state_code'],strength=d['strength'],level=d['level']) for d in explanation['drivers']],fold_steps=[dict(step_index=s['step_index'],lhs_state=s['lhs_state'],rhs_driver_id=ids[s['rhs_driver_id']],rhs_state=s['rhs_state'],result_state=s['result_state'],contention_triggered=s['contention_triggered']) for s in explanation['fold_steps']],conflict_sources=[dict(lhs_driver_id=ids[s['lhs_driver_id']],rhs_driver_id=ids[s['rhs_driver_id']],lhs_state=s['lhs_state'],rhs_state=s['rhs_state']) for s in explanation['conflict_sources']],summary='电气观测为 '+('CON：比较已测驱动的折叠步骤。' if node.get('contention') else 'FLT：当前没有有效驱动。' if node.get('state_code')==2 else '按已测状态与强度解释。'))
    return result
class TeachingService:
    def __init__(self,course_id,build,work,student_root=None):
        if not re.fullmatch(r'course-0[1-4]-[a-z-]+',course_id):raise ValueError('E_COURSE_ID')
        self.folder=ROOT/'examples/virtual/courses'/course_id;self.task,self.plan,self.rubric=verify_package(self.folder);self.build=Path(build);self.work=Path(work);self.work.mkdir(parents=True,exist_ok=True);self.student_root=Path(student_root or self.folder).resolve();self.token=secrets.token_urlsafe(32);self.mac=secrets.token_bytes(32);self.hints=[];self.measurements=[];self.level=0;self.lock=threading.RLock();self.history=[];self.replays=[];self.wave_events={}
        if student_root is None:
            self.student_root=(self.work/'student').resolve();self.student_root.mkdir(parents=True,exist_ok=True)
            (self.student_root/'solution.sv').write_bytes((self.folder/'broken.sv').read_bytes())
        project=read(self.folder/'virtual.project.json');project['project_root']=self.folder.as_posix();project['fault_profile']=self.plan;self.result,cfg=assemble(project,self.work/'initial-build',self.build);self.runner=Runner(self.result,cfg,self.work/'private-run');self.frame=self.runner.call('get');self.http=None;self.http_token=secrets.token_urlsafe(32)
    def digest(self,value):return hmac.new(self.mac,str(value).encode(),hashlib.sha256).hexdigest()
    def project_measurement(self,m):
        result={k:m[k] for k in ['probe_id','target','position','nominal_mv','voltage_model','read_only'] if k in m};result['state']=safe_state(m['state']);return result
    def public_frame(self,frame=None):
        f=frame or self.frame;result={k:copy.deepcopy(f[k]) for k in ['state','reason','tick_ps','position','attempted_position','topology_position','outputs','input_writes','eval_calls','final_calls','pending_count','pending_inputs','binding_status'] if k in f};result.update(phase1=True,phase2=True,mode='student_blind_test',run_id=self.token,run_token=self.token,task=self.task,hints_used=len(self.hints),allowed_controls=['play','pause','step','stop','set','probe','capture','hint','grade'])
        result['student_workspace_source']=(self.student_root/'solution.sv').as_posix()
        result['board']=dict(board_id=f.get('board',{}).get('board_id',''),nodes={k:safe_state(v) for k,v in f.get('board',{}).get('nodes',{}).items()},leds=f.get('board',{}).get('leds',{}),peripherals=f.get('board',{}).get('peripherals',{}))
        if 'trace' in f:result['trace']=dict(f['trace'],run_id=self.token)
        if 'frequency_hz' in f:result['frequency_hz']=f['frequency_hz']
        if 'instruments' in f:result['instruments']=dict(live=[self.project_measurement(m) for m in f['instruments'].get('live',[])])
        if 'digests' in f:result['digests']={k:self.digest(v) for k,v in f['digests'].items() if k in ['circuit','control']};result['digests']['algorithm']='sigflow.public-hmac.v1'
        if f.get('guard'):result['guard']={k:v for k,v in f['guard'].items() if k in ['port_ref','package_pin','state','label','attempted_position','last_committed_position']}
        result['error']='E_UNKNOWN_INPUT' if f.get('reason')=='UnknownInput' else 'E_NO_CLOCK_EDGE' if f.get('reason')=='NoClockEdge' else ''
        return result
    def forward(self,request):
        op=request['op'];values={k:v for k,v in request.items() if k not in ['op','command_id','run_token','role']}
        allowed={'get':set(),'step':{'mode','ticks'},'set':{'target','state'},'pulse':{'target','duration_ticks','pressed_state','released_state'},'uart.send':{'target','bytes'},'dip.set':{'target','value'},'play':set(),'pause':set(),'stop':set(),'trace.commits':{'after_revision','through_revision','limit'},'probe.attach':{'probe_id','target'},'probe.detach':{'probe_id'},'probe.read':{'probe_id'},'capture.arm':{'config'},'capture.read':set()}
        if op not in allowed or set(values)-allowed[op]:raise ValueError('E_BLIND_OPERATION_DENIED')
        if op=='probe.attach' and values['target'] not in self.task['allowed_observations']:raise ValueError('E_OBSERVATION_NOT_ALLOWED')
        if op=='capture.arm' and any(t not in self.task['allowed_observations'] for t in values['config']['targets']):raise ValueError('E_OBSERVATION_NOT_ALLOWED')
        response=self.runner.call(op,**values)
        if op=='trace.commits':
            names={s['id']:s['name'] for s in response['signals']}
            for batch in response['batches']:
                for event in batch['events']:
                    name=names[event['signal_id']]
                    if name in self.task['allowed_observations']:
                        position=event.get('position',event);key=tuple(str(position[k]) for k in ['tick','delta','event_seq']);self.wave_events.setdefault(name,{})[key]=event
            response['run_id']=self.token;return response
        self.frame=response;projected=self.public_frame(response)
        if 'instrument_reply' in response:
            m=response['instrument_reply']
            if 'state' in m and 'probe_id' in m:
                m=self.project_measurement(m)
                if op=='probe.read':
                    m['evidence_id']='measurement-'+str(len(self.measurements)+1);self.measurements.append(m)
            elif m.get('source')=='virtual_probe':m={k:v for k,v in m.items() if k!='source_fingerprint'};m['source_fingerprint']=response['instrument_reply']['source_fingerprint']
            projected['instrument_reply']=m
        self.history.append(dict(operation=op,position=projected['position'],reason=projected.get('reason')));return projected
    def hint(self,request):
        level=str(request.get('level','L1'))
        if level not in ['L1','L2','L3','L4'] or int(level[1:])!=self.level+1:raise ValueError('E_HINT_LEVEL_LOCKED')
        if self.level>=1 and len(self.measurements)<self.level:raise ValueError('E_HINT_EVIDENCE_REQUIRED')
        text=self.rubric['hints'][self.level]
        if self.level==2:text+=' 已记录测量点：'+', '.join(sorted({m['target'] for m in self.measurements}))+'。'
        self.level+=1;item=dict(level=level,sequence=len(self.hints)+1,text=text,position=self.frame.get('position'));self.hints.append(item);return dict(schema='sigflow.teaching-hint.v1',run_token=self.token,**item,hints_used=len(self.hints))
    def grade(self,request):
        cause=request.get('root_cause',{});evidence=set(request.get('evidence_ids',[]));known={m['evidence_id'] for m in self.measurements};targets={m['target'] for m in self.measurements if m['evidence_id'] in evidence}
        located=cause==self.rubric['cause'] and set(self.rubric['required_observations'])<=targets and evidence<=known
        measured=[m for m in self.measurements if m['evidence_id'] in evidence]
        if cause.get('type')=='open':located=located and any(m['state']['state_code']==2 for m in measured) and any(m['state']['valid'] for m in measured)
        if cause.get('type')=='bridge':located=located and any(m['state']['contention'] for m in measured)
        if cause.get('type')=='button_bounce':located=located and len(self.wave_events.get('pad.3',{}))>=6
        if cause.get('type')=='clock_stop':located=located and any(h.get('reason')=='NoClockEdge' for h in self.history)
        base=dict(schema='sigflow.teaching-grade.v1',status='fail',score=0,first_failure_tick=self.frame.get('position',{}).get('tick','0'),signal='submitted_evidence',expected='correct location supported by actual measurements',actual='insufficient',seeds_tested=0,hints_used=len(self.hints),run_fingerprint=self.digest('unverified'))
        if not located:return base
        if self.rubric['repair_kind']=='physical_service' and request.get('repair',{}).get('restore')!=self.rubric['allowed_repair']:base.update(score=30,signal='repair',expected='physical connection restored',actual='not restored');return base
        project=read(self.folder/'virtual.project.json');project['project_root']=self.folder.as_posix();project['fault_profile']=copy.deepcopy(self.plan)
        if self.rubric['repair_kind']=='rtl_debounce':
            source=Path(request.get('repair',{}).get('source','')).resolve()
            if not source.is_relative_to(self.student_root) or not source.is_file() or source.suffix.lower() not in ['.v','.sv']:return dict(base,score=30,signal='repair',actual='source unavailable')
            text=source.read_text(encoding='utf-8-sig')
            if re.search(r'\$(?:fopen|readmem|system)|`include',text):return dict(base,status='inconclusive',score=30,signal='repair',actual='outside course source subset')
            project['source_files']=[source.as_posix()]
        else:project['fault_profile']['faults']=[]
        results=[]
        try:
            result,cfg=assemble(project,self.work/'repair-build',self.build)
            for index,seed in enumerate(self.rubric['hidden_seeds']):
                config=copy.deepcopy(cfg);config['fault_profile']=bounce_for_seed(config['fault_profile'],seed)
                runner=Runner(result,config,self.work/('grade-'+uuid.uuid4().hex));ok=False
                try:
                    # A reset requiring a nonzero register value is sampled before release.
                    runner.call('step');runner.call('set',target='reset_p4',state=2);runner.call('set',target='key_p3',state=13)
                    if self.rubric['assertion']=='uart_bad_frame_rejection_and_recovery':
                        # Normal frame, a physical bad stop bit, then a valid
                        # frame with a bounded peer clock offset. DUT owns the
                        # 8N1 accept counter; observations alone cannot pass it.
                        runner.call('step');runner.call('uart.send',target='uart0',bytes=[65]);frame=runner.call('step',mode='ticks',ticks='120000000')
                        def good(f):return sum((not f['outputs']['led_n['+str(b)+']'])<<b for b in range(3))
                        normal=good(frame)==1;bad=copy.deepcopy(config['fault_profile']);bad['faults']=[dict(id='hidden-protocol-'+seed,enabled=True,visibility='teacher_only',hook='protocol.transform',target=dict(kind='component',id='uart0'),effect=dict(type='uart_framing_error',bit='stop'),trigger=dict(kind='permanent',start_tick='0'))];runner.call('profile.load',profile=bad);runner.call('uart.send',target='uart0',bytes=[85]);frame=runner.call('step',mode='ticks',ticks='120000000');rejected=good(frame)==1
                        bad['revision']=2;bad['faults']=[dict(id='hidden-offset-'+seed,enabled=True,visibility='teacher_only',hook='protocol.transform',target=dict(kind='component',id='uart0'),effect=dict(type='uart_baud_offset_ppm',offset_ppm=[10000,20000,-20000][index]),trigger=dict(kind='permanent',start_tick='0'))];runner.call('profile.load',profile=bad);runner.call('uart.send',target='uart0',bytes=[67]);frame=runner.call('step',mode='ticks',ticks='120000000');ok=normal and rejected and good(frame)==2
                    elif self.rubric['repair_kind']=='rtl_debounce':
                        frame=runner.call('step',mode='ticks',ticks='20000000000');count=sum((not frame['outputs']['led_n['+str(bit+3)+']'])<<bit for bit in range(3));ok=count==1
                    else:
                        frame=runner.call('step');ok=frame.get('reason')=='StepComplete' and frame['board']['leds']['led0']['lit'] and not frame['board']['nodes']['pad.10']['contention']
                    results.append(dict(passed=bool(ok),tick=frame['position']['tick'],circuit=frame['digests']['circuit'],profile=config['fault_profile'],seed=seed))
                    runner.call('stop')
                finally:runner.close()
            write(self.work/('grading-'+uuid.uuid4().hex+'.private.json'),dict(runner_sha256=result['runner_sha256'],results=results));tested=len(results);passed=all(r['passed'] for r in results);fingerprint=dict(runner=result['runner_sha256'],outcomes=[dict(passed=r['passed'],tick=r['tick']) for r in results]);base.update(status='pass' if passed else 'fail',score=100 if passed else 30,first_failure_tick=None if passed else next(r['tick'] for r in results if not r['passed']),signal='functional_output',expected=1,actual=1 if passed else 0,seeds_tested=tested,run_fingerprint=self.digest(json.dumps(fingerprint,sort_keys=True)));return base
        except Exception:
            return dict(base,status='inconclusive',score=30,signal='repair_build',expected='successful compilation and bounded execution',actual='tool or runtime failure',seeds_tested=len(results))
    def handle(self,request):
        with self.lock:
            if 'run_token' in request and request['run_token']!=self.token:raise ValueError('E_RUN_OWNERSHIP')
            op=request.get('op',request.get('capability','get'))
            if op in ['virtual.session.read','get']:
                self.frame=self.runner.call('get');return self.public_frame()
            if op=='virtual.evidence.query':return dict(schema='sigflow.virtual-evidence.public.v1',run_token=self.token,measurements=self.measurements,hints=self.hints,wave_statistics={name:dict(transitions=len(events)) for name,events in self.wave_events.items()})
            if op=='teaching.hint.request':return self.hint(request)
            if op=='teaching.grade.submit':return self.grade(request)
            if op=='journal':return dict(schema='sigflow.public-observation-journal.v1',run_token=self.token,records=self.history,measurements=self.measurements)
            if op=='replay.current':
                raw=self.runner.call('replay.current',repetitions=min(3,max(1,int(request.get('repetitions',3)))));return {**{k:raw[k] for k in ['status','matched_commits','repetitions']},'circuit_digest':self.digest(raw['circuit_digest']),'control_digest':self.digest(raw['control_digest']),'attempts':[dict(status='passed',matched_commits=r['matched_commits'],circuit_digest=self.digest(r['circuit_digest']),control_digest=self.digest(r['control_digest'])) for r in raw['attempts']]}
            return self.forward(dict(request,op=op))
    def close(self):
        self.runner.close()
        if self.http:self.http.shutdown();self.http.server_close()
    def serve_http(self):
        service=self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_POST(self):
                self.connection.settimeout(10)
                try:length=int(self.headers.get('Content-Length','0'))
                except ValueError:self.send_error(400);return
                if length<1 or length>1024*1024:self.send_error(413);return
                # Consume a bounded request before replying, including 401/403.
                # Closing with unread bytes can reset WinSock instead of delivering
                # the intended HTTP denial to a legitimate local SDK client.
                payload=self.rfile.read(length)
                if len(payload)!=length:self.send_error(400);return
                if self.headers.get('Authorization')!='Bearer '+service.http_token:self.send_error(401);return
                if self.headers.get('Host')!='127.0.0.1:'+str(service.http.server_port) or self.headers.get('Origin') is not None:self.send_error(403);return
                if self.path!='/api/v1/virtual-teaching/call':self.send_error(404);return
                try:
                    request=json.loads(payload);cap=request.get('capability',request.get('op'))
                    if request.get('run_token')!=service.token:raise ValueError('E_RUN_OWNERSHIP')
                    if cap not in CAPABILITIES:raise ValueError('E_CAPABILITY_NOT_GRANTED')
                    data=service.handle(dict(request,op=cap));body=json.dumps(dict(protocol_version='sigflow.virtual-teaching.v1',ok=True,data=data)).encode();status=200
                except Exception as e:body=json.dumps(dict(protocol_version='sigflow.virtual-teaching.v1',ok=False,error=str(e) if isinstance(e,ValueError) else 'E_TEACHING_REQUEST')).encode();status=400
                self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
        self.http=ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=self.http.serve_forever,daemon=True).start();return dict(url='http://127.0.0.1:'+str(self.http.server_port)+'/api/v1/virtual-teaching/call',bearer=self.http_token,capabilities=CAPABILITIES)
def main():
    sys.stdin.reconfigure(encoding='utf-8',errors='strict');sys.stdout.reconfigure(encoding='utf-8',errors='strict');sys.stderr.reconfigure(encoding='utf-8',errors='replace')
    p=argparse.ArgumentParser();p.add_argument('--config',type=Path,required=True);a=p.parse_args();config=read(a.config);service=None
    try:
        service=TeachingService(config['course_id'],config.get('build_dir',ROOT/'build-m0'),config.get('work',ROOT/'.work/p2/trusted'/uuid.uuid4().hex),config.get('student_root'));gateway=service.serve_http()
        for line in sys.stdin:
            try:
                request=json.loads(line);response=service.handle(request);response['teaching_gateway']=gateway
            except Exception as e:response=dict(status='rejected',error=str(e) if isinstance(e,ValueError) else 'E_TRUSTED_TEACHING_FAILURE')
            print(json.dumps(response,ensure_ascii=False),flush=True)
    finally:
        if service:service.close()
if __name__=='__main__':main()
