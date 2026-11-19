"""Generate versioned P2 examples and teacher packages; private signing key stays local."""
import hashlib,json,shutil
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization
ROOT=Path(__file__).resolve().parents[2]
def read(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def write(p,j):p=Path(p);p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(j,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def profile(board,rules):return dict(schema='sigflow.fault-profile.v2',id='teacher-plan',revision=1,board_ref=board['id']+'@'+board['version'],tick_ps='1',rng=dict(algorithm='pcg32-v1',seed='9500123',streams='by_fault_id'),conflict_policy='reject_overlapping_mutations',faults=rules)
def rule(id,type,target,kind,hook,effect=None):return dict(id=id,enabled=True,visibility='teacher_only',hook=hook,target=dict(kind=kind,id=target),effect=dict(effect or {},type=type),trigger=dict(kind='permanent',start_tick='0'))
def main():
    board=read(ROOT/'registry/boards/tang-nano-9k.phase1.virtual.json');board['version']='2.0.0';board['electrical'].update(resolver_version='sigflow.state13.v1',state_encoding='sigflow.state13.v1');board['instruments']=[dict(id='probe-template',model='sigflow.probe@1',read_only=True),dict(id='meter-template',model='sigflow.digital-meter@1',voltage_model='digital_nominal_estimate_not_spice')];write(ROOT/'registry/boards/tang-nano-9k.phase2.virtual.json',board)
    folder=ROOT/'examples/virtual/phase2';folder.mkdir(parents=True,exist_ok=True)
    for name in ['phase1.sv','phase1.cst']:shutil.copyfile(ROOT/'examples/virtual/phase1'/name,folder/name.replace('phase1','phase2'))
    project=read(ROOT/'examples/virtual/phase1/virtual.project.json');project.update(source_files=['phase2.sv'],cst_path='phase2.cst',board_path='registry/boards/tang-nano-9k.phase2.virtual.json',phase2=True);project.pop('fault_profile_path',None);project['fault_profile']=profile(board,[]);write(folder/'virtual.project.json',project)
    # Frozen product table: FLT=0x02 is the identity, UND=0x00 remains undriven unknown.
    def combine(a,b):
        if a==b:return a
        if a==2:return b
        if b==2:return a
        sa,sb=a&28,b&28
        if sa==sb:return sa|34
        strong,weak=(a,b) if sa>sb else (b,a)
        return strong|(32 if (weak&28)>=12 and (strong&3)!=(weak&3) and (strong&28)!=20 else 0)
    table=[[combine(a,b) for b in range(32)] for a in range(32)];write(ROOT/'registry/logic/state13.v1.json',dict(schema='sigflow.state13.v1',identity=2,encoding=dict(UND=0,FLT=2,WUD=4,WLO=5,WHI=7,SUD=12,SLO=13,SHI=15,ILO=21,IHI=23,PLO=29,PHI=31,CON=32),strength_order=['F','W','S','I','P'],table=table,policy='Indices strip CON; CON on a weaker historical operand may be overridden; sticky_conflict separately retains the entire canonical sorted fold history. Ideal wins over weaker S/W; P wins over I and flags opposite drive. Equal-strength differing polarity/undefined is CON. No PUD product extension.',research_delta='Reverse study mixes FLT/UND indices and misstates associativity; product explicitly uses FLT=2 identity and does not assume associativity.'))
    key_file=ROOT/'.work/p2/teacher-key.pem';key_file.parent.mkdir(parents=True,exist_ok=True)
    if key_file.exists():key=serialization.load_pem_private_key(key_file.read_bytes(),None)
    else:
        key=Ed25519PrivateKey.generate();key_file.write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    pub=ROOT/'registry/teaching/course-signing-key.pub';pub.parent.mkdir(parents=True,exist_ok=True);pub.write_bytes(key.public_key().public_bytes(serialization.Encoding.PEM,serialization.PublicFormat.SubjectPublicKeyInfo))
    cases=[('course-01-open-wire','LED 异常：比较两个测量点','wire.led0','open',rule('secret-open-4a7','open','wire.led0','connection','topology.transform')),
           ('course-02-bridge-contention','相邻输出异常：寻找共同驱动','pad.10-pad.11','bridge',rule('secret-bridge-8b2','bridge','pad.10','node','topology.transform',dict(other_node='pad.11'))),
           ('course-03-button-debounce','一次操作产生多次计数','key_p3','button_bounce',None),
           ('course-04-uart-clock-fault','外设时序异常：恢复时钟与通信','clk27m','clock_stop',rule('secret-clock-9c3','clock_stop','clk27m','clock','clock.schedule',dict(hold_level=0,resume_phase='restart_half_period')))]
    for number,(id,title,location,type,fault) in enumerate(cases,1):
        path=ROOT/'examples/virtual/courses'/id;path.mkdir(parents=True,exist_ok=True)
        base=read(ROOT/'registry/boards/tang-nano-9k.virtual.json');base['version']='2.0.0';base['electrical'].update(resolver_version='sigflow.state13.v1',state_encoding='sigflow.state13.v1')
        config=read(ROOT/'examples/virtual/counter/virtual.project.json');config.update(phase2=True,phase1=False,board_path=str(path/'board.json'),source_files=['broken.sv'],project_root=str(path));config.pop('fault_profile_path',None)
        if number==4:
            base=read(ROOT/'registry/boards/tang-nano-9k.phase2.virtual.json');base['version']='2.0.0-teaching-1mhz';base['clocks'][0]['frequency_hz']['num']='1000000'
            for c in base['components']:
                if c['id']=='uart0':c['params']['baud']=100000
            config=read(ROOT/'examples/virtual/phase2/virtual.project.json');config.update(source_files=['broken.sv'],top_module='p2_uart_course',cst_path='counter.cst',board_path=str(path/'board.json'),clock=dict(config['clock'],frequency_hz='1000000'));config.pop('fault_profile',None);broken=(ROOT/'examples/virtual/phase2/uart_course.sv').read_text(encoding='utf-8')
        elif number==3:
            base['clocks'][0]['frequency_hz']['num']='1000';config['clock']['frequency_hz']='1000';config['top_module']='p1_debounce';broken=(ROOT/'tests/virtual/p1/debounce.sv').read_text(encoding='utf-8').replace('clean_count <= clean_count + 1\'b1','clean_count <= clean_count + 1\'b1')
            # Broken exercise: an undebounced edge drives both counters.
            broken=broken.replace('assign led_n = ~{clean_count, raw_count};','assign led_n = ~{raw_count, raw_count};')
            template=read(ROOT/'examples/virtual/phase1/contracts/fault-profile.json')['faults'][2];template.update(id='secret-bounce-2d1',enabled=True,visibility='teacher_only');fault=template
            repaired=(ROOT/'tests/virtual/p1/debounce.sv').read_text(encoding='utf-8');(path/'reference-repair.sv').write_text(repaired,encoding='utf-8')
        else:broken=(ROOT/'examples/virtual/counter/counter.sv').read_text(encoding='utf-8')
        (path/'broken.sv').write_text(broken,encoding='utf-8');shutil.copyfile(ROOT/('examples/virtual/phase2/phase2.cst' if number==4 else 'examples/virtual/counter/counter.cst'),path/'counter.cst');write(path/'board.json',base);write(path/'virtual.project.json',config);write(path/'faults.private.json',profile(base,[fault]))
        public=dict(schema='sigflow.course-task.v1',id='lesson-'+str(number),title=title,objective='采集可复核的测量证据，定位异常并提交修复',allowed_observations=['pad.10','led0.K','pad.11','led1.K','pad.3','key_p3.contact','pad.52','port:led_n[0]','port:led_n[3]'],public_tests=['reset_then_press','next_event_and_ticks'],hint_levels=['L1','L2','L3','L4'],mode='student_blind_test')
        rubric=dict(schema='sigflow.course-rubric.v1',cause=dict(location=location,type=type),hidden_seeds=['73191','88703','95077'],project='virtual.project.json',repair_kind='rtl_debounce' if number==3 else 'physical_service',allowed_repair=location,assertion='one_press_counts_once' if number==3 else 'clock_and_counter_restore',hints=['观察操作后输出是否符合一次操作一次变化。','在输入、Pad 与器件端分别放置只读探针，比较变化。','用已采集的两侧观测或时钟边沿确认异常发生的物理范围。','修复连接、驱动互斥或稳定采样原则后，执行三组隐藏激励验证。'])
        rubric['required_observations']=[['pad.10','led0.K'],['pad.10','pad.11'],['pad.3','port:led_n[3]'],['pad.52','port:led_n[0]']][number-1]
        if number==3:rubric['hidden_perturbation']=dict(algorithm='pcg32-v1',edge_jitter_ticks='50000000',derivation='sha256-seed-and-fault-id-independent-stream')
        if number==4:rubric['assertion']='uart_bad_frame_rejection_and_recovery'
        public['clock_fixture']=dict(frequency_hz=config['clock']['frequency_hz'],physical_board_oscillator_hz='27000000',teaching_override=number in [3,4])
        write(path/'task.public.json',public);write(path/'rubric.private.json',rubric)
        names=['task.public.json','faults.private.json','rubric.private.json','virtual.project.json','board.json','broken.sv','counter.cst'];manifest=dict(schema='sigflow.signed-course.v1',algorithm='Ed25519',files={n:hashlib.sha256((path/n).read_bytes()).hexdigest() for n in names});canonical=json.dumps(manifest,sort_keys=True,separators=(',',':')).encode();manifest['signature']=key.sign(canonical).hex();write(path/'manifest.private.json',manifest)
    print('P2 board, table, shadow project and four signed teacher packages generated.')
if __name__=='__main__':main()
