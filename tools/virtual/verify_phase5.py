"""Real Phase 5 verification: I2C multibyte burst read + cross-end SFVB web parity.

mode=i2c_burst   Real Verilator 3-byte burst read (Master ACK -> Master ACK ->
                 Master NAK -> STOP), MSB=1 registers, data/address NAK recovery,
                 and zero-regression check against frozen Phase 4 i2c-read digests.
mode=web_parity  Real Verilator trajectories across M0/P1/P2/P3/P4/P5 encoded
                 to SFVB binary streams, decoded via sfvb_reference_decoder.js
                 and projected via sfvb_web_projector.js, asserting 100% per-frame
                 workbench view parity + protocol resilience + 64-bit BigInt.
"""
import argparse,copy,hashlib,json,os,shutil,subprocess,sys,time,uuid
from pathlib import Path
from virtual_teaching_service import Runner,assemble,read,write,ROOT
sys.path.insert(0,str(Path(__file__).resolve().parent))
sys.stdout.reconfigure(encoding='utf-8',errors='replace')

FROZEN_P4_I2C_READ_CIRCUIT='2782e323ab39c779cff89ae0a06ac50884059d9f15741a347543fb3223611ddf'
FROZEN_P4_I2C_READ_CONTROL='8098665b5669a7f6bb16d30f6eb15f90a12f4fd921e92d53688ccd607ed8032b'
EXPECTED_OLED_SHA256='307a5bc25d1638e7d825a90ae9acc7ebf9b1db1f19ab0a47e0f94dfe93733180'

def rule(id,type,target,kind,hook,effect=None,trigger=None):
    body=dict(effect or {},type=type)
    return dict(id=id,enabled=True,visibility='teacher_only',target=dict(id=target,kind=kind),hook=hook,effect=body,trigger=trigger or dict(kind='permanent',start_tick='0'))

def bounce_edges():
    edges=[dict(dt='0',contact='closed')]
    for dt in ('2','5','9'):edges.append(dict(dt=dt,contact='released'))
    edges.append(dict(dt='14',contact='closed'))
    return edges

def crc8(data):
    crc=0
    for byte in data:
        crc^=byte
        for _ in range(8):crc=((crc<<1)^0x07)&0xFF if crc&0x80 else (crc<<1)&0xFF
    return crc

def collect_batches(r):
    signals=None;batches=[];after='0'
    while True:
        page=r.call('trace.commits',after_revision=after,limit=128)
        if signals is None:signals=page['signals']
        for batch in page['batches']:
            batches.append(dict(revision=batch['revision'],events=batch['events']))
        after=page['next_revision']
        if page['complete'] or not page['batches']:break
    return signals,batches

