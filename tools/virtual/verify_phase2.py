"""Real P2 acceptance. No mocked DUT or successful self-reported repairs."""
import argparse,copy,json,os,re,subprocess,sys,time,uuid,shutil,urllib.request,urllib.error
from pathlib import Path
from virtual_teaching_service import Runner,assemble,TeachingService,verify_package,read,write,ROOT
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
def rule(id,type,target,kind,hook,effect=None):return dict(id=id,enabled=True,visibility='teacher_only',target=dict(id=target,kind=kind),hook=hook,effect=dict(effect or {},type=type),trigger=dict(kind='permanent',start_tick='0'))
class Acceptance:
    def __init__(self,build,mode):self.build=Path(build).resolve();self.work=ROOT/'.work/p2'/mode;self.work.mkdir(parents=True,exist_ok=True);self.report=dict(status='running',checks=[],replays=[],commands=[]);self.clients=[];self.mode=mode
    def check(self,ok,text):
        with (self.work/'progress.log').open('a',encoding='utf-8') as f:f.write(str(bool(ok))+' '+text+'\n')
        if not ok:raise AssertionError(text)
        self.report['checks'].append(text)
    def run(self,args,label,timeout=600):
        env=dict(os.environ);env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+str(self.build)+';'+env.get('PATH','');started=time.monotonic();r=subprocess.run([str(x) for x in args],cwd=ROOT,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=timeout);(self.work/(label+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8');self.report['commands'].append(dict(argv=[str(x) for x in args],exit_code=r.returncode,seconds=round(time.monotonic()-started,3)));self.check(r.returncode==0,label+' actual process exits successfully: '+(r.stdout+r.stderr)[-1000:] if r.returncode else label+' actual process exits successfully');return r
    def minimal(self):
        p=read(ROOT/'examples/virtual/courses/course-01-open-wire/virtual.project.json');p['project_root']=(ROOT/'examples/virtual/courses/course-01-open-wire').as_posix();p['fault_profile']=read(ROOT/'examples/virtual/courses/course-01-open-wire/faults.private.json');p['fault_profile']['faults']=[];return p
    def client(self,result,cfg,label,rules):
        cfg=copy.deepcopy(cfg);cfg['fault_profile']['faults']=rules;r=Runner(result,cfg,self.work/(re.sub(r'[^A-Za-z0-9_.-]','_',label)+'-'+uuid.uuid4().hex));self.clients.append(r);return r
    def replay(self,r,label):
        f=r.call('stop');replay=r.call('replay.current',repetitions=3);self.check(replay['status']=='passed' and len(replay['attempts'])==3 and all(a['circuit_digest']==f['digests']['circuit'] and a['control_digest']==f['digests']['control'] for a in replay['attempts']),label+' three from-zero real RTL replays preserve both digests');self.report['replays'].append(dict(label=label,**replay));r.close();self.clients.remove(r)
    def start(self,r):r.call('step');r.call('set',target='reset_p4',state=2);r.call('set',target='key_p3',state=13);return r.call('step')
    def faults(self):
        from jsonschema import Draft202012Validator
        for p in (ROOT/'registry/schemas').glob('*.schema.json'):Draft202012Validator.check_schema(read(p))
        for folder in (ROOT/'examples/virtual/courses').iterdir():
            if folder.is_dir():
                Draft202012Validator(read(ROOT/'registry/schemas/fault-profile.v2.schema.json')).validate(read(folder/'faults.private.json'));Draft202012Validator(read(ROOT/'registry/schemas/course-task.v1.schema.json')).validate(read(folder/'task.public.json'))
        self.check(True,'versioned schemas validate all four signed public tasks and private profiles')
        self.run([self.build/'tests/virtual/p2/p2_native_tests.exe',ROOT,self.work],'native');result,cfg=assemble(self.minimal(),self.work/'counter-build',self.build)
        r=self.client(result,cfg,'bridge',[rule('hidden-bridge','bridge','pad.10','node','topology.transform',dict(other_node='pad.11'))]);f=r.call('get');self.check(not f['board']['nodes']['pad.10']['contention'],'same-level bridge combines real sources without invented CON');f=self.start(r);node=f['board']['nodes']['pad.10'];self.check(node['contention'] and len(node['explanation']['conflict_sources'])>=1 and len(node['drivers'])==2,'opposite real DUT outputs bridge into one explained CON network');self.replay(r,'bridge')
        r=self.client(result,cfg,'state13-short-rail',[rule('hidden-power-short','stuck_at','fpga0.pad.10','fpga_pad_driver','driver.transform',dict(value=1,mode='short_to_rail'))]);f=self.start(r);self.check(f['board']['nodes']['pad.10']['contention'] and f['board']['nodes']['pad.10']['state_code']==63,'actual Power short-to-rail and opposing Strong DUT driver produce explained CON under state13');self.replay(r,'state13-short-to-rail')
        r=self.client(result,cfg,'pull',[rule('hidden-pull','pull_removal','pad.3:board-pullup-p3','driver','driver.transform')]);f=r.call('get');self.check(f['reason']=='UnknownInput' and f['input_writes']=='0' and f['guard']['state']==2,'removing actual external pull makes input FLT before any DUT writes');r.call('set',target='key_p3',state=13);f=r.call('step');self.check(f['reason']=='StepComplete','remaining strong contact repairs pull loss');self.replay(r,'pull-removal')
        for name,bindings in [('swap',[dict(port_ref='led_n[0]',pad=11),dict(port_ref='led_n[1]',pad=10)]),('unbound',[dict(port_ref='led_n[0]',pad=None)])]:
            r=self.client(result,cfg,name,[rule('hidden-map','mapping_overlay','effective','pad_map','mapping.overlay',dict(bindings=bindings))]);f=self.start(r);self.check(f['pad_map']['original']!=f['pad_map']['effective'],'mapping overlay retains immutable CST and effective PadMap')
            self.check(f['board']['leds']['led1']['lit'] if name=='swap' else f['board']['nodes']['pad.10']['state_code']==2,name+' changes actual mapped physical endpoint');self.replay(r,'mapping-'+name)
        r=self.client(result,cfg,'led-reverse',[rule('hidden-polarity','led_polarity_reverse','led0','component','peripheral.observe')]);f=r.call('get');self.check(f['outputs']['led_n[0]'] is True and f['board']['leds']['led0']['lit'],'LED polarity reversal changes observation without rewriting Pad');self.replay(r,'LED polarity')
        p=read(ROOT/'examples/virtual/phase2/virtual.project.json');p['project_root']=(ROOT/'examples/virtual/phase2').as_posix();p['fault_profile']['rng']['seed']='11';full,base=assemble(p,self.work/'full-build',self.build)
        for kind in ['dead_segment','common_mismatch']:
            effect=dict(segment='a') if kind=='dead_segment' else {};r=self.client(full,base,kind,[rule('hidden-segment',kind,'seg0','component','peripheral.observe',effect)]);r.call('set',target='reset_p4',state=2);f=r.call('step',mode='ticks',ticks='4000000');digits=f['board']['peripherals']['seg0']['digits'];self.check(all(not d[0]['instant_active'] and d[0]['duty_ppm']==0 for d in digits) if kind=='dead_segment' else sum(any(s['instant_active'] for s in d) for d in digits)==3,kind+' changes electrical peripheral model on real scanned outputs');self.replay(r,kind)
        stop=rule('clock-held','clock_stop','clk27m','clock','clock.schedule',dict(hold_level=0,resume_phase='restart_half_period'))
        for kind,effect in [('uart_framing_error',dict(bit='stop')),('uart_framing_error',dict(bit='start')),('uart_baud_offset_ppm',dict(offset_ppm=20000)),('uart_baud_offset_ppm',dict(offset_ppm=-50000))]:
            label=kind+str(effect);fault=rule('uart-disturbance',kind,'uart0','component','protocol.transform',effect);r=self.client(full,base,label,[stop,fault]);byte=255 if effect.get('bit')=='start' else 85;r.call('uart.send',target='uart0',bytes=[byte]);f=r.call('step',mode='ticks',ticks='14000000');u=f['p1']['peripherals']['uart']['uart0']
            if effect.get('bit')=='stop':self.check(u['received'] and u['received'][0]['framing_error'],'wire-level UART stop corruption is detected by actual sampler')
            elif effect.get('bit')=='start':self.check(not u['received'],'missing start bit creates no fabricated decoded byte')
            else:
                expected=(10*1000000000000*1000000+(921600*(1000000+effect['offset_ppm']))//2)//(921600*(1000000+effect['offset_ppm']));self.check(int(u['transmitted'][0]['tick'])==expected,'baud offset changes actual TX bit deadlines with integer rounding')
                if effect['offset_ppm']==20000:self.check(u['received'] and u['received'][0]['valid'],'+2% is tolerated rather than forced into failure')
            r.call('fault.revoke',fault_id='uart-disturbance');r.call('uart.send',target='uart0',bytes=[65]);f=r.call('step',mode='ticks',ticks='14000000');self.check(f['p1']['peripherals']['uart']['uart0']['received'][-1]['valid'],'UART recovers on a later valid frame after protocol fault revocation');self.replay(r,label)
        card=read(ROOT/'registry/boards/tang-nano-9k.phase2.virtual.json')
        for c in card['components']:
            if c['id']=='uart0':c['params']['parity']='even'
        write(self.work/'parity-board.json',card);p['board_path']=(self.work/'parity-board.json').as_posix();even,evenCfg=assemble(p,self.work/'even-build',self.build);r=self.client(even,evenCfg,'parity',[stop,rule('flip','uart_parity_flip','uart0','component','protocol.transform')]);r.call('uart.send',target='uart0',bytes=[85]);f=r.call('step',mode='ticks',ticks='14000000');self.check(f['p1']['peripherals']['uart']['uart0']['received'][0]['parity_error'],'actual parity wire inversion reaches parity checker');self.replay(r,'parity-flip')
    def probes(self):
        result,cfg=assemble(self.minimal(),self.work/'counter-build',self.build);captures=[]
        for name,rules in [('baseline',[]),('open',[rule('private-open','open','wire.led0','connection','topology.transform')])]:
            r=self.client(result,cfg,name,rules);f=self.start(r);writes=f['input_writes'];r.call('probe.attach',probe_id='left',target='wire.led0.a');r.call('probe.attach',probe_id='right',target='wire.led0.b');left=r.call('probe.read',probe_id='left');right=r.call('probe.read',probe_id='right');a=left['instrument_reply'];b=right['instrument_reply'];self.check(left['input_writes']==writes and right['input_writes']==writes,'dual-ended probe measurement performs no DUT writes');self.check(a['state']['valid'] and (not b['state']['valid'] if name=='open' else b['state']['valid']),'line endpoints expose physical '+name+' connection state');self.check(a['voltage_model']=='digital_nominal_estimate_not_spice' and a['nominal_mv']==0,'digital meter reports nominal Bank voltage model explicitly')
            config=dict(source='virtual_probe',targets=['pad.10','led0.K'],depth=8,pretrigger=2,decimation=2,trigger=dict(kind='mask_equal',mask=0,value=0,count=3));r.call('capture.arm',config=config)
            for i in range(20):f=r.call('step')
            capture=r.call('capture.read')['instrument_reply'];self.check(not capture['armed'] and capture['triggered'] and capture['trigger_index']==2 and len(capture['samples'])==8,'circular pretrigger and decimation preserve chronological samples');captures.append(capture);self.replay(r,'capture-'+name)
        from jsonschema import Draft202012Validator
        for capture in captures:Draft202012Validator(read(ROOT/'registry/schemas/virtual-capture.v1.schema.json')).validate(capture)
        self.check(True,'both capture artifacts conform to versioned extended physical capture schema');write(self.work/'baseline-capture.json',captures[0]);write(self.work/'fault-capture.json',captures[1]);self.report['captures']=captures
        self.run([self.build/'tests/virtual/p2/p2_capture_artifacts.exe',self.work/'baseline-capture.json',self.work/'fault-capture.json',self.work/'comparison','diverged'],'strict-compare');compared=read(self.work/'comparison/comparison.json');self.check(compared['signal']=='led0.K' and compared['expected_valid'] and not compared['actual_valid'],'WaveCompareSession reports first actual wire-end validity divergence');self.check('bx ' in Path(compared['artifacts']['vcd']).read_text(encoding='utf-8'),'CaptureDecoder exports invalid physical sample as x, never fabricated zero');self.report['comparison']=compared
        wrong=copy.deepcopy(captures[1]);wrong['sample_phase']='pre_eval';write(self.work/'wrong-phase.json',wrong);self.run([self.build/'tests/virtual/p2/p2_capture_artifacts.exe',self.work/'baseline-capture.json',self.work/'wrong-phase.json',self.work/'wrong-phase','inconclusive'],'reject-phase')
        wrong=copy.deepcopy(captures[1]);wrong['samples'][-1]['edge_index']=str(int(wrong['samples'][-1]['edge_index'])+2);write(self.work/'wrong-edge.json',wrong);self.run([self.build/'tests/virtual/p2/p2_capture_artifacts.exe',self.work/'baseline-capture.json',self.work/'wrong-edge.json',self.work/'wrong-edge','inconclusive'],'reject-edge')
        r=self.client(result,cfg,'trigger-contract',[])
        for kind in ['edge_rising','edge_falling','stall','timeout']:
            config=dict(source='virtual_probe',targets=['pad.3'],depth=3,pretrigger=0,decimation=1,max_samples=32,trigger=dict(kind=kind,mask=0 if kind in ['stall','timeout'] else 1,value=0,count=2))
            r.call('capture.arm',config=config)
            for i in range(14):r.call('set',target='key_p3',state=13 if i%2==0 else 2);r.call('step')
            capture=r.call('capture.read')['instrument_reply'];self.check(capture['triggered'] and not capture['armed'] and len(capture['samples'])==3,'real runner '+kind+' trigger produces bounded post-trigger window')
        config.update(sample_en=False,max_edges=2);r.call('capture.arm',config=config)
        for i in range(4):r.call('step')
        gated=r.call('capture.read')['instrument_reply'];self.check(not gated['armed'] and not gated['triggered'] and not gated['samples'],'sample_en gating respects finite observation budget without fabricated data');r.close();self.clients.remove(r)
        request=dict(project=str(ROOT/'examples/virtual/counter/virtual.project.json'),build_dir=str(self.build),output=str(self.work/'rtl'),depth=16,probes=['led_n[0]','led_n[1]']);write(self.work/'rtl-request.json',request);self.run([sys.executable,ROOT/'tools/virtual/build_virtual_tracebridge.py','--request',self.work/'rtl-request.json','--result',self.work/'rtl-result.json'],'rtl-tracebridge');rtl=read(self.work/'rtl-result.json');types={f['type'] for f in rtl['sent_cobs_crc16_frames']};self.check({1,3,5,7,9,11}<=types and rtl['ping_verified'] and len(rtl['samples'])==16,'actual RTL consumes PING GET_INFO CONFIG ARM STATUS READ_CAPTURE COBS+CRC16 frames');self.check(rtl['source_unchanged'] and [s['value'] for s in rtl['independent_observations']]==rtl['samples'],'decoded RTL capture equals independent wrapper probe samples and student files remain unchanged');self.report['rtl']=rtl
        write(self.work/'rtl-reference.json',rtl['independent_reference']);write(self.work/'rtl-samples.json',rtl['capture_snapshot']);self.run([self.build/'tests/virtual/p2/p2_capture_artifacts.exe',self.work/'rtl-reference.json',self.work/'rtl-samples.json',self.work/'rtl-comparison','matched'],'rtl-strict-compare')
        offset=self.work/'offset-project';offset.mkdir(exist_ok=True);(offset/'offset.sv').write_text("module offset_top(input clock_in,reset_n,output [7:4] y,output [0:0] z); assign y=4'b1010;assign z=1'b1;endmodule\n",encoding='ascii');project=read(ROOT/'examples/virtual/counter/virtual.project.json');project.update(top_module='offset_top',source_files=['offset.sv']);write(offset/'virtual.project.json',project)
        request.update(project=str(offset/'virtual.project.json'),output=str(self.work/'rtl-offset'));request.pop('probes',None);write(self.work/'offset-request.json',request);self.run([sys.executable,ROOT/'tools/virtual/build_virtual_tracebridge.py','--request',self.work/'offset-request.json','--result',self.work/'offset-result.json'],'rtl-offset-indices');offset_result=read(self.work/'offset-result.json');self.check(set(offset_result['capture_snapshot']['targets'])=={'port:y[4]','port:y[5]','port:y[6]','port:y[7]','port:z[0]'} and offset_result['source_unchanged'],'real RTL auto-probe retains nonzero HDL ranges and single-bit vector [0] without source changes');self.report['rtl_nonzero_indices']=offset_result
    def teaching(self):
        sys.path.insert(0,str(ROOT/'agent/src'));from sigflow_edu_agent.virtual_teaching_client import VirtualTeachingClient
        tampered=self.work/'tampered-course';shutil.copytree(ROOT/'examples/virtual/courses/course-01-open-wire',tampered,dirs_exist_ok=True);(tampered/'faults.private.json').write_text('{}',encoding='utf-8')
        try:verify_package(tampered);denied=False
        except Exception:denied=True
        self.check(denied,'Ed25519 course digest rejects modified private plan before runner assembly')
        for id in ['course-01-open-wire','course-02-bridge-contention','course-03-button-debounce','course-04-uart-clock-fault']:
            service=TeachingService(id,self.build,self.work/(id+'-'+uuid.uuid4().hex));gateway=service.serve_http();client=VirtualTeachingClient(gateway['url'],gateway['bearer'],service.token);responses=[]
            try:
                frame=client.session_read();responses.append(frame);self.check(frame['mode']=='student_blind_test' and 'faults' not in frame and 'p1' not in frame,'student projection excludes private runtime checkpoint for '+id)
                if 'open-wire' in id:
                    cfg=read(service.result['session_config']);cfg['teaching']=dict(mode='student_blind_test',run_token=service.token);direct=Runner(service.result,cfg,self.work/('direct-blind-runner-'+uuid.uuid4().hex))
                    try:
                        projected=direct.call('get');journal=direct.call('journal');self.check('p1' not in projected and 'faults' not in projected and service.plan['faults'][0]['id'] not in json.dumps([projected,journal]),'actual C++ Runner independently projects private STATE sources and journal before stdout')
                        try:direct.call('profile.load',profile=service.plan);denied=False
                        except RuntimeError:denied=True
                        self.check(denied,'actual C++ Runner rejects blind profile mutations independently of Python host')
                    finally:direct.close()
                for bearer,token,code in [('wrong-bearer',service.token,401),(gateway['bearer'],'wrong-run-token',400)]:
                    request=urllib.request.Request(gateway['url'],data=json.dumps(dict(capability='virtual.session.read',run_token=token)).encode(),headers={'Authorization':'Bearer '+bearer,'Content-Type':'application/json'})
                    try:urllib.request.urlopen(request,timeout=10);denied=False
                    except urllib.error.HTTPError as e:denied=e.code==code
                    self.check(denied,'HTTP gateway rejects unauthorized bearer or foreign run ownership')
                for op in ['fault.revoke','profile.load','replay.file']:
                    try:service.handle(dict(op=op,fault_id=service.plan['faults'][0]['id'],role='teacher'));denied=False
                    except ValueError:denied=True
                    self.check(denied,'blind host denies '+op+' even with client role elevation')
                try:client.hint_request('L4');denied=False
                except Exception:denied=True
                self.check(denied,'L4 cannot be requested before lower levels')
                if 'clock' in id:service.handle(dict(op='step'))
                else:
                    service.handle(dict(op='step'));service.handle(dict(op='set',target='reset_p4',state=2));service.handle(dict(op='set',target='key_p3',state=13));service.handle(dict(op='step',mode='ticks',ticks='20000000000') if 'button' in id else dict(op='step'))
                targets=['pad.10','led0.K'] if 'open' in id else ['pad.10','pad.11'] if 'bridge' in id else ['pad.3','port:led_n[3]'] if 'button' in id else ['pad.52','port:led_n[0]']
                for i,target in enumerate(targets):
                    service.handle(dict(op='probe.attach',probe_id='p'+str(i),target=target));responses.append(service.handle(dict(op='probe.read',probe_id='p'+str(i))))
                after='0';through=service.frame['trace']['revision']
                while after!=through:
                    page=service.handle(dict(op='trace.commits',after_revision=after,through_revision=through,limit=32));responses.append(page);after=page['next_revision']
                evidence=client.evidence_query();ids=[m['evidence_id'] for m in evidence['measurements']]
                for level in ['L1','L2','L3']:responses.append(client.hint_request(level))
                service.handle(dict(op='probe.read',probe_id='p0'));responses.append(client.hint_request('L4'))
                wrong=client.grade_submit(dict(location='pad.999',type='unknown'),ids,{});self.check(wrong['status']=='fail','wrong localization cannot pass by self-reported repair');responses.append(wrong)
                unrepaired=client.grade_submit(service.rubric['cause'],ids,{});self.check(unrepaired['status']!='pass','correct explanation without functional repair cannot pass');responses.append(unrepaired)
                if 'button' in id:
                    solution=service.student_root/'solution.sv';bad=client.grade_submit(service.rubric['cause'],ids,dict(source=str(solution)));self.check(bad['status']=='fail' and bad['seeds_tested']==3,'actual undebounced student RTL fails all-seed functional grading');responses.append(bad);solution.write_bytes((service.folder/'reference-repair.sv').read_bytes());repair=dict(source=str(solution))
                else:repair=dict(restore=service.rubric['allowed_repair'])
                graded=client.grade_submit(service.rubric['cause'],ids,repair);self.check(graded['status']=='pass' and graded['score']==100 and graded['seeds_tested']==3,id+' correct localization + actual repair + three hidden runner seeds passes');responses.append(graded)
                responses.append(service.handle(dict(op='journal')));secrets_=[service.plan['rng']['seed'],*[f['id'] for f in service.plan['faults']],*service.rubric['hidden_seeds']];serialized=json.dumps(responses,ensure_ascii=False);self.check(all(secret not in serialized for secret in secrets_) and 'fault_id' not in serialized and 'rubric' not in serialized,'all student DTO/journal/hint/grade replies contain zero private ids, values or rubric')
                write(self.work/(id+'-student-dtos.json'),responses)
            finally:service.close()
def main():
    p=argparse.ArgumentParser();p.add_argument('--build-dir',type=Path,default=ROOT/'build-m0');p.add_argument('--mode',choices=['faults','probes','teaching'],required=True);a=p.parse_args();test=Acceptance(a.build_dir,a.mode);(test.work/'progress.log').write_text('',encoding='utf-8')
    try:getattr(test,a.mode)();test.report['status']='passed'
    except Exception as e:test.report.update(status='failed',error=str(e));raise
    finally:
        for client in test.clients:client.close()
        test.report['check_count']=len(test.report['checks']);write(ROOT/'.work/p2'/(a.mode+'-results.json'),test.report)
    print('PASS P2 '+a.mode+': '+str(test.report['check_count'])+' real-tool assertions')
if __name__=='__main__':main()
