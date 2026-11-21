"""Real Phase 4 acceptance: advanced bus peripherals, bus/protocol/CDC faults.

mode=bus       I2C open-drain sensor + SPI OLED, i2c_nak and spi_mode_fault.
mode=protocol  crc_tamper upper-layer CRC, cdc_sampling_approx, SFVB parity.
"""
import argparse,copy,json,os,subprocess,sys,time,uuid,shutil
from pathlib import Path
from virtual_teaching_service import Runner,assemble,read,write,ROOT
sys.path.insert(0,str(Path(__file__).resolve().parent))
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
def rule(id,type,target,kind,hook,effect=None,trigger=None):
    body=dict(effect or {},type=type)
    return dict(id=id,enabled=True,visibility='teacher_only',target=dict(id=target,kind=kind),hook=hook,effect=body,trigger=trigger or dict(kind='permanent',start_tick='0'))
def crc8(data):
    crc=0
    for byte in data:
        crc^=byte
        for _ in range(8):crc=((crc<<1)^0x07)&0xFF if crc&0x80 else (crc<<1)&0xFF
    return crc
class Acceptance:
    def __init__(self,build,mode):
        self.build=Path(build).resolve();self.work=ROOT/'.work/p4'/mode;self.work.mkdir(parents=True,exist_ok=True)
        self.report=dict(status='running',checks=[],commands=[],replays=[]);self.mode=mode;self.clients=[]
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
        folder=self.work/(label+'-'+uuid.uuid4().hex);shutil.rmtree(folder,ignore_errors=True)
        r=Runner(result,cfg,folder);self.clients.append(r);return r
    def replay(self,r,label):
        f=r.call('stop');replay=r.call('replay.current',repetitions=3)
        self.check(replay['status']=='passed' and len(replay['attempts'])==3 and all(a['circuit_digest']==f['digests']['circuit'] and a['control_digest']==f['digests']['control'] for a in replay['attempts']),label+' three from-zero real RTL replays preserve both digests')
        self.report['replays'].append(dict(label=label,**replay));r.close();self.clients.remove(r)
    def project(self,name):
        path=ROOT/f'examples/virtual/phase4/{name}/virtual.project.json'
        project=read(path);project['project_root']=(ROOT/f'examples/virtual/phase4/{name}').as_posix();return project
    def native(self):
        from jsonschema import Draft202012Validator
        profile=read(ROOT/'registry/schemas/fault-profile.v2.schema.json')
        for p in (ROOT/'registry/schemas').glob('*.schema.json'):Draft202012Validator.check_schema(read(p))
        for folder in (ROOT/'examples/virtual/courses').iterdir():
            if folder.is_dir():Draft202012Validator(profile).validate(read(folder/'faults.private.json'))
        for name in ('i2c-temp','spi-oled','crc-cdc'):
            Draft202012Validator(profile).validate(read(ROOT/f'examples/virtual/phase4/{name}/virtual.project.json')['fault_profile'])
        # The four Phase 4 effects must validate inside a real profile envelope.
        injected=dict(read(ROOT/'examples/virtual/phase4/i2c-temp/virtual.project.json')['fault_profile'],faults=[
            rule('p4-nak','i2c_nak','i2c0','component','peripheral.event',dict(phase='address',nth_transaction=1)),
            rule('p4-slip','spi_mode_fault','oled0','component','peripheral.event',dict(mode='bit_slip',slip_bits=1))])
        Draft202012Validator(profile).validate(injected)
        Draft202012Validator(profile).validate(dict(read(ROOT/'examples/virtual/phase4/crc-cdc/virtual.project.json')['fault_profile'],faults=[
            rule('p4-crc','crc_tamper','uart0','component','protocol.transform',dict(protocol_id='sigflow.uart-packet-crc8.v1',xor_mask=255,frame_index=0)),
            rule('p4-cdc','cdc_sampling_approx','key_p3','input','input.sampling',dict(window_ps='2000',max_delay_cycles=2,policy='seeded_enum'))]))
        self.check(True,'schemas validate every course, phase 4 project and the four new fault effects')
        self.run([self.build/'tests/virtual/p3/p3_native_tests.exe','native',ROOT,self.work],'native')
        native=read(self.work/'native.json');self.check(native['status']=='passed','native algebra suite still passes '+str(native.get('checks'))+' checks')
    def i2c(self,name,label,rules,expect):
        project=self.project(name);result,cfg=assemble(project,self.work/(label+'-build'),self.build)
        r=self.client(result,cfg,label,rules)
        r.call('set',target='reset_p4',state=2);r.call('step')
        frame=r.call('step',mode='ticks',ticks='20000000')
        sensor=frame['board']['peripherals']['i2c0']
        self.check(sensor['transactions']>=1,'real I2C master drives START on the resolved open-drain bus')
        if expect=='read':
            self.check(sensor['ack_count']==1 and sensor['nak_count']==0 and sensor['last_read_byte']=='1a','sensor ACK slot drives SLO and the master reads 0x1a exactly')
            self.check(sensor['registers']==['1a','2b','5c'] and sensor['bus_state']=='idle','register file preserved and bus returned to idle after STOP')
            self.check(frame['outputs']['led_n[0]'] is True and frame['outputs']['led_n[1]'] is False
                and frame['outputs']['led_n[2]'] is True and frame['outputs']['led_n[3]'] is False
                and frame['outputs']['led_n[4]'] is False,'DUT LED bank reproduces every bit of the read byte')
        elif expect=='nak-recover':
            self.check(sensor['nak_count']==1 and sensor['transactions']>=2 and sensor['ack_count']==1,'injected NAK is observed once, then the master retries and the sensor acknowledges')
            self.check(frame['outputs']['led_n[1]'] is False,'recovered read still produces the exact byte on the LED bank')
            ledger=[e for record in r.call('journal')['records'] if record.get('kind')=='commit'
                for e in record.get('p1',{}).get('faults',{}).get('events',[]) if e.get('type')=='i2c.nak_injected']
            self.check(ledger and ledger[0]['phase']=='address' and ledger[0]['transaction']==1,'fault ledger records i2c.nak_injected with phase and transaction')
        self.replay(r,label)
    def bus(self):
        from jsonschema import Draft202012Validator
        profile=read(ROOT/'registry/schemas/fault-profile.v2.schema.json')
        self.i2c('i2c-temp','i2c-read',[],'read')
        # Address mismatch on a derived board proves the error/retry-exhausted path.
        card=read(ROOT/'registry/boards/tang-nano-9k.phase4.virtual.json')
        for component in card['components']:
            if component['id']=='i2c0':component['params']['address_7bit']=0x49
        write(self.work/'i2c-alt-board.json',card)
        project=self.project('i2c-temp');project['board_path']=(self.work/'i2c-alt-board.json').as_posix()
        result,cfg=assemble(project,self.work/'i2c-alt-build',self.build)
        Draft202012Validator(profile).validate(project['fault_profile'])
        r=self.client(result,cfg,'i2c-no-ack',[])
        r.call('set',target='reset_p4',state=2);r.call('step')
        frame=r.call('step',mode='ticks',ticks='20000000')
        sensor=frame['board']['peripherals']['i2c0']
        self.check(sensor['ack_count']==0 and sensor['transactions']>=2,'unmatched address retries without an ACK')
        self.check(frame['outputs']['led_n[0]'] is True and frame['outputs']['led_n[1]'] is False
            and frame['outputs']['led_n[2]'] is True and frame['outputs']['led_n[3]'] is False
            and frame['outputs']['led_n[4]'] is True,'retry-exhausted error pattern is visible on the LED bank')
        self.replay(r,'i2c-no-ack')
        nak=[rule('hidden-nak','i2c_nak','i2c0','component','peripheral.event',dict(phase='address',nth_transaction=1))]
        Draft202012Validator(profile).validate(dict(read(ROOT/'examples/virtual/phase4/i2c-temp/virtual.project.json')['fault_profile'],faults=nak))
        self.i2c('i2c-temp','i2c-nak',nak,'nak-recover')
        # SPI OLED: exact pattern, then a deterministic sampling distortion.
        project=self.project('spi-oled');result,cfg=assemble(project,self.work/'oled-build',self.build)
        r=self.client(result,cfg,'oled',[])
        r.call('set',target='reset_p4',state=2);r.call('step')
        frame=r.call('step',mode='ticks',ticks='34000000')
        oled=frame['board']['peripherals']['oled0']
        self.check(oled['display_on'] is True and oled['inverted'] is False,'SSD1306 teaching commands 0xAE/0xA6/0xAF are decoded from the DC/SCK stream')
        self.check(oled['page']==0 and oled['column']==8 and oled['write_count']==8,'eight data bytes land in page 0 columns 0..7')
        self.check(oled['lit_pixels']==28,'lit pixel count matches the transmitted pattern exactly')
        self.check(oled['frame_sha256']=='307a5bc25d1638e7d825a90ae9acc7ebf9b1db1f19ab0a47e0f94dfe93733180','frame_sha256 is the deterministic VRAM digest')
        self.check(len(oled['pages'])==4 and len(oled['pages'][0]['bytes'])==128,'page snapshot stays compact while remaining exact')
        self.report['oled']=dict(lit_pixels=oled['lit_pixels'],frame_sha256=oled['frame_sha256'],write_count=oled['write_count'])
        self.replay(r,'oled')
        slip=[rule('hidden-slip','spi_mode_fault','oled0','component','peripheral.event',dict(mode='bit_slip',slip_bits=1))]
        Draft202012Validator(profile).validate(dict(read(ROOT/'examples/virtual/phase4/spi-oled/virtual.project.json')['fault_profile'],faults=slip))
        project=self.project('spi-oled');result,cfg=assemble(project,self.work/'oled-slip-build',self.build)
        r=self.client(result,cfg,'oled-slip',slip)
        r.call('set',target='reset_p4',state=2);r.call('step')
        frame=r.call('step',mode='ticks',ticks='34000000')
        slipped=frame['board']['peripherals']['oled0']
        self.check(slipped['frame_sha256']!=oled['frame_sha256'] and slipped['lit_pixels']!=oled['lit_pixels'],'bit-slip fault deterministically corrupts the transmitted pattern')
        self.check(slipped['display_on'] is False,'slipped command stream never latches the display-on command')
        self.report['oled_slip']=dict(lit_pixels=slipped['lit_pixels'],frame_sha256=slipped['frame_sha256'])
        ledger=[e for record in r.call('journal')['records'] if record.get('kind')=='commit'
            for e in record.get('p1',{}).get('faults',{}).get('events',[]) if e.get('type')=='spi.mode_fault_applied']
        self.check(bool(ledger) and ledger[0]['mode']=='bit_slip' and ledger[0]['slip_bits']==1,'fault ledger records spi.mode_fault_applied')
        self.replay(r,'oled-slip')
    def protocols(self):
        from jsonschema import Draft202012Validator
        profile=read(ROOT/'registry/schemas/fault-profile.v2.schema.json')
        payload=[0x11,0x22];packet=[0xAA,len(payload)]+payload;packet.append(crc8(packet))
        def send_and_settle(r):
            r.call('set',target='reset_p4',state=2);r.call('step')
            r.call('uart.send',target='uart0',bytes=packet)
            return r.call('step',mode='ticks',ticks='16000000')
        project=self.project('crc-cdc');result,cfg=assemble(project,self.work/'crc-build',self.build)
        r=self.client(result,cfg,'crc-valid',[])
        frame=send_and_settle(r)
        self.check(frame['outputs']['led_n[0]'] is False and frame['outputs']['led_n[1]'] is False
            and frame['outputs']['led_n[2]'] is True and frame['outputs']['led_n[4]'] is False,'legal packet passes crc8-0x07 with no framing error')
        self.replay(r,'crc-valid')
        tamper=[rule('hidden-crc','crc_tamper','uart0','component','protocol.transform',
            dict(protocol_id='sigflow.uart-packet-crc8.v1',xor_mask=255,frame_index=0))]
        Draft202012Validator(profile).validate(dict(read(ROOT/'examples/virtual/phase4/crc-cdc/virtual.project.json')['fault_profile'],faults=tamper))
        project=self.project('crc-cdc');result,cfg=assemble(project,self.work/'tamper-build',self.build)
        r=self.client(result,cfg,'crc-tamper',tamper)
        frame=send_and_settle(r)
        self.check(frame['outputs']['led_n[1]'] is True and frame['outputs']['led_n[2]'] is False,'tampered CRC byte is rejected by the upper-layer checker')
        self.check(frame['outputs']['led_n[4]'] is False,'bottom-layer UART framing stays fully legal under crc_tamper')
        ledger=[e for record in r.call('journal')['records'] if record.get('kind')=='commit'
            for e in record.get('p1',{}).get('faults',{}).get('events',[]) if e.get('type')=='crc.tampered']
        self.check(bool(ledger) and ledger[0]['protocol_id']=='sigflow.uart-packet-crc8.v1' and ledger[0]['frame_index']==0,'fault ledger records crc.tampered with protocol id and frame index')
        self.replay(r,'crc-tamper')
        # cdc_sampling_approx: press the stimulus inside the violation window
        # before a real clock edge; the seeded draw defers the import.
        cdc=[rule('hidden-cdc','cdc_sampling_approx','key_p3','input','input.sampling',
            dict(window_ps='2000',max_delay_cycles=2,policy='seeded_enum'))]
        Draft202012Validator(profile).validate(dict(read(ROOT/'examples/virtual/phase4/crc-cdc/virtual.project.json')['fault_profile'],faults=cdc))
        project=self.project('crc-cdc');result,cfg=assemble(project,self.work/'cdc-build',self.build)
        runs=[]
        for seed in ('4100101','4100102','4100103'):
            current=copy.deepcopy(project);current['fault_profile']['rng']['seed']=seed;current['fault_profile']['faults']=cdc
            built,config=assemble(current,self.work/('cdc-'+seed),self.build)
            r=self.client(built,config,'cdc-'+seed,cdc)
            r.call('set',target='reset_p4',state=2);r.call('step')
            before=r.call('get')
            edge=int(before['p1']['next_clock_tick'])
            target=edge-1000
            r.call('step',mode='ticks',ticks=str(target-int(before['position']['tick'])))
            r.call('set',target='key_p3',state=13)
            frame=r.call('step')
            records=r.call('journal')['records']
            events=[e for record in records if record.get('kind')=='commit'
                for e in record.get('p1',{}).get('faults',{}).get('events',[]) if e.get('type')=='cdc.sampling_approx']
            self.check(bool(events),'seeded CDC draw fires for an input edge inside the violation window: seed '+seed)
            self.check(events[0]['approximation']=='functional_metastability_approx','CDC ledger marks the functional metastability approximation explicitly')
            self.check(events[0]['policy']=='seeded_enum' and 0<=events[0]['chosen_delay_cycles']<=2,'CDC draw enumerates a whole-cycle sampling delay branch')
            self.report.setdefault('cdc',[]).append(dict(seed=seed,chosen=events[0]['chosen_delay_cycles'],rng=events[0]['rng']))
            runs.append(events[0]['chosen_delay_cycles'])
            self.replay(r,'cdc-'+seed)
        self.check(len(runs)==3,'three fixed seeds exercise the CDC approximation deterministically')
        # SFVB parity over a Phase 4 trajectory (I2C + OLED states included).
        project=self.project('i2c-temp');result,cfg=assemble(project,self.work/'sfvb-build',self.build)
        r=self.client(result,cfg,'sfvb',[])
        frames=[r.call('get')]
        frames.append(r.call('set',target='reset_p4',state=2))
        frames.append(r.call('step'))
        frames.append(r.call('step',mode='ticks',ticks='20000000'))
        signals=None;batches=[];after='0'
        while True:
            page=r.call('trace.commits',after_revision=after,limit=128)
            if signals is None:signals=page['signals']
            for batch in page['batches']:batches.append(dict(revision=batch['revision'],events=batch['events']))
            after=page['next_revision']
            if page['complete'] or not page['batches']:break
        self.check(len(batches)>=100,'Phase 4 trace sweep collects '+str(len(batches))+' per-commit batches')
        r.call('stop');r.close();self.clients.remove(r)
        write(self.work/'p4-trajectory.json',dict(run_handle='9100',generation='1',signals=signals,runs=[dict(frames=frames,trace_batches=batches)]))
        self.run([self.build/'tests/virtual/p3/p3_native_tests.exe','encode',self.work/'p4-trajectory.json',self.work/'p4-frames.sfvb',self.work/'p4-frames.json',self.work/'p4-decoded.json'],'sfvb-encode')
        self.run(['node',ROOT/'tools/virtual/sfvb_reference_decoder.js',self.work/'p4-frames.sfvb',self.work/'p4-js-decoded.json'],'sfvb-js')
        source=read(self.work/'p4-frames.json');decoded=read(self.work/'p4-decoded.json');javascript=read(self.work/'p4-js-decoded.json')
        self.check(source==decoded,'C++ SFVB decode is lossless for a Phase 4 trajectory')
        self.check(source==javascript,'Node.js reference decoder matches every Phase 4 field')
        canonical=lambda v:json.dumps(v,sort_keys=True,ensure_ascii=False,indent=1)
        self.check(canonical(source)==canonical(javascript),'cross-language canonical dumps are byte-identical for the Phase 4 stream')
        self.check(any(c['id']=='i2c0' for f in source['frames'] for c in f['components']) or any(c['id']=='oled0' for f in source['frames'] for c in f['components']),'binary stream carries i2c-sensor / spi-oled component states')
        self.report['sfvb']=dict(frames=len(source['frames']),batches=len(batches))
    def finish(self,status,**extra):
        self.report['status']=status;self.report.update(extra)
        self.report['check_count']=len(self.report['checks'])
        write(self.work/(self.mode+'-results.json'),self.report)
        for r in self.clients:
            try:r.close()
            except Exception:pass
        print(json.dumps({k:self.report[k] for k in ('status','check_count') if k in self.report}),flush=True)
def main():
    p=argparse.ArgumentParser();p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--mode',choices=['bus','protocols'],required=True);a=p.parse_args()
    acceptance=Acceptance(a.build_dir,a.mode)
    try:
        acceptance.native()
        if a.mode=='bus':acceptance.bus()
        else:acceptance.protocols()
        acceptance.finish('passed')
    except Exception as e:
        acceptance.finish('failed',error=str(e));print(str(e),file=sys.stderr);return 1
    return 0
if __name__=='__main__':raise SystemExit(main())
