"""Stable desktop + 41 CTests; append P2 archives without rewriting M0/P1."""
import gzip,hashlib,json,os,shutil,subprocess,sys,time,xml.etree.ElementTree as ET
from datetime import datetime
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'.work/p2/final';OUT=ROOT/'docs/virtual/evidence';P2=OUT/'P2'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def read(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def write(p,j):Path(p).write_text(json.dumps(j,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def sources():
    paths=set()
    for folder in ['core/src','include','main','tests','tools/virtual','registry','examples/virtual','rtl/debug','agent/src','GUI','InnerPlugin/eda-sim-verilator','InnerPlugin/eda-cst-gowin-cst']:
        paths.update(p for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in {'.cpp','.h','.hpp','.inc','.cmake','.txt','.py','.sv','.v','.cst','.json','.pub','.html','.md'} and not {'__pycache__','bin','CMakeFiles'}&set(p.parts))
    paths.update(ROOT/p for p in ['CMakeLists.txt','core/CMakeLists.txt','tests/CMakeLists.txt','.gitattributes','.gitignore'])
    return {p.relative_to(ROOT).as_posix():sha(p) for p in sorted(paths)}
def historical():
    reference=ROOT/'.work/p2/historical-sha256.json'
    if not reference.exists():reference=P2/'historical-sha256.json'
    frozen=read(reference)
    if len(frozen)!=88:raise RuntimeError('Expected M0 60 + P1 27 + P1 summary = 88 protected files')
    return frozen
def main():
    sys.stdout.reconfigure(encoding='utf-8',errors='replace');WORK.mkdir(parents=True,exist_ok=True);P2.mkdir(parents=True,exist_ok=True)
    started=time.time();before=sources();protected=historical();original=read(OUT/'P1-acceptance-summary.json');old={t['name'] for t in original['tests']}
    additions={'p2_state13_resolver_and_extended_faults','p2_virtual_probe_and_tracebridge','p2_blind_course_gateway_and_grading','p2_gui_teaching_closed_loop_acceptance'}
    if len(old)!=37:raise RuntimeError('Expected 37 original CTests')
    unchanged=lambda:all((ROOT/p).is_file() and sha(ROOT/p)==digest for p,digest in protected.items())
    if not unchanged():raise RuntimeError('Historical evidence changed before validation')
    env=dict(os.environ);env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+str(ROOT/'build-m0')+';'+env.get('PATH','');temp=WORK/'temp'/str(time.time_ns());temp.mkdir(parents=True,exist_ok=True);env['TMP']=env['TEMP']=str(temp)
    cmake=ROOT/'.work/cmake-python/cmake/data/bin/cmake.exe';ctest=cmake.with_name('ctest.exe');build=ROOT/'build-m0';xml=WORK/'ctest.xml';xml.unlink(missing_ok=True);commands=[]
    for name,args in [('build',[cmake,'--build',build,'--parallel','4']),('ctest',[ctest,'--test-dir',build,'--output-on-failure','-j','1','--output-junit',xml])]:
        print('Running P2 final '+name,flush=True);begin=time.monotonic()
        with (WORK/(name+'.log')).open('w',encoding='utf-8') as stream:r=subprocess.run([str(a) for a in args],cwd=ROOT,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=3600)
        commands.append(dict(argv=[str(a) for a in args],exit_code=r.returncode,seconds=round(time.monotonic()-begin,3)))
        if r.returncode:print((WORK/(name+'.log')).read_text(encoding='utf-8',errors='replace')[-12000:]);break
    tests=[]
    if xml.exists():
        for t in ET.parse(xml).getroot().iter('testcase'):tests.append(dict(name=t.attrib['name'],passed=t.find('failure') is None and t.find('skipped') is None and t.attrib.get('status')!='notrun',seconds=float(t.attrib.get('time',0))))
    reports={name:read(ROOT/'.work/p2'/path) if (ROOT/'.work/p2'/path).exists() else {} for name,path in [('faults','faults-results.json'),('probes','probes-results.json'),('teaching','teaching-results.json'),('gui','gui/gui-results.json')]}
    native=read(ROOT/'.work/p2/faults/native.json') if (ROOT/'.work/p2/faults/native.json').exists() else {}
    stable=before==sources();preserved=unchanged();byname={t['name'] for t in tests}
    good=len(commands)==2 and all(c['exit_code']==0 for c in commands) and len(tests)==41 and byname==old|additions and all(t['passed'] for t in tests) and stable and preserved and native.get('status')=='passed' and all(r.get('status')=='passed' for r in reports.values())
    artifacts={}
    def copy(source,name=None,compress=False):
        source=Path(source)
        if not source.is_file():return
        target=P2/(name or source.name);target.parent.mkdir(parents=True,exist_ok=True)
        if compress:target.write_bytes(gzip.compress(source.read_bytes(),mtime=0))
        else:shutil.copyfile(source,target)
        artifacts[target.relative_to(P2).as_posix()]=dict(sha256=sha(target),bytes=target.stat().st_size,**(dict(uncompressed_sha256=sha(source),raw_bytes=source.stat().st_size) if compress else {}))
    for name in ['build.log','ctest.log','ctest.xml']:copy(WORK/name)
    for name,path in [('faults-results.json','faults-results.json'),('probes-results.json','probes-results.json'),('teaching-results.json','teaching-results.json'),('gui-results.json','gui/gui-results.json')]:copy(ROOT/'.work/p2'/path,name)
    copy(ROOT/'.work/p2/faults/native.json','state13-native.json')
    for mode in ['faults','probes','teaching','gui']:copy(ROOT/'.work/p2'/mode/'progress.log',mode+'-progress.log')
    for p in (ROOT/'.work/p2/gui').glob('*.png'):copy(p,'gui/'+p.name)
    for name in ['comparison.json','baseline.json','actual.json','student-controller-frame.json','gui.log']:copy(ROOT/'.work/p2/gui'/name,'gui/'+name)
    for p in (ROOT/'.work/p2/teaching').glob('*-student-dtos.json'):copy(p,'student-dtos/'+p.name)
    for p in (ROOT/'.work/p2/teaching').rglob('grading-*.private.json'):
        if p.stat().st_mtime>=started:copy(p,'teacher-grading/'+p.parent.name+'/'+p.name)
    # Archive only journals produced by this actual CTest run, one per fault.
    for p in (ROOT/'.work/p2/faults').glob('*/private.journal.jsonl'):
        if p.stat().st_mtime>=started:copy(p,'journals/'+p.parent.name+'.jsonl.gz',True)
    for mode in ['probes','gui']:
        rtl=reports[mode].get('rtl',{})
        for key in ['raw','vcd']:
            if rtl.get(key):copy(rtl[key],'rtl-'+mode+'/capture.'+key)
        if rtl.get('raw'):copy(Path(rtl['raw']).parent/'capture.extended.json','rtl-'+mode+'/capture.extended.json')
    for name in ['baseline-capture.json','fault-capture.json','rtl-reference.json','rtl-samples.json','rtl-result.json']:copy(ROOT/'.work/p2/probes'/name,'probes/'+name)
    for folder in ['comparison','rtl-comparison']:
        for p in (ROOT/'.work/p2/probes'/folder).glob('*'):
            if p.is_file():copy(p,'probes/'+folder+'/'+p.name)
    write(P2/'historical-sha256.json',protected);artifacts['historical-sha256.json']=dict(sha256=sha(P2/'historical-sha256.json'),bytes=(P2/'historical-sha256.json').stat().st_size)
    limits=['Digital nominal voltage/state models; no SPICE current or analog metastability','Canonical driver order; STATE fold is not assumed associative','Mode A post-eval ring capture and Mode B pre-eval finite linear RTL capture have different explicit phases; cross-phase comparison is inconclusive','RTL transport is dedicated wrapper UART; not board Pad17/18 transport validation; pretrigger=0','Course03 1kHz and Course04 1MHz/100kbaud are explicit teaching fixtures; normal P2 board remains 27MHz','Teacher course and private journal directories must be deployed on trusted host; local blindness does not protect against machine owner','Interactive Windows GUI screenshots cover Light/Dark; physical multi-monitor switching not newly tested','Web deferred']
    result=dict(schema='sigflow.phase2-acceptance.v1',date=datetime.now().astimezone().isoformat(),status='passed' if good else 'failed',Phase2_complete=bool(good),base_commit='415b5a2',branch=subprocess.check_output(['git','branch','--show-current'],cwd=ROOT,text=True).strip(),test_total=len(tests),test_passed=sum(t['passed'] for t in tests),original_tests=37,new_tests=4,tests=tests,commands=commands,source_unchanged_during_validation=stable,source_sha256=before,historical_archive_unchanged=preserved,historical_count=len(protected),historical_sha256=protected,state13_native=native,faults=reports['faults'],probes=reports['probes'],teaching=reports['teaching'],gui=reports['gui'],desktop_binary=dict(path=str(build/'sigflow.exe'),sha256=sha(build/'sigflow.exe')),artifacts=artifacts,scope_limits=limits)
    write(OUT/'P2-acceptance-summary.json',result)
    lines=['# Phase 2 调试探针与教学判题闭环实测报告','',f'验收时间：{result["date"]}；基线 `415b5a2`；分支 `{result["branch"]}`。','',f'最终状态：**{result["status"]}**，`Phase2_complete={str(good).lower()}`。','', '| 验收 | 实测结果 |','|---|---|',f'| 全部 CTest | {result["test_passed"]}/{len(tests)}；原 37 项 + 新 4 项 |',f'| 冻结 STATE 表与原生核心 | {native.get("checks",0)} 项检查；包含 1024 格逐格对照 |',f'| 扩展故障/重放 | {reports["faults"].get("check_count",0)} 项真实工具断言 |',f'| Probe/双模式 TraceBridge | {reports["probes"].get("check_count",0)} 项真实工具断言；实际 RTL COBS+CRC16 回包 |',f'| 四课程/网关/判题 | {reports["teaching"].get("check_count",0)} 项；错误权限、错定位、未修复 RTL 拒绝，正确修复三组隐藏激励 |',f'| GUI | {reports["gui"].get("check_count",0)} 项真实 wxApp 断言；原生 Light/Dark 截图 |',f'| 历史归档 | {len(protected)} 份 SHA-256 保持：{preserved}；60 M0 + 27 P1 artifact + P1 summary |',f'| 参与验收的源码 | 构建前后逐文件哈希一致：{stable} |','','## 实现与证据','','| 模块 | 生产入口 / 证据 |','|---|---|','| STATE | State13 / BoardTopology；registry/logic/state13.v1.json；P2/state13-native.json |','| 拓扑/协议故障 | Phase2Faults / Phase1Session / Phase1Peripherals；P2/faults-results.json；P2/journals |','| 只读仪表/严格对齐 | VirtualInstruments / VirtualCaptureArtifacts / WaveCompareSession；P2/probes-results.json |','| 实际 RTL 协议 | build_virtual_tracebridge.py / BitClockTransport / DebugProtocol / DebugAcquisition；P2/rtl-probes/capture.raw/vcd |','| 可信教学 | virtual_teaching_service.py / BlindProjection / VirtualTeachingClient；签名课程；P2/student-dtos、teacher-grading |','| 真实 GUI | VirtualTeachingPanel + Board/Fault/Trace/UART；P2/gui-results.json；P2/gui/teaching-light.png、teaching-dark.png |','','原生核心测试中的计数器替身仅用于代数/会话隔离单测；真实工具断言及 GUI 验收另行编译并运行 Verilated DUT。采集回包逐值核对独立 wrapper tap，禁止用生成 VCD 冒充 RTL 协议采集。','', 'Mode A 的最早实际分歧是开路线末端 valid 状态，不会将无效样本填成 0。Mode B 与同相位独立参考轨严格比较；跨 pre/post-eval 相位明确拒绝。','','## 复现与边界','','执行 `python tools/virtual/verify_phase2_final.py`；需要交互式 Windows 桌面和固定工具链。详细契约、版本差异和使用步骤见 [接口与复现实验](Phase2接口与复现实验.md)。','',*['- '+s for s in limits],'','机器可读结论和逐文件哈希见 [P2-acceptance-summary.json](evidence/P2-acceptance-summary.json)。原 M0/P1 报告及归档保持原字节。','']
    (ROOT/'docs/virtual/Phase2调试探针与教学判题闭环实测报告.md').write_text('\n'.join(lines),encoding='utf-8')
    print(f'P2={good}; {result["test_passed"]}/{len(tests)} CTest; historical={preserved}; source stable={stable}');return 0 if good else 1
if __name__=='__main__':raise SystemExit(main())
