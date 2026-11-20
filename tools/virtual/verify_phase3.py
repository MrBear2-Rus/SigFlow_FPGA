"""Real Phase 3 acceptance. No mocked DUT and no fabricated frames.

mode=codec     native SFVB algebra + a real Verilator trajectory (four P1
               faults, bridge contention, PWM segments, bit-level UART and a
               blind student projection) encoded, decoded and compared with
               the Node.js DataView/BigInt reference decoder.
mode=peripherals  real Verilator keypad scan/ghost projects plus the new
               reset_glitch and clock_divider_offset timing faults.
"""
import argparse,copy,json,os,subprocess,sys,time,uuid
from pathlib import Path
from virtual_teaching_service import Runner,assemble,read,write,ROOT
sys.path.insert(0,str(Path(__file__).resolve().parent))
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
def rule(id,type,target,kind,hook,effect=None,trigger=None):
    body=dict(effect or {},type=type)
    return dict(id=id,enabled=True,visibility='teacher_only',target=dict(id=target,kind=kind),hook=hook,effect=body,trigger=trigger or dict(kind='permanent',start_tick='0'))
def bounce_edges():
    edges=[dict(dt='0',contact='closed')]
    for dt in ('2','5','9'):edges.append(dict(dt=dt,contact='released'))
    edges.append(dict(dt='14',contact='closed'))
    return edges