class Acceptance:
    def __init__(self,build,mode):
        self.build=Path(build).resolve();self.work=ROOT/'.work/p5'/mode;self.work.mkdir(parents=True,exist_ok=True)
        (self.work/'progress.log').write_text('',encoding='utf-8')
        self.report=dict(status='running',checks=[],commands=[],replays=[]);self.mode=mode;self.clients=[]

    def check(self,ok,text):
        with (self.work/'progress.log').open('a',encoding='utf-8') as f:f.write(str(bool(ok))+' '+text+'\n')
        if not ok:raise AssertionError(text)
        self.report['checks'].append(text)

    def run(self,args,label,timeout=900):
        env=dict(os.environ);env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+str(self.build)+';'+env.get('PATH','')
        started=time.monotonic()
        r=subprocess.run([str(x) for x in args],cwd=ROOT,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=timeout)
        (self.work/(label+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8')
        self.report['commands'].append(dict(argv=[str(x) for x in args],exit_code=r.returncode,seconds=round(time.monotonic()-started,3)))
        self.check(r.returncode==0,label+' exits 0: '+(r.stdout+r.stderr)[-400:])
        return r

    def client(self,result,cfg,label,rules):
        cfg=copy.deepcopy(cfg);cfg['fault_profile']['faults']=rules
        folder=self.work/(label+'-'+uuid.uuid4().hex);shutil.rmtree(folder,ignore_errors=True)
        r=Runner(result,cfg,folder);self.clients.append(r);return r

    def replay(self,r,label):
        f=r.call('stop');replay=r.call('replay.current',repetitions=3)
        self.check(replay['status']=='passed' and len(replay['attempts'])==3 and all(a['circuit_digest']==f['digests']['circuit'] and a['control_digest']==f['digests']['control'] for a in replay['attempts']),label+' three from-zero real RTL replays preserve both digests')
        self.report['replays'].append(dict(label=label,**replay));r.close();self.clients.remove(r)
        return f

    def project(self,rel):
        path=ROOT/'examples/virtual'/rel/'virtual.project.json'
        p=read(path);p['project_root']=path.parent.as_posix();return p

    def i2c_burst(self):
        from jsonschema import Draft202012Validator
        profile_schema=read(ROOT/'registry/schemas/fault-profile.v2.schema.json')
        p5_proj=self.project('phase5/i2c-burst')
        Draft202012Validator(profile_schema).validate(p5_proj['fault_profile'])
        self.check(True,'phase5/i2c-burst fault_profile validates against fault-profile.v2 schema')

        # 1. Normal 3-byte burst read: [0x1A, 0x2B, 0x5C] -> XOR = 0x6D (low 5 = 0x0D -> ~led_n = 5'b10010)
        res,cfg=assemble(p5_proj,self.work/'burst-build',self.build)
        r=self.client(res,cfg,'burst-normal',[])
        r.call('set',target='reset_p4',state=2);r.call('step')
        frame=r.call('step',mode='ticks',ticks='45000000')
        sensor=frame['board']['peripherals']['i2c0']
        self.check(sensor['transactions']==2 and sensor['ack_count']==3 and sensor['nak_count']==0,'normal 3-byte burst completes write-pointer + burst-read in 2 transactions with 3 ACKs')
        self.check(sensor['reg_pointer']==2 and sensor['last_read_byte']=='5c' and sensor['bus_state']=='idle','sensor reg_pointer advances to 2 and last_read_byte ends on 0x5c')
        self.check(sensor['registers']==['1a','2b','5c'],'sensor register file preserved')
        outs=frame['outputs']
        self.check(outs['led_n[0]'] is False and outs['led_n[1]'] is True and outs['led_n[2]'] is False and outs['led_n[3]'] is False and outs['led_n[4]'] is True,'DUT burst parity 0x1A^0x2B^0x5C = 0x6D matches led_n = 5\'b10010')
        self.replay(r,'burst-normal')

        # 2. MSB=1 register variant [0xA5, 0xC3, 0x96] -> XOR = 0xF0 (low 5 = 5'b10000 -> ~led_n = 5'b01111)
        board_msb=read(ROOT/'registry/boards/tang-nano-9k.phase4.virtual.json')
        for comp in board_msb['components']:
            if comp['id']=='i2c0':comp['params']['registers']=[0xA5,0xC3,0x96]
        write(self.work/'msb-board.json',board_msb)
        p_msb=copy.deepcopy(p5_proj);p_msb['board_path']=(self.work/'msb-board.json').as_posix()
        res_msb,cfg_msb=assemble(p_msb,self.work/'burst-msb-build',self.build)
        r=self.client(res_msb,cfg_msb,'burst-msb',[])
        r.call('set',target='reset_p4',state=2);r.call('step')
        frame=r.call('step',mode='ticks',ticks='45000000')
        sensor=frame['board']['peripherals']['i2c0']
        self.check(sensor['reg_pointer']==2 and sensor['last_read_byte']=='96' and sensor['registers']==['a5','c3','96'] and sensor['ack_count']==3,'MSB=1 registers [0xA5,0xC3,0x96] burst cleanly without bit slip')
        outs=frame['outputs']
        self.check(outs['led_n[0]'] is True and outs['led_n[1]'] is True and outs['led_n[2]'] is True and outs['led_n[3]'] is True and outs['led_n[4]'] is False,'DUT MSB=1 burst parity 0xA5^0xC3^0x96 = 0xF0 matches led_n = 5\'b01111')
        self.replay(r,'burst-msb')

        # 3. Data-phase NAK in transaction 1 (write pointer byte) -> retry & recover
        data_nak=[rule('p5-data-nak','i2c_nak','i2c0','component','peripheral.event',dict(phase='data',nth_transaction=1))]
        Draft202012Validator(profile_schema).validate(dict(p5_proj['fault_profile'],faults=data_nak))
        r=self.client(res,cfg,'burst-data-nak',data_nak)
        r.call('set',target='reset_p4',state=2);r.call('step')
        frame=r.call('step',mode='ticks',ticks='60000000')
        sensor=frame['board']['peripherals']['i2c0']
        self.check(sensor['nak_count']==1 and sensor['transactions']==3 and sensor['ack_count']==4 and sensor['reg_pointer']==2 and sensor['last_read_byte']=='5c' and sensor['bus_state']=='idle','data-phase NAK in tx 1 retries and finishes 3-byte burst cleanly')
        outs=frame['outputs']
        self.check(outs['led_n[0]'] is False and outs['led_n[1]'] is True and outs['led_n[2]'] is False and outs['led_n[3]'] is False and outs['led_n[4]'] is True,'recovered DUT parity still matches 5\'b10010 after data-phase NAK')
        ledger=[e for record in r.call('journal')['records'] if record.get('kind')=='commit'
            for e in record.get('p1',{}).get('faults',{}).get('events',[]) if e.get('type')=='i2c.nak_injected']
        self.check(len(ledger)==1 and ledger[0]['phase']=='data' and ledger[0]['transaction']==1,'journal records i2c.nak_injected phase=data transaction=1')
        self.replay(r,'burst-data-nak')

        # 4. Address-phase NAK in transaction 2 (burst read header) -> retry & recover
        addr_nak=[rule('p5-addr2-nak','i2c_nak','i2c0','component','peripheral.event',dict(phase='address',nth_transaction=2))]
        Draft202012Validator(profile_schema).validate(dict(p5_proj['fault_profile'],faults=addr_nak))
        r=self.client(res,cfg,'burst-addr2-nak',addr_nak)
        r.call('set',target='reset_p4',state=2);r.call('step')
        frame=r.call('step',mode='ticks',ticks='60000000')
        sensor=frame['board']['peripherals']['i2c0']
        self.check(sensor['nak_count']==1 and sensor['transactions']==4 and sensor['ack_count']==5 and sensor['reg_pointer']==2 and sensor['last_read_byte']=='5c' and sensor['bus_state']=='idle','address-phase NAK in tx 2 retries full sequence and finishes burst cleanly')
        outs=frame['outputs']
        self.check(outs['led_n[0]'] is False and outs['led_n[1]'] is True and outs['led_n[2]'] is False and outs['led_n[3]'] is False and outs['led_n[4]'] is True,'recovered DUT parity matches 5\'b10010 after tx2 address NAK')
        ledger=[e for record in r.call('journal')['records'] if record.get('kind')=='commit'
            for e in record.get('p1',{}).get('faults',{}).get('events',[]) if e.get('type')=='i2c.nak_injected']
        self.check(len(ledger)==1 and ledger[0]['phase']=='address' and ledger[0]['transaction']==2,'journal records i2c.nak_injected phase=address transaction=2')
        self.replay(r,'burst-addr2-nak')

        # 5. Phase 4 zero-regression check: exact i2c-read digests match frozen P4 evidence byte-for-byte
        p4_proj=self.project('phase4/i2c-temp')
        res4,cfg4=assemble(p4_proj,self.work/'p4-regression-build',self.build)
        r4=self.client(res4,cfg4,'p4-i2c-read',[])
        r4.call('set',target='reset_p4',state=2);r4.call('step')
        f4=r4.call('step',mode='ticks',ticks='20000000')
        self.check(f4['board']['peripherals']['i2c0']['last_read_byte']=='1a' and f4['board']['peripherals']['i2c0']['ack_count']==1,'Phase 4 single-byte read stays exact')
        stopped4=self.replay(r4,'p4-i2c-read-zero-regression')
        self.check(stopped4['digests']['circuit']==FROZEN_P4_I2C_READ_CIRCUIT and stopped4['digests']['control']==FROZEN_P4_I2C_READ_CONTROL,'Phase 4 i2c-read circuit_digest and control_digest have ZERO regression against frozen P4 evidence')
        self.report['p4_zero_regression']=stopped4['digests']

    def _encode_and_verify_stream(self,name,signals,runs,raw_frames_list):
        traj=dict(run_handle='5100',generation='1',signals=signals,runs=runs)
        traj_path=self.work/(name+'-trajectory.json')
        sfvb_path=self.work/(name+'.sfvb')
        cpp_src_path=self.work/(name+'-cpp-src.json')
        cpp_dec_path=self.work/(name+'-cpp-dec.json')
        js_dec_path=self.work/(name+'-js-dec.json')
        web_proj_path=self.work/(name+'-web-proj.json')
        write(traj_path,traj)
        self.run([self.build/'tests/virtual/p3/p3_native_tests.exe','encode',traj_path,sfvb_path,cpp_src_path,cpp_dec_path],'encode-'+name)
        self.run(['node',ROOT/'tools/virtual/sfvb_reference_decoder.js',sfvb_path,js_dec_path],'js-ref-'+name)
        self.run(['node',ROOT/'tools/virtual/sfvb_web_projector.js',sfvb_path,web_proj_path],'js-proj-'+name)

        src=read(cpp_src_path);js=read(js_dec_path);proj=read(web_proj_path)
        self.check(src==js,'['+name+'] sfvb_reference_decoder.js matches C++ frame-for-frame')
        self.check(proj['status']=='passed' and proj['count']==len(raw_frames_list),'['+name+'] sfvb_web_projector.js projects all '+str(len(raw_frames_list))+' frames')

        for idx,(raw,view) in enumerate(zip(raw_frames_list,proj['views'])):
            self.check(view['run_id']==raw['run_id'],'['+name+f'#{idx}] run_id parity')
            self.check(view['tick']==raw['position']['tick'] and view['delta']==raw['position']['delta'],'['+name+f'#{idx}] 64-bit tick/delta parity')
            self.check(view['state']==raw['state'] and view['reason']==raw.get('reason',''),'['+name+f'#{idx}] state/reason parity')
            self.check(view['trace']['revision']==raw['trace']['revision']
                and view['trace']['committed_through_tick']==raw['trace']['committed_through_tick']
                and view['trace']['committed_event_seq']==raw['trace']['tail_event_seq'],'['+name+f'#{idx}] trace cursor parity')

            raw_nodes=raw.get('board',{}).get('nodes',{})
            self.check(set(view['nodes'].keys())==set(raw_nodes.keys()),'['+name+f'#{idx}] node set parity')
            for node_name,node_val in raw_nodes.items():
                vn=view['nodes'][node_name]
                self.check(vn['state_code']==node_val['state_code'] and vn['logic']==node_val['logic']
                    and vn['valid']==node_val['valid'] and vn['contention']==node_val['contention'],'['+name+f'#{idx}] node {node_name} 13-state parity')

            # LED honest mounting & integration parity
            raw_leds=raw.get('board',{}).get('leds',{})
            pads=[10,11,13,14,15,16]
            for i in range(6):
                vl=view['leds'][i]
                led_k=f'led{i}.K'
                if not raw_nodes or led_k in raw_nodes:
                    self.check(vl['mounted'] is True,'['+name+f'#{idx}] led{i} mounted')
                    if f'led{i}' in raw_leds and 'integration' in raw_leds[f'led{i}']:
                        self.check(vl['duty_ppm']==raw_leds[f'led{i}']['integration']['duty_ppm'],'['+name+f'#{idx}] led{i} duty_ppm parity')
                else:
                    pad_name=f'pad.{pads[i]}'
                    self.check(vl['mounted'] is False,'['+name+f'#{idx}] led{i} unmounted honest state')
                    if pad_name in raw_nodes:
                        self.check(vl['multiplexed'] is True and vl['label']==f'P{pads[i]} → 外接' and vl['state_code']==raw_nodes[pad_name]['state_code'],'['+name+f'#{idx}] multiplexed pad.{pads[i]} honest label & state')

            # Key honest mounting parity
            for k_idx,k_pad in enumerate((3,4)):
                vk=view['keys'][k_idx]
                k_node=f'key_p{k_pad}.contact'
                if not raw_nodes or k_node in raw_nodes:
                    self.check(vk['mounted'] is True,'['+name+f'#{idx}] key_p{k_pad} mounted')
                else:
                    self.check(vk['mounted'] is False and vk['state_name']=='未挂载','['+name+f'#{idx}] unmounted key_p{k_pad} honest label')

            # Component parity
            raw_periph=raw.get('board',{}).get('peripherals',{})
            for cid,cval in raw_periph.items():
                model=cval.get('model','')
                if model=='sigflow.matrix-keypad@1':
                    self.check(view['keypads'][cid]==cval,'['+name+f'#{idx}] keypad {cid} parity')
                elif model=='sigflow.i2c-sensor@1':
                    for field in ('address_7bit','registers','reg_pointer','ack_count','nak_count','last_read_byte','transactions','bus_state','read_mode'):
                        self.check(view['i2cs'][cid][field]==cval[field],'['+name+f'#{idx}] i2c {cid}.{field} parity')
                elif model=='sigflow.spi-oled@1':
                    vo=view['oleds'][cid]
                    for field in ('width','height','cpol','cpha','display_on','inverted','page','column','write_count','lit_pixels','frame_sha256'):
                        self.check(vo[field]==cval[field],'['+name+f'#{idx}] oled {cid}.{field} parity')
                    pix_sum=sum(sum(row) for row in vo['pixels'])
                    self.check(vo['raw_lit_pixels']==cval['lit_pixels'] and (pix_sum==cval['lit_pixels'] if not cval['inverted'] else True),'['+name+f'#{idx}] oled decoded 2D pixel bitmap parity')
                elif model=='sigflow.seven-segment-mux@1':
                    self.check(view['seven_seg'][cid]==cval,'['+name+f'#{idx}] seven_seg {cid} parity')
                elif model=='sigflow.dip-switch@1':
                    self.check(view['dips'][cid]==cval,'['+name+f'#{idx}] dip {cid} parity')

        return sfvb_path,proj['views']

    def web_parity(self):
        # Stream 1: p12-mixed (Phase 2 PWM/7-seg/DIP/UART/13-state bridge + StudentProjection blind second run)
        p2=self.project('phase2')
        p2['fault_profile']['rng']['seed']='3100002'
        p2_rules=[
            rule('hidden-stuck','stuck_at','fpga0.pad.10','fpga_pad_driver','driver.transform',dict(mode='replace_source',value=1)),
            rule('hidden-open','open','wire.led1','connection','topology.transform'),
            rule('hidden-bounce','button_bounce','key_p3','component','peripheral.event',dict(drive='open_drain',duration_ticks='14',relative_edges=bounce_edges()),
                dict(kind='intermittent',event=dict(type='input.intent',target='key_p3',action='press',occurrence=1),probability_ppm=1000000,active_ticks='200',cooldown_ticks='500',max_activations=1)),
            rule('hidden-clock','clock_stop','clk27m','clock','clock.schedule',dict(hold_level=0,resume_phase='restart_half_period'),dict(kind='transient',start_tick='3000000',end_tick='6000000')),
            rule('hidden-bridge','bridge','pad.10','node','topology.transform',dict(other_node='pad.11'))]
        res2,cfg2=assemble(p2,self.work/'p12-build',self.build)
        r=self.client(res2,cfg2,'p12-teacher',p2_rules)
        t_frames=[r.call('get'),r.call('set',target='reset_p4',state=2),r.call('set',target='key_p3',state=13),r.call('step'),
            r.call('uart.send',target='uart0',bytes=[65,66]),r.call('dip.set',target='dip0',value=2),
            r.call('step',mode='ticks',ticks='4000000'),r.call('step',mode='ticks',ticks='8000000'),
            r.call('set',target='key_p3',state=2),r.call('step')]
        sigs2,t_batches=collect_batches(r)
        r.call('stop');r.close();self.clients.remove(r)

        blind_cfg=copy.deepcopy(cfg2);blind_cfg['teaching']=dict(mode='student_blind_test',run_token='opaque-p5-blind-token-0123456789abcdef')
        b=self.client(res2,blind_cfg,'p12-blind',p2_rules)
        b_frames=[b.call('get'),b.call('step'),b.call('set',target='key_p3',state=13),b.call('step',mode='ticks',ticks='4000000'),b.call('step')]
        _,b_batches=collect_batches(b)
        b.call('stop');b.close();self.clients.remove(b)

        p12_sfvb,p12_views=self._encode_and_verify_stream('p12-mixed',sigs2,[dict(frames=t_frames,trace_batches=t_batches),dict(frames=b_frames,trace_batches=b_batches)],t_frames+b_frames)
        self.check(any(v['uarts'].get('uart0',{}).get('transmitted') for v in p12_views[:len(t_frames)]),'p12-mixed web view retains bit-level UART tx history')

        # Stream 2: p3-keypad (ghost key (1,1) emergence)
        p3=self.project('phase3/keypad-ghost')
        res3,cfg3=assemble(p3,self.work/'p3-build',self.build)
        r3=self.client(res3,cfg3,'p3-keypad',[])
        kp_frames=[r3.call('get'),r3.call('set',target='reset_p4',state=2),r3.call('keypad.set',target='keypad0',keys=[[0,0],[0,1],[1,0]]),
            r3.call('step'),r3.call('set',target='keypad_row1',state=13),r3.call('step')]
        sigs3,kp_batches=collect_batches(r3)
        r3.call('stop');r3.close();self.clients.remove(r3)
        _,kp_views=self._encode_and_verify_stream('p3-keypad',sigs3,[dict(frames=kp_frames,trace_batches=kp_batches)],kp_frames)
        self.check(kp_views[-1]['keypads']['keypad0']['ghosts']==[['1','1']],'p3-keypad web view reproduces ghost key (1,1)')

        # Stream 3: p4-oled (OLED 64x32 + keypad + honest P16 & KEY P3)
        p4o=self.project('phase4/spi-oled')
        res4o,cfg4o=assemble(p4o,self.work/'p4o-build',self.build)
        r4o=self.client(res4o,cfg4o,'p4-oled',[])
        o_frames=[r4o.call('get'),r4o.call('set',target='reset_p4',state=2),r4o.call('step'),
            r4o.call('step',mode='ticks',ticks='17000000'),r4o.call('step',mode='ticks',ticks='17000000'),
            r4o.call('keypad.set',target='keypad0',keys=[[1,2],[3,0]]),r4o.call('step',mode='ticks',ticks='1000000')]
        sigs4o,o_batches=collect_batches(r4o)
        r4o.call('stop');r4o.close();self.clients.remove(r4o)
        p4o_sfvb,o_views=self._encode_and_verify_stream('p4-oled',sigs4o,[dict(frames=o_frames,trace_batches=o_batches)],o_frames)
        self.check(o_views[-1]['oleds']['oled0']['frame_sha256']==EXPECTED_OLED_SHA256 and o_views[-1]['oleds']['oled0']['lit_pixels']==28,'p4-oled web view reproduces exact OLED 28-pixel bitmap & digest')
        self.check(o_views[-1]['leds'][5]['multiplexed'] is True and o_views[-1]['leds'][5]['label']=='P16 → 外接' and o_views[-1]['keys'][0]['state_name']=='未挂载','p4-oled web view matches Desktop honest P16 & KEY P3 rendering')

        # Stream 4: p4-crc-cdc (crc_tamper + cdc_sampling_approx)
        p4c=self.project('phase4/crc-cdc')
        p4c['fault_profile']['rng']['seed']='4100101'
        c_rules=[
            rule('p5-crc','crc_tamper','uart0','component','protocol.transform',dict(protocol_id='sigflow.uart-packet-crc8.v1',xor_mask=255,frame_index=0)),
            rule('p5-cdc','cdc_sampling_approx','key_p3','input','input.sampling',dict(window_ps='2000',max_delay_cycles=2,policy='seeded_enum'))]
        res4c,cfg4c=assemble(p4c,self.work/'p4c-build',self.build)
        r4c=self.client(res4c,cfg4c,'p4-crc-cdc',c_rules)
        payload=[0x11,0x22];packet=[0xAA,len(payload)]+payload;packet.append(crc8(packet))
        c_frames=[r4c.call('get'),r4c.call('set',target='reset_p4',state=2),r4c.call('step'),
            r4c.call('uart.send',target='uart0',bytes=packet),r4c.call('step',mode='ticks',ticks='16000000')]
        before=c_frames[-1];edge=int(before['p1']['next_clock_tick']);target=edge-1000
        c_frames.append(r4c.call('step',mode='ticks',ticks=str(target-int(before['position']['tick']))))
        c_frames.append(r4c.call('set',target='key_p3',state=13))
        c_frames.append(r4c.call('step'))
        sigs4c,c_batches=collect_batches(r4c)
        c_journal=r4c.call('journal')['records']
        r4c.call('stop');r4c.close();self.clients.remove(r4c)
        _,c_views=self._encode_and_verify_stream('p4-crc-cdc',sigs4c,[dict(frames=c_frames,trace_batches=c_batches)],c_frames)
        self.check(c_views[4]['leds'][1]['lit'] is False and c_views[4]['leds'][2]['lit'] is True and c_views[4]['leds'][4]['lit'] is True,'p4-crc-cdc web view reproduces CRC8 tamper rejection (bad_count lit, good_count off, legal framing) on LED bank')
        self.check(any(e.get('type')=='cdc.sampling_approx' for rec in c_journal if rec.get('kind')=='commit' for e in rec.get('p1',{}).get('faults',{}).get('events',[])),'p4-crc-cdc trajectory exercises real cdc_sampling_approx')

        # Stream 5: p5-i2c-burst with data-phase i2c_nak, capturing exact i2c.nak_injected tick into SFVB RT_EVENTS
        p5i=self.project('phase5/i2c-burst')
        nak_rule=[rule('p5-nak-web','i2c_nak','i2c0','component','peripheral.event',dict(phase='data',nth_transaction=1))]
        res5,cfg5=assemble(p5i,self.work/'p5i-build',self.build)
        probe_r=self.client(res5,cfg5,'p5-i2c-probe',nak_rule)
        probe_r.call('set',target='reset_p4',state=2);probe_r.call('step')
        probe_r.call('step',mode='ticks',ticks='60000000')
        nak_events=[e for rec in probe_r.call('journal')['records'] if rec.get('kind')=='commit'
            for e in rec.get('p1',{}).get('faults',{}).get('events',[]) if e.get('type')=='i2c.nak_injected']
        probe_r.call('stop');probe_r.close();self.clients.remove(probe_r)
        self.check(bool(nak_events),'located exact tick of i2c.nak_injected')
        nak_tick=int(nak_events[0]['tick'])

        r5=self.client(res5,cfg5,'p5-i2c-web',nak_rule)
        i_frames=[r5.call('get'),r5.call('set',target='reset_p4',state=2),r5.call('step')]
        cur_tick=int(i_frames[-1]['position']['tick'])
        i_frames.append(r5.call('step',mode='ticks',ticks=str(nak_tick-cur_tick)))
        i_frames.append(r5.call('step',mode='ticks',ticks=str(60000000-(nak_tick-cur_tick))))
        sigs5,i_batches=collect_batches(r5)
        r5.call('stop');r5.close();self.clients.remove(r5)
        p5i_sfvb,i_views=self._encode_and_verify_stream('p5-i2c-burst',sigs5,[dict(frames=i_frames,trace_batches=i_batches)],i_frames)
        self.check(any(ev['type']=='i2c.nak_injected' and ev['detail']['phase']=='data' for v in i_views for ev in v['events']),'p5-i2c-burst SFVB RT_EVENTS delivers i2c.nak_injected to web view')
        self.check(i_views[-1]['i2cs']['i2c0']['last_read_byte']=='5c' and i_views[-1]['i2cs']['i2c0']['nak_count']==1 and i_views[-1]['i2cs']['i2c0']['ack_count']==4,'p5-i2c-burst web view matches recovered 3-byte I2C burst state')

        # Run Node self-test: gap/resync, baseline loss, stale generation, ACK dedup, 64-bit BigInt, blind leak & CSS token checks
        selftest_out=self.work/'web-projector-selftest.json'
        self.run(['node',ROOT/'tools/virtual/sfvb_web_projector.js','--self-test',p5i_sfvb,p12_sfvb,ROOT/'GUI/virtual-sfvb-player.html',selftest_out],'web-selftest')
        selftest=read(selftest_out)
        self.check(selftest['status']=='passed' and selftest['checks']>=12,'sfvb_web_projector.js --self-test passed '+str(selftest['checks'])+' protocol/BigInt/security checks')
        self.report['streams']=dict(p12=len(p12_views),p3=len(kp_views),p4_oled=len(o_views),p4_crc_cdc=len(c_views),p5_i2c_burst=len(i_views),selftest_checks=selftest['checks'])

    def finish(self,status,**extra):
        self.report['status']=status;self.report.update(extra)
        self.report['check_count']=len(self.report['checks'])
        write(self.work/(self.mode+'-results.json'),self.report)
        for r in self.clients:
            try:r.close()
            except Exception:pass
        print(json.dumps({k:self.report[k] for k in ('status','check_count') if k in self.report}),flush=True)

def main():
    p=argparse.ArgumentParser();p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--mode',choices=['i2c_burst','web_parity'],required=True);a=p.parse_args()
    acceptance=Acceptance(a.build_dir,a.mode)
    try:
        if a.mode=='i2c_burst':acceptance.i2c_burst()
        else:acceptance.web_parity()
        acceptance.finish('passed')
    except Exception as e:
        acceptance.finish('failed',error=str(e));print(str(e),file=sys.stderr);return 1
    return 0

if __name__=='__main__':raise SystemExit(main())
