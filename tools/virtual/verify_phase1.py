"""P1 acceptance uses production project assembly and actual generated RTL runners."""
import argparse,copy,hashlib,json,os,queue,subprocess,sys,threading,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
def read(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def write(p,data):p=Path(p);p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');return p
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def rule(id,type,target,kind,hook,effect,trigger=None):return dict(id=id,enabled=True,visibility='student_visible',hook=hook,target=dict(kind=kind,id=target),effect=dict(effect,type=type),trigger=trigger or dict(kind='permanent',start_tick='0'))
def stop_rule(end=None,hold=0):return rule('stop-clock','clock_stop','clk27m','clock','clock.schedule',dict(hold_level=hold,resume_phase='restart_half_period'),dict(kind='transient',start_tick='0',end_tick=str(end)) if end else None)
def profile(board,rules):return dict(schema='sigflow.fault-profile.v1',id='p1-acceptance',revision=1,board_ref=board['id']+'@'+board['version'],tick_ps='1',rng=dict(algorithm='pcg32-v1',seed='20261109',streams='by_fault_id'),conflict_policy='reject_overlapping_mutations',faults=rules)
def outputs(frame):return {name:int(not value) for name,value in frame['outputs'].items()}
def count(frame,offset=0,width=6):return sum(outputs(frame)['led_n['+str(i+offset)+']']<<i for i in range(width))
class Client:
    def __init__(self,owner,result,name,config=None):
        self.owner=owner;self.name=name;self.counter=0;self.transcript=[]
        cfg=copy.deepcopy(config or read(result['session_config']));cfg['run_id']='p1-'+name;cfg['journal_path']=(owner.work/(name+'-'+uuid.uuid4().hex+'.journal.jsonl')).as_posix();cfg['durability']='durable'
        self.cfg=cfg;self.config_path=write(owner.work/(name+'.json'),cfg);self.lines=queue.Queue();self.errors=[]
        self.process=subprocess.Popen([result['runner_path'],str(self.config_path)],cwd=ROOT,env=owner.env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',bufsize=1)
        def consume():
            for line in self.process.stdout:self.lines.put(line)
            self.lines.put(None)
        def errors():
            for line in self.process.stderr:self.errors.append(line)
        threading.Thread(target=consume,daemon=True).start();threading.Thread(target=errors,daemon=True).start();owner.clients.append(self)
    def call(self,op,**args):
        request=dict(op=op,**args)
        if op not in ['get','journal','trace.commits','replay.current','replay.file']:
            self.counter+=1;request['command_id']=self.name+'-'+str(self.counter)
        self.process.stdin.write(json.dumps(request)+'\n');self.process.stdin.flush()
        line=self.lines.get(timeout=180)
        if line is None:raise RuntimeError('runner exited: '+''.join(self.errors))
        response=json.loads(line);self.transcript.append(dict(request=request,response=response));return response
    def step(self,mode='rising',ticks=None):return self.call('step',mode=mode,**(dict(ticks=str(ticks)) if ticks is not None else {}))
    def close(self):
        if self.process.poll() is None:self.process.stdin.close();self.process.wait(timeout=20)
        if self.process.returncode:raise RuntimeError('runner failed: '+''.join(self.errors))
        write(self.owner.work/(self.name+'-transcript.json'),self.transcript)
        self.owner.clients.remove(self)
class Acceptance:
    def __init__(self,build,mode):
        self.build=Path(build).resolve();self.mode=mode;self.work=ROOT/'.work/p1'/mode;self.work.mkdir(parents=True,exist_ok=True);self.env=dict(os.environ);self.env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+self.env.get('PATH','');self.clients=[];self.report=dict(status='running',checks=[],commands=[],replays=[])
    def check(self,ok,message):
        with (self.work/'progress.log').open('a',encoding='utf-8') as log:log.write(str(bool(ok))+' '+message+'\n')
        if not ok:raise AssertionError(message)
        self.report['checks'].append(message)
    def run(self,args,name,timeout=360):
        start=time.monotonic();r=subprocess.run([str(x) for x in args],cwd=ROOT,env=self.env,capture_output=True,encoding='utf-8',errors='replace',timeout=timeout)
        (self.work/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8');self.report['commands'].append(dict(argv=[str(x) for x in args],exit_code=r.returncode,seconds=round(time.monotonic()-start,3)))
        if r.returncode:raise RuntimeError(name+': '+(r.stdout+r.stderr)[-3000:])
        return r
    def build_project(self,name,request):
        request=dict(request,repo_root=ROOT.as_posix(),build_dir=self.build.as_posix());req=write(self.work/(name+'-request.json'),request);out=self.work/(name+'-build.json')
        self.run([sys.executable,ROOT/'tools/virtual/build_virtual_project.py','--request',req,'--result',out],name+'-build');result=read(out);self.check(result['ok'],'production builder compiled '+name);return result
    def replay(self,c,name):
        stopped=c.call('stop');r=c.call('replay.current',repetitions=3)
        self.check(r.get('status')=='passed' and len(r['attempts'])==3 and r['matched_commits']>0 and all(a['circuit_digest']==stopped['digests']['circuit'] and a['control_digest']==stopped['digests']['control'] and a['fault_rng_peripherals_verified'] for a in r['attempts']),name+' three from-zero real RTL replays verify RNG, integrators, UART and both digests')
        self.report['replays'].append(dict(name=name,**r));c.close()
    def faults(self):
        self.run([self.build/'tests/virtual/p1/p1_native_tests.exe',ROOT,self.work,'faults'],'native')
        board=read(ROOT/'registry/boards/tang-nano-9k.virtual.json');board['version']='1.0.0-draft';board_file=write(self.work/'counter-board.json',board)
        request=read(ROOT/'examples/virtual/counter/virtual.project.json');request.update(project_root=(ROOT/'examples/virtual/counter').as_posix(),board_path=board_file.as_posix(),phase1=True,integration_window_ticks='1000000',fault_profile=profile(board,[]))
        result=self.build_project('counter',request);base=read(result['session_config'])
        for mode in ['replace_source','short_to_rail']:
            cfg=copy.deepcopy(base);cfg['fault_profile']=profile(board,[rule('stuck','stuck_at','fpga0.pad.10','fpga_pad_driver','driver.transform',dict(value=0,mode=mode))]);c=Client(self,result,mode,cfg);f=c.call('get');node=f['board']['nodes']['pad.10'];self.check(f['outputs']['led_n[0]'] is True,'stuck leaves original real DUT output untouched')
            self.check((node['state_code']==13 and not node['contention']) if mode=='replace_source' else (node['contention'] and len(node['drivers'])==2),mode+' has physically distinct driver-set semantics')
            c.call('set',target='reset_p4',state=2);c.call('set',target='key_p3',state=13);f=c.step();self.check(f['outputs']['led_n[0]'] is False and not f['board']['nodes']['pad.10']['contention'],'same strong levels do not fabricate contention');f=c.step();self.check(f['outputs']['led_n[0]'] is True and (f['board']['nodes']['pad.10']['contention'] is (mode=='short_to_rail')),'changing DUT output is independent of stuck source transform')
            c.call('fault.revoke',fault_id='stuck');f=c.step();self.check(f['board']['nodes']['pad.10']['logic']==('1' if f['outputs']['led_n[0]'] else '0') and all('fault:' not in d['driver_id'] for d in f['board']['nodes']['pad.10']['drivers']),'revoke reconstructs original driver set')
            self.replay(c,mode)
        cfg=copy.deepcopy(base);cfg['fault_profile']=profile(board,[rule('open','open','wire.led1','connection','topology.transform',{},dict(kind='transient',start_tick='1000',end_tick='2000'))]);c=Client(self,result,'open-window',cfg);f=c.step('next_event');self.check(f['position']['tick']=='1000' and f['board']['nodes']['led1.K']['state_code']==2 and f['board']['nodes']['pad.11']['valid'],'Open start detaches actual LED endpoint without altering driven Pad');f=c.step('next_event');self.check(f['position']['tick']=='2000' and f['board']['wires']['wire.led1'],'transient end is exclusive and re-resolves the graph');self.replay(c,'open')
        cfg=copy.deepcopy(base);cfg['fault_profile']=profile(board,[stop_rule(1000000)]);c=Client(self,result,'clock-stop',cfg);f=c.step();self.check(f.get('error')=='E_NO_CLOCK_EDGE' and f['position']['tick']=='0','default rising Step reports bounded E_NO_CLOCK_EDGE without skipping stop interval');f=c.step('ticks',200000);self.check(f['position']['tick']=='200000','Step N Ticks advances time while stopped');f=c.step('next_event');self.check(f['position']['tick']=='1000000' and not f['p1']['clock_stopped'],'Next Event reaches stop-window end');f=c.step();self.check(f['position']['tick']=='1018519','clock resumes a full rational half-period from stop end');self.replay(c,'clock-stop')
        c=Client(self,result,'conflict',base);before=c.call('get');a=rule('a','stuck_at','fpga0.pad.10','fpga_pad_driver','driver.transform',dict(value=0,mode='replace_source'));b=copy.deepcopy(a);b['id']='b';b['effect']['value']=1;r=c.call('profile.load',profile=profile(board,[a,b]));self.check(r['status']=='rejected' and 'E_FAULT_CONFLICT' in r['error'] and r['input_writes']==before['input_writes'],'conflicting profile is rejected structurally before DUT import');c.call('stop');c.close()
        # Exact 10ms contract on an explicitly declared 1kHz teaching clock fixture.
        # The 27MHz reference above is never cycle-folded or silently slowed.
        slow=copy.deepcopy(board);slow['version']='1.0.0-p1-fixture-1khz';slow['clocks'][0]['id']='lab_clock_1k';slow['clocks'][0]['frequency_hz']['num']='1000';slow['phase1_test_fixture']=dict(physical_27mhz_claim=False,purpose='10ms debounce reference at a declared 1kHz teaching sample clock')
        for edge in slow['connections']:
            if edge['id']=='wire.clock':edge['endpoint']='lab_clock_1k.out'
        slow_file=write(self.work/'debounce-board-1khz.json',slow);deb=copy.deepcopy(request);deb.update(source_files=[(ROOT/'tests/virtual/p1/debounce.sv').as_posix()],top_module='p1_debounce',board_path=slow_file.as_posix());deb['clock']['frequency_hz']='1000';deb['clock']['max_step_events']=1000;deb['fault_profile']=profile(slow,[]);deb_result=self.build_project('debounce',deb);deb_cfg=read(deb_result['session_config']);self.report['debounce_clock_fixture']=slow['phase1_test_fixture']
        physical=[]
        for name,bouncing,release,unrelated in [('clean',False,False,False),('bounce',True,False,False),('bounce-unrelated',True,False,True),('release-cancel',True,True,False)]:
            cfg=copy.deepcopy(deb_cfg);p=read(ROOT/'examples/virtual/phase1/contracts/fault-profile.json');p['board_ref']=slow['id']+'@'+slow['version']
            for fault in p['faults']:fault['enabled']=bouncing and fault['id']=='bounce-key3'
            if unrelated:
                extra=rule('unrelated-disabled','stuck_at','fpga0.pad.16','fpga_pad_driver','driver.transform',dict(value=1,mode='replace_source'));extra['enabled']=False;p['faults'].append(extra)
            cfg['fault_profile']=p;c=Client(self,deb_result,name,cfg)
            # A reset value different from Verilator's two-state power-on zero
            # must be sampled under asserted reset before the first press.
            reset_frame=c.step();self.check(reset_frame['board']['nodes']['pad.4']['logic']=='0','reference reset is sampled on an actual clock edge before release')
            c.call('set',target='reset_p4',state=2);c.call('set',target='key_p3',state=13)
            if release:c.step('ticks',300000000);c.call('set',target='key_p3',state=2)
            f=c.step('ticks',20000000000);self.check(f['reason']=='StepComplete','10ms contact expansion advances through exact peripheral deadlines');raw=count(f,0,3);filtered=count(f,3,3)
            self.check((raw,filtered)==((1,0) if release else (3,1) if bouncing else (1,1)),name+' real RTL raw/debounced counters have expected edge counts; actual='+str((raw,filtered)))
            if bouncing:
                status=next(r for r in f['faults']['rules'] if r['id']=='bounce-key3');self.check(status['activations']=='1' and status['matches']=='1' and status['remaining_edges']==0,'intermittent expansion has no recursive input.intent trigger')
            if name in ['bounce','bounce-unrelated']:
                stop=c.call('stop');records=c.call('journal')['records'];events=[e for r in records if r['kind']=='commit' for e in r['trace_events']];signals=records[0]['trace_signals'];signal=next(s['id'] for s in signals if s['name']=='pad.3');trajectory=[e for e in events if e['signal_id']==signal];physical.append(trajectory)
                if name=='bounce':self.report['bounce_contact_trajectory']=trajectory
                r=c.call('replay.current',repetitions=3);self.check(r['status']=='passed' and all(a['circuit_digest']==stop['digests']['circuit'] and a['control_digest']==stop['digests']['control'] for a in r['attempts']),name+' bounce RNG and edges replay three times');self.report['replays'].append(dict(name=name,**r));c.close()
            else:self.replay(c,name)
        self.check(physical[0]==physical[1],'adding unrelated fault leaves every original physical contact event unchanged')
    def peripherals(self):
        self.run([self.build/'tests/virtual/p1/p1_native_tests.exe',ROOT,self.work,'peripherals'],'native')
        project=read(ROOT/'examples/virtual/phase1/virtual.project.json');project['project_root']=(ROOT/'examples/virtual/phase1').as_posix();project.pop('fault_profile_path',None);board=read(ROOT/'registry/boards/tang-nano-9k.phase1.virtual.json');project['fault_profile']=profile(board,[]);result=self.build_project('lab',project);base=read(result['session_config'])
        c=Client(self,result,'pwm-segments',base);c.call('set',target='reset_p4',state=2);f=c.step('ticks',8000000);self.check(f['reason']=='StepComplete','real 27MHz PWM/scan RTL advances every required clock edge');duties=[f['board']['leds']['led'+str(i)]['integration']['duty_ppm'] for i in range(3)];self.check(duties==[250000,500000,750000],'integer sliding 4us window measures exact 25/50/75% from real RTL')
        for i in range(15):self.check([c.call('get')['board']['leds']['led'+str(j)]['integration']['duty_ppm'] for j in range(3)]==duties,'read-only GUI polling cannot change simulated-time integration')
        component=f['board']['peripherals']['seg0'];expected=[0x06,0x5b,0x4f,0x66];actual=[sum((s['duty_ppm']>0)<<i for i,s in enumerate(digit)) for digit in component['digits']];self.check(actual==expected and int(component['scan_period_ticks'])>0,'integrated four-digit scan reconstructs 1 2 3 4 with measured scan period');self.check(sum(any(s['instant_active'] for s in digit) for digit in component['digits'])==1,'single-step electrical observation only conducts the selected digit')
        c.call('dip.set',target='dip0',value=3);f=c.step('next_event');self.check(f['board']['nodes']['pad.40']['logic']=='1' and f['board']['nodes']['pad.41']['logic']=='1' and f['outputs']['led_n[4]'] is False and f['outputs']['led_n[5]'] is False,'DIP bits traverse reviewed Pad/CST into actual DUT ports');self.report['pwm_duty_ppm']=duties;self.report['segment_masks']=actual;self.replay(c,'pwm-segments')
        cathode=copy.deepcopy(board)
        for component in cathode['components']:
            if component['model']=='sigflow.seven-segment-mux@1':component['params'].update(common='cathode',segment_active_level=1,digit_active_level=0)
        cathode_file=write(self.work/'cathode-board.json',cathode);source=(ROOT/'examples/virtual/phase1/phase1.sv').read_text(encoding='utf-8').replace("4'b0001 << count[4:3]","~(4'b0001 << count[4:3])").replace("~8'b","8'b");source_file=self.work/'cathode.sv';source_file.write_text(source,encoding='utf-8');p=copy.deepcopy(project);p['source_files']=[source_file.as_posix()];p['board_path']=cathode_file.as_posix();cat_result=self.build_project('cathode',p);c=Client(self,cat_result,'cathode');c.call('set',target='reset_p4',state=2);f=c.step('ticks',8000000);actual=[sum((s['duty_ppm']>0)<<i for i,s in enumerate(digit)) for digit in f['board']['peripherals']['seg0']['digits']];self.check(actual==expected,'common-cathode polarity uses explicit active levels without RTL-name shortcuts');self.replay(c,'common-cathode')
        cfg=copy.deepcopy(base);cfg['fault_profile']=profile(board,[stop_rule()]);c=Client(self,result,'uart-8n1',cfg);c.call('set',target='reset_p4',state=2);c.call('uart.send',target='uart0',bytes=[0x41,0x55,0,255]);f=c.step('ticks',50000000);uart=f['p1']['peripherals']['uart']['uart0'];self.check([r['byte'] for r in uart['received']]==[0x41,0x55,0,255] and all(r['valid'] for r in uart['received']),'8N1 round trip crosses UART.TX -> pad18 -> real RTL -> pad17 -> UART.RX');self.check([r['byte'] for r in uart['transmitted']]==[0x41,0x55,0,255] and f['p1']['clock_stopped'],'UART deadlines and combinational DUT input still progress during ClockStop');self.report['uart_8n1_frames']=uart;self.replay(c,'uart-8n1')
        for mode in ['replace_source','short_to_rail']:
            cfg=copy.deepcopy(base);fault=rule('bad-stop','stuck_at','fpga0.pad.17','fpga_pad_driver','driver.transform',dict(value=0,mode=mode),dict(kind='transient',start_tick='9765625',end_tick='10850694'));cfg['fault_profile']=profile(board,[stop_rule(),fault]);c=Client(self,result,'uart-bad-stop-'+mode,cfg);c.call('uart.send',target='uart0',bytes=[85]);f=c.step('ticks',13000000);frames=f['p1']['peripherals']['uart']['uart0']['received'];self.check(len(frames)==1 and frames[0]['byte']==85 and frames[0]['framing_error'] and not frames[0]['valid'],'UART detects real '+mode+' stop-bit error at sampled physical tick');
            if mode=='short_to_rail':self.check(frames[0]['state_code']&32,'invalid UART stop sample retains raw CON code')
            c.call('uart.send',target='uart0',bytes=[65]);f=c.step('ticks',13000000);self.check(f['p1']['peripherals']['uart']['uart0']['received'][-1]['valid'],'UART receiver recovers on next valid frame after fault ends');self.replay(c,'uart-bad-stop-'+mode)
        parity=copy.deepcopy(board)
        for component in parity['components']:
            if component['model']=='sigflow.uart-peer@1':component['params'].update(data_bits=7,parity='even',stop_bits=2)
        parity_file=write(self.work/'uart-7e2-board.json',parity);p=copy.deepcopy(project);p['board_path']=parity_file.as_posix();p['fault_profile']=profile(parity,[stop_rule()]);parity_result=self.build_project('uart-7e2',p);c=Client(self,parity_result,'uart-7e2');c.call('uart.send',target='uart0',bytes=[0xa5]);f=c.step('ticks',15000000);frames=f['p1']['peripherals']['uart']['uart0']['received'];self.check(len(frames)==1 and frames[0]['byte']==0x25 and frames[0]['valid'],'configurable 7E2 parity and two-stop-bit FSM is sampled at integer midpoints');self.replay(c,'uart-7e2')
        write(ROOT/'.work/p1/gui-reference.json',result)
    def finish(self):
        self.report['status']='passed';self.report['check_count']=len(self.report['checks'])
def main():
    p=argparse.ArgumentParser();p.add_argument('--build-dir',type=Path,default=ROOT/'build-m0');p.add_argument('--mode',choices=['faults','peripherals'],required=True);a=p.parse_args();acceptance=Acceptance(a.build_dir,a.mode);(acceptance.work/'progress.log').write_text('',encoding='utf-8')
    try:getattr(acceptance,a.mode)();acceptance.finish()
    except Exception as e:acceptance.report.update(status='failed',error=str(e));print(str(e),file=sys.stderr);raise
    finally:
        for c in acceptance.clients:
            if c.process.poll() is None:c.process.kill();c.process.wait(timeout=15)
        write(ROOT/'.work/p1'/(a.mode+'-results.json'),acceptance.report)
    print('PASS P1 '+a.mode+': '+str(acceptance.report['check_count'])+' real-tool assertions')
if __name__=='__main__':main()