class Acceptance:
    def __init__(self,build,mode):
        self.build=Path(build).resolve();self.work=ROOT/'.work/p3'/mode;self.work.mkdir(parents=True,exist_ok=True)
        self.report=dict(status='running',checks=[],commands=[]);self.mode=mode;self.clients=[]
    def check(self,ok,text):
        with (self.work/'progress.log').open('a',encoding='utf-8') as f:f.write(str(bool(ok))+' '+text+'\n')
        if not ok:raise AssertionError(text)
        self.report['checks'].append(text)
    def run(self,args,label,timeout=900):
        env=dict(os.environ);env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+str(self.build)+';'+env.get('PATH','')
        started=time.monotonic();r=subprocess.run([str(x) for x in args],cwd=ROOT,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=timeout)
        (self.work/(label+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8')
        self.report['commands'].append(dict(argv=[str(x) for x in args],exit_code=r.returncode,seconds=round(time.monotonic()-started,3)))
        self.check(r.returncode==0,label+' actual process exits successfully: '+(r.stdout+r.stderr)[-400:])
        return r
    def client(self,result,cfg,label,rules):
        cfg=copy.deepcopy(cfg);cfg['fault_profile']['faults']=rules
        r=Runner(result,cfg,self.work/(label+'-'+uuid.uuid4().hex));self.clients.append(r);return r
    def replay(self,r,label):
        f=r.call('stop');replay=r.call('replay.current',repetitions=3)
        self.check(replay['status']=='passed' and len(replay['attempts'])==3 and all(a['circuit_digest']==f['digests']['circuit'] and a['control_digest']==f['digests']['control'] for a in replay['attempts']),label+' three from-zero real RTL replays preserve both digests')
        self.report.setdefault('replays',[]).append(dict(label=label,**replay));r.close();self.clients.remove(r)
    def native(self):
        from jsonschema import Draft202012Validator
        for p in (ROOT/'registry/schemas').glob('*.schema.json'):Draft202012Validator.check_schema(read(p))
        profile=read(ROOT/'registry/schemas/fault-profile.v2.schema.json')
        for folder in (ROOT/'examples/virtual/courses').iterdir():
            if folder.is_dir():Draft202012Validator(profile).validate(read(folder/'faults.private.json'))
        for project in ('keypad-scan','keypad-ghost'):
            p=read(ROOT/'examples/virtual/phase3'/project/'virtual.project.json');Draft202012Validator(profile).validate(p['fault_profile'])
        injected=dict(faults=[
            rule('p3-reset-glitch','reset_glitch','key_p4','component','peripheral.event',dict(pressed_state=0,duration_ticks='1500'),dict(kind='transient',start_tick='200000',end_tick='2000000')),
            rule('p3-ratio','clock_divider_offset','clk27m','clock','clock.schedule',dict(den_offset='1',resume_phase='restart_half_period'),dict(kind='transient',start_tick='0',end_tick='300000'))])
        Draft202012Validator(profile).validate(dict(read(ROOT/'examples/virtual/phase3/keypad-ghost/virtual.project.json')['fault_profile'],**injected))
        self.check(True,'versioned schemas validate courses, phase 3 projects and the new timing fault rules')
        self.run([self.build/'tests/virtual/p3/p3_native_tests.exe','native',ROOT,self.work],'native')
        native=read(self.work/'native.json');self.check(native['status']=='passed' and native['checks']>=79,'native codec/fault/keypad suite passes '+str(native['checks'])+' checks')
        self.report['native']=native
    def codec(self):
        p=read(ROOT/'examples/virtual/phase2/virtual.project.json');p['project_root']=(ROOT/'examples/virtual/phase2').as_posix()
        p['fault_profile']['rng']['seed']='3100002'
        p['fault_profile']['faults']=[
            rule('hidden-stuck','stuck_at','fpga0.pad.10','fpga_pad_driver','driver.transform',dict(mode='replace_source',value=1)),
            rule('hidden-open','open','wire.led1','connection','topology.transform'),
            rule('hidden-bounce','button_bounce','key_p3','component','peripheral.event',dict(drive='open_drain',duration_ticks='14',relative_edges=bounce_edges()),
                dict(kind='intermittent',event=dict(type='input.intent',target='key_p3',action='press',occurrence=1),probability_ppm=1000000,active_ticks='200',cooldown_ticks='500',max_activations=1)),
            rule('hidden-clock','clock_stop','clk27m','clock','clock.schedule',dict(hold_level=0,resume_phase='restart_half_period'),dict(kind='transient',start_tick='3000000',end_tick='6000000')),
            rule('hidden-bridge','bridge','pad.10','node','topology.transform',dict(other_node='pad.11'))]
        result,cfg=assemble(p,self.work/'codec-build',self.build)
        r=self.client(result,cfg,'trajectory',p['fault_profile']['faults'])
        frames=[]
        frames.append(r.call('get'))
        frames.append(r.call('set',target='reset_p4',state=2))
        frames.append(r.call('set',target='key_p3',state=13))
        frames.append(r.call('step'))
        frames.append(r.call('uart.send',target='uart0',bytes=[65,66]))
        frames.append(r.call('dip.set',target='dip0',value=2))
        advanced=r.call('step',mode='ticks',ticks='4000000')
        self.check(advanced['status']=='applied' and advanced['reason']=='StepComplete','real trajectory advances through UART/DIP/multi-clock steps')
        frames.append(advanced)
        frames.append(r.call('step',mode='ticks',ticks='8000000'))
        frames.append(r.call('set',target='key_p3',state=2))
        frames.append(r.call('step'))
        frames.append(r.call('step'))
        signals=None;batches=[]
        after='0'
        while True:
            page=r.call('trace.commits',after_revision=after,limit=128)
            if signals is None:signals=page['signals']
            for batch in page['batches']:
                batches.append(dict(revision=batch['revision'],events=batch['events']))
            after=page['next_revision']
            if page['complete'] or not page['batches']:break
        self.check(len(batches)>=40,'real trace sweep collects per-commit batches: '+str(len(batches)))
        self.replay(r,'trajectory')
        run_one=dict(frames=frames,trace_batches=batches)
        # Blind student projection: the same declaration through the trusted
        # blind-mode runner; every reply is projected before it leaves the process.
        blind_cfg=copy.deepcopy(cfg);blind_cfg['teaching']=dict(mode='student_blind_test',run_token='opaque-p3-blind-run-token-0123456789abcdef')
        b=Runner(result,blind_cfg,self.work/('blind-'+uuid.uuid4().hex));self.clients.append(b)
        blind=[]
        blind.append(b.call('get'))
        blind.append(b.call('step'))
        blind.append(b.call('set',target='key_p3',state=13))
        blind.append(b.call('step',mode='ticks',ticks='4000000'))
        blind.append(b.call('step'))
        blind_batches=[];after='0'
        while True:
            page=b.call('trace.commits',after_revision=after,limit=128)
            for batch in page['batches']:blind_batches.append(dict(revision=batch['revision'],events=batch['events']))
            after=page['next_revision']
            if page['complete'] or not page['batches']:break
        blind_run=dict(frames=blind,trace_batches=blind_batches)
        b.call('stop');b.close();self.clients.remove(b)
        trajectory=dict(run_handle='7777',generation='1',signals=signals,runs=[run_one,blind_run])
        write(self.work/'trajectory.json',trajectory)
        self.run([self.build/'tests/virtual/p3/p3_native_tests.exe','encode',self.work/'trajectory.json',self.work/'frames.sfvb',self.work/'frames.json',self.work/'decoded.json'],'sfvb-encode')
        self.run(['node',ROOT/'tools/virtual/sfvb_reference_decoder.js',self.work/'frames.sfvb',self.work/'js-decoded.json'],'sfvb-js-decode')
        source=json.loads((self.work/'frames.json').read_text(encoding='utf-8'))
        decoded=json.loads((self.work/'decoded.json').read_text(encoding='utf-8'))
        javascript=json.loads((self.work/'js-decoded.json').read_text(encoding='utf-8'))
        self.check(source==decoded,'C++ binary decode is losslessly equivalent to the source JSON delta frames')
        self.check(source==javascript,'Node.js DataView/BigInt decode matches the C++ frames field by field')
        canonical=lambda value:json.dumps(value,sort_keys=True,ensure_ascii=False,indent=1)
        self.check(canonical(source)==canonical(javascript),'cross-language canonical dumps are byte-identical')
        kinds=[f['kind'] for f in source['frames']]
        self.check(kinds[0]=='snapshot' and kinds.count('delta')>=10 and kinds.count('snapshot')>=2,'full snapshot opens the stream and the blind run forces a real re-baseline')
        stuck=[change for frame in source['frames'] for change in frame['pad_changes'] if change['target']=='pad.10' and change['state_code']==15]
        contention=[change for frame in source['frames'] for change in frame['pad_changes'] if change['contention']]
        self.check(stuck and contention,'decoded stream keeps stuck strong evidence and 13-state bridge contention')
        uart=[component for frame in source['frames'] for component in frame['components'] if component['id']=='uart0' and component['state'].get('transmitted')]
        self.check(uart,'decoded stream carries the bit-level UART transmit history')
        blind_frames=[frame for frame in source['frames'] if frame['run_id']=='opaque-p3-blind-run-token-0123456789abcdef']
        self.check(len(blind_frames)>=4,'the blind run contributes its own projected frames')
        blind_blob=json.dumps(blind_frames,ensure_ascii=False)
        for secret in ('hidden-stuck','hidden-bounce','hidden-bridge','hidden-open','hidden-clock','3100002','fault_id','rubric'):
            self.check(secret not in blind_blob,'blind decoded frames leak no '+secret)
        teacher_frames=[frame for frame in source['frames'] if frame['run_id']!='opaque-p3-blind-run-token-0123456789abcdef']
        self.check(any('fault_id' in json.dumps(frame) for frame in teacher_frames),'teacher-run frames keep their deterministic fault ledger')
        self.run([self.build/'tests/virtual/p3/p3_native_tests.exe','decode-broken',self.work/'frames.sfvb','gap',self.work/'broken-gap.json'],'sfvb-gap')
        self.run([self.build/'tests/virtual/p3/p3_native_tests.exe','decode-broken',self.work/'frames.sfvb','baseline',self.work/'broken-baseline.json'],'sfvb-baseline')
        self.run([self.build/'tests/virtual/p3/p3_native_tests.exe','decode-broken',self.work/'frames.sfvb','stale',self.work/'broken-stale.json'],'sfvb-stale')
        for label,expected in (('gap','E_SFVB_SEQ_GAP'),('baseline','E_SFVB_BASELINE_LOST'),('stale','E_SFVB_GENERATION_STALE')):
            broken=read(self.work/('broken-'+label+'.json'))
            self.check(broken['status']=='passed' and broken['observed']==expected,label+' corruption is rejected with '+expected)
        self.report['codec']=dict(frames=len(source['frames']),batches=len(batches),blind_frames=len(blind))
    def peripherals(self):
        from jsonschema import Draft202012Validator
        profile=read(ROOT/'registry/schemas/fault-profile.v2.schema.json')
        scan=read(ROOT/'examples/virtual/phase3/keypad-scan/virtual.project.json');scan['project_root']=(ROOT/'examples/virtual/phase3/keypad-scan').as_posix()
        ghost=read(ROOT/'examples/virtual/phase3/keypad-ghost/virtual.project.json');ghost['project_root']=(ROOT/'examples/virtual/phase3/keypad-ghost').as_posix()
        diode_card=read(ROOT/'registry/boards/tang-nano-9k.phase3.virtual.json')
        for component in diode_card['components']:
            if component['id']=='keypad0':component['params']['diodes']=True
        write(self.work/'keypad-diodes-board.json',diode_card)
        scan_diode=copy.deepcopy(scan);scan_diode['board_path']=(self.work/'keypad-diodes-board.json').as_posix()
        ghost_diode=copy.deepcopy(ghost);ghost_diode['board_path']=(self.work/'keypad-diodes-board.json').as_posix()
        for project in (scan,scan_diode,ghost,ghost_diode):Draft202012Validator(profile).validate(project['fault_profile'])
        self.check(True,'phase 3 projects and the diode board variant validate against the versioned schema')
        keys=[[0,0],[0,1],[1,0]]
        # 1. Push-pull scanner without diodes: multi-key press short-circuits the
        #    scan network and the strict import policy pauses with CON evidence.
        result,cfg=assemble(scan,self.work/'scan-build',self.build)
        r=self.client(result,cfg,'scan-contention',[])
        r.call('set',target='reset_p4',state=2)
        r.call('step')
        r.call('keypad.set',target='keypad0',keys=keys)
        blocked=r.call('step')
        self.check(blocked['status']=='blocked' and blocked['reason']=='UnknownInput' and blocked['error']=='E_UNKNOWN_INPUT','diode-less scanner contention pauses before importing contested columns')
        self.check(blocked['guard']['state']==46 and len(blocked['guard']['drivers'])>=2,'guard keeps the folded CON state and both fighting scan drivers')
        r.call('keypad.set',target='keypad0',keys=[])
        recovered=r.call('step')
        self.check(recovered['reason']=='StepComplete','releasing the keys unblocks the strict guard')
        self.replay(r,'scan-contention')
        # 2. Same scanner with anti-ghost diodes: three keys, no contention, and
        #    the ghost candidate (1,1) never appears in any scan slot.
        result,cfg=assemble(scan_diode,self.work/'scan-diode-build',self.build)
        r=self.client(result,cfg,'scan-diodes',[])
        r.call('set',target='reset_p4',state=2)
        r.call('step')
        r.call('keypad.set',target='keypad0',keys=keys)
        col0=0;col1=0
        for index in range(8):
            frame=r.call('step',mode='ticks',ticks='1000000')
            self.check(frame['status']=='applied','diode scanner slot '+str(index)+' advances without contention')
            outputs=frame['outputs']
            if outputs['led_n[0]'] is False:col0+=1
            if outputs['led_n[1]'] is False:col1+=1
        self.check(col0==4 and col1==2,'two rotations detect the three real keys and the ghost column stays high in the row1 slot')
        keypad=frame['board']['peripherals']['keypad0']
        self.check(keypad['diodes'] is True and keypad['ghosts']==[] and len(keypad['closed'])==3,'diode model keeps exactly the three closed keys and no ghosts')
        self.replay(r,'scan-diodes')
        # 3. Open-drain style manual scan without diodes: the classic ghost key
        #    emerges from the real merged junction and the DUT sees it.
        result,cfg=assemble(ghost,self.work/'ghost-build',self.build)
        r=self.client(result,cfg,'ghost-junction',[])
        r.call('set',target='reset_p4',state=2)
        r.call('keypad.set',target='keypad0',keys=keys)
        r.call('step')
        r.call('set',target='keypad_row1',state=13)
        scanned=r.call('step')
        keypad=scanned['board']['peripherals']['keypad0']
        self.check(keypad['ghosts']==[['1','1']] and keypad['apparent'].get('1,1') is True,'ghost key (1,1) emerges from the real merged row/column junction')
        self.check(scanned['board']['nodes']['pad.30']['logic']=='0' and scanned['outputs']['led_n[1]'] is False,'DUT input sees the ghost column as pressed')
        self.check(scanned['outputs']['led_n[4]'] is False,'the merged junction also drags the released row indicator low')
        self.replay(r,'ghost-junction')
        # 4. Same stimulus with diodes: only the real junction conducts.
        result,cfg=assemble(ghost_diode,self.work/'ghost-diode-build',self.build)
        r=self.client(result,cfg,'ghost-diodes',[])
        r.call('set',target='reset_p4',state=2)
        r.call('keypad.set',target='keypad0',keys=keys)
        r.call('step')
        r.call('set',target='keypad_row1',state=13)
        clean=r.call('step')
        keypad=clean['board']['peripherals']['keypad0']
        self.check(keypad['ghosts']==[] and keypad['apparent'].get('1,1') is None,'anti-ghost diodes suppress the ghost junction')
        self.check(clean['board']['nodes']['pad.30']['logic']=='1' and clean['outputs']['led_n[1]'] is True,'released row cannot back-feed the column through a diode')
        self.replay(r,'ghost-diodes')
        # 5. reset_glitch: the exact-width pulse resets asynchronous logic while
        #    the clock is fully held, so no clock edge can explain it.
        result,cfg=assemble(ghost,self.work/'glitch-build',self.build)
        faults=[
            rule('hold-clock','clock_stop','clk27m','clock','clock.schedule',dict(hold_level=0,resume_phase='restart_half_period'),dict(kind='permanent',start_tick='55556')),
            rule('reset-glitch-1','reset_glitch','key_p4','component','peripheral.event',dict(pressed_state=0,duration_ticks='1500'),dict(kind='transient',start_tick='200000',end_tick='2000000'))]
        Draft202012Validator(profile).validate(dict(read(ROOT/'examples/virtual/phase3/keypad-ghost/virtual.project.json')['fault_profile'],faults=faults))
        # Clock stop must not be armed at construction: the scan runs first and
        # the mid-session profile.load holds the clock only for the glitch leg.
        r=self.client(result,cfg,'reset-glitch',[])
        r.call('set',target='reset_p4',state=2)
        r.call('step')
        r.call('set',target='keypad_row1',state=13)
        lit=r.call('step')
        self.check(lit['outputs']['led_n[5]'] is False,'row indicator shows the manual scan before the glitch window')
        r.call('profile.load',profile=dict(read(ROOT/'examples/virtual/phase3/keypad-ghost/virtual.project.json')['fault_profile'],faults=faults))
        glitched=r.call('step',mode='ticks',ticks='2000000')
        self.check(glitched['position']['tick']=='2055556','glitch step lands on its target with the clock held')
        self.check(all(glitched['outputs']['led_n['+str(i)+']'] is True for i in range(6)),'narrow async reset glitch cleared the DUT with zero clock edges')
        records=r.call('journal')['records']
        edges=[event for record in records if record.get('kind')=='commit' for event in record.get('p1',{}).get('faults',{}).get('events',[]) if event.get('type')=='pin.edge']
        self.check(len(edges)==2 and edges[0]['tick']=='200000' and edges[1]['tick']=='201500','journal keeps the exact press/release glitch edges')
        self.replay(r,'reset-glitch')
        # 6. clock_divider_offset: the measured period doubles while active and
        #    restarts on the base half period after the window.
        result,cfg=assemble(scan,self.work/'ratio-build',self.build)
        faults=[rule('ratio-2','clock_divider_offset','clk27m','clock','clock.schedule',dict(den_offset='1',resume_phase='restart_half_period'),dict(kind='transient',start_tick='0',end_tick='300000'))]
        r=self.client(result,cfg,'divider-offset',faults)
        r.call('set',target='reset_p4',state=2)
        first=r.call('step')
        self.check(first['position']['tick']=='37037','halved clock moves the first real rising edge to the doubled half period')
        second=r.call('step')
        self.check(second['position']['tick']=='111111','second rising edge stays on the offset schedule')
        window=r.call('step',mode='ticks',ticks='300000')
        self.check(window['status']=='applied','offset window completes on the real clock')
        resume=r.call('step')
        self.check(int(resume['position']['tick'])-411111==18519,'after the window the base half-period schedule restarts')
        self.replay(r,'divider-offset')
        self.report['peripherals']=dict(contention_guard=46,ghost='1,1',diodes=True)
    def finish(self,status,**extra):
        self.report['status']=status;self.report.update(extra)
        self.report['check_count']=len(self.report['checks'])
        write(self.work/(self.mode+'-results.json'),self.report)
        for r in self.clients:
            try:r.close()
            except Exception:pass
        print(json.dumps(self.report,ensure_ascii=False)[:400])
def main():
    p=argparse.ArgumentParser();p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--mode',choices=['codec','peripherals'],required=True);a=p.parse_args()
    acceptance=Acceptance(a.build_dir,a.mode)
    try:
        if a.mode=='codec':
            acceptance.native();acceptance.codec()
        else:
            acceptance.peripherals()
        acceptance.finish('passed')
    except Exception as e:
        acceptance.finish('failed',error=str(e))
        print(str(e),file=sys.stderr)
        return 1
    return 0
if __name__=='__main__':raise SystemExit(main())
