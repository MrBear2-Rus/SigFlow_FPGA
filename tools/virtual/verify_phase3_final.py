"""Stable desktop + 44 CTests; append P3 archives without rewriting M0/P1/P2."""
import gzip,hashlib,json,os,shutil,subprocess,sys,time,xml.etree.ElementTree as ET
from datetime import datetime
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'.work/p3/final';OUT=ROOT/'docs/virtual/evidence';P3=OUT/'P3'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def read(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def write(p,j):Path(p).write_text(json.dumps(j,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def sources():
    paths=set()
    for folder in ['core/src','include','main','tests','tools/virtual','registry','examples/virtual','rtl/debug','agent/src','GUI','InnerPlugin/eda-sim-verilator','InnerPlugin/eda-cst-gowin-cst']:
        paths.update(p for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in {'.cpp','.h','.hpp','.inc','.cmake','.txt','.py','.js','.sv','.v','.cst','.json','.pub','.html','.md'} and not {'__pycache__','bin','CMakeFiles','node_modules'}&set(p.parts))
    paths.update(ROOT/p for p in ['CMakeLists.txt','core/CMakeLists.txt','tests/CMakeLists.txt','.gitattributes','.gitignore'])
    return {p.relative_to(ROOT).as_posix():sha(p) for p in sorted(paths)}
def snapshot_historical():
    """Freeze the M0/P1 ledger plus every already-archived P2 artifact byte."""
    frozen={}
    reference=ROOT/'.work/p2/historical-sha256.json'
    if not reference.exists():reference=OUT/'P2/historical-sha256.json'
    if reference.exists():frozen.update(read(reference))
    for base in [OUT/'P2',OUT/'P2-acceptance-summary.json']:
        if base.is_file():frozen[base.relative_to(ROOT).as_posix()]=sha(base)
        elif base.is_dir():
            for p in base.rglob('*'):
                if p.is_file():frozen[p.relative_to(ROOT).as_posix()]=sha(p)
    return frozen
def main():
    sys.stdout.reconfigure(encoding='utf-8',errors='replace');WORK.mkdir(parents=True,exist_ok=True);P3.mkdir(parents=True,exist_ok=True)
    reference=ROOT/'.work/p3/historical-sha256.json'
    if not reference.exists():
        frozen=snapshot_historical();write(reference,frozen)
        print(f'froze {len(frozen)} historical evidence files into {reference}')
    protected=read(reference)
    if len(protected)<88:raise RuntimeError('historical snapshot incomplete')
    started=time.time();before=sources();original=read(OUT/'P2-acceptance-summary.json');old={t['name'] for t in original['tests']}
    additions={'p3_sfvb_binary_codec_and_js_parity','p3_matrix_keypad_and_timing_faults','p3_float_window_and_teaching_ui'}
    if len(old)!=41:raise RuntimeError('Expected 41 original CTests')
    unchanged=lambda:all((ROOT/p).is_file() and sha(ROOT/p)==digest for p,digest in protected.items())
    if not unchanged():raise RuntimeError('Historical evidence changed before validation')
    env=dict(os.environ);env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+str(ROOT/'build-m0')+';'+env.get('PATH','');temp=WORK/'temp'/str(time.time_ns());temp.mkdir(parents=True,exist_ok=True);env['TMP']=env['TEMP']=str(temp)
    cmake=ROOT/'.work/cmake-python/cmake/data/bin/cmake.exe';ctest=cmake.with_name('ctest.exe');build=ROOT/'build-m0';xml=WORK/'ctest.xml';xml.unlink(missing_ok=True);commands=[]
    for name,args in [('build',[cmake,'--build',build,'--parallel','4']),('ctest',[ctest,'--test-dir',build,'--output-on-failure','-j','1','--output-junit',xml])]:
        print('Running P3 final '+name,flush=True);begin=time.monotonic()
        with (WORK/(name+'.log')).open('w',encoding='utf-8') as stream:r=subprocess.run([str(a) for a in args],cwd=ROOT,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=3600)
        commands.append(dict(argv=[str(a) for a in args],exit_code=r.returncode,seconds=round(time.monotonic()-begin,3)))
        if r.returncode:print((WORK/(name+'.log')).read_text(encoding='utf-8',errors='replace')[-12000:]);break
    tests=[]
    if xml.exists():
        for t in ET.parse(xml).getroot().iter('testcase'):tests.append(dict(name=t.attrib['name'],passed=t.find('failure') is None and t.find('skipped') is None and t.attrib.get('status')!='notrun',seconds=float(t.attrib.get('time',0))))
    reports={name:read(ROOT/'.work/p3'/path) if (ROOT/'.work/p3'/path).exists() else {} for name,path in [('codec','codec/codec-results.json'),('peripherals','peripherals/peripherals-results.json'),('gui','gui/gui-results.json')]}
    native=read(ROOT/'.work/p3/codec/native.json') if (ROOT/'.work/p3/codec/native.json').exists() else {}
    stable=before==sources();preserved=unchanged();byname={t['name'] for t in tests}
    good=len(commands)==2 and all(c['exit_code']==0 for c in commands) and len(tests)==44 and byname==old|additions and all(t['passed'] for t in tests) and stable and preserved and native.get('status')=='passed' and all(r.get('status')=='passed' for r in reports.values())
    artifacts={}
    def copy(source,name=None,compress=False):
        source=Path(source)
        if not source.is_file():return
        target=P3/(name or source.name);target.parent.mkdir(parents=True,exist_ok=True)
        if compress:target.write_bytes(gzip.compress(source.read_bytes(),mtime=0))
        else:shutil.copyfile(source,target)
        artifacts[target.relative_to(P3).as_posix()]=dict(sha256=sha(target),bytes=target.stat().st_size,**(dict(uncompressed_sha256=sha(source),raw_bytes=source.stat().st_size) if compress else {}))
    for name in ['build.log','ctest.log','ctest.xml']:copy(WORK/name)
    for name,path in [('codec-results.json','codec-results.json'),('peripherals-results.json','peripherals-results.json'),('gui-results.json','gui/gui-results.json')]:copy(ROOT/'.work/p3'/path,name)
    copy(ROOT/'.work/p3/codec/native.json','native.json')
    for mode in ['codec','peripherals','gui']:copy(ROOT/'.work/p3'/mode/'progress.log',mode+'-progress.log')
    # SFVB binary stream and every parity projection of the real trajectory.
    for name in ['frames.sfvb','frames.json','decoded.json','js-decoded.json','trajectory.json','encode-summary.json','broken-gap.json','broken-baseline.json','broken-stale.json','glitch-events.json']:
        copy(ROOT/'.work/p3/codec'/name,'codec/'+name)
    for name in ['teaching-light.png','teaching-dark.png','agent-evidence.json','student-controller-frame.json']:
        copy(ROOT/'.work/p3/gui'/name,'gui/'+name)
    for p in (ROOT/'.work/p3/peripherals').glob('*-results.json'):copy(p)
    # Archive journals produced by this actual run: one per scenario folder.
    for scenario in ['codec','peripherals','gui']:
        base=ROOT/'.work/p3'/scenario
        if not base.is_dir():continue
        for p in base.rglob('private.journal.jsonl'):
            if p.stat().st_mtime>=started:copy(p,'journals/'+scenario+'-'+p.parent.parent.name+'.jsonl.gz',True)
    write(P3/'historical-sha256.json',protected);artifacts['historical-sha256.json']=dict(sha256=sha(P3/'historical-sha256.json'),bytes=(P3/'historical-sha256.json').stat().st_size)
    limits=['SFVB v1 frames are the frozen sigflow.virtual-frame.v1 projection; payload trace blocks are uncompressed','JS reference decoder covers the frame layer with DataView/BigInt; no browser runtime claims','Float window migration is view-host only; physical multi-monitor dragging is manual UX, placement is verified per-display','Offscreen capture renders via PW_RENDERFULLCONTENT with a recursive WM_PRINT fallback; no analog voltage is claimed','Matrix keypad ghosting follows the digital junction model; push-pull scan contention pauses under the strict UnknownInput guard','reset_glitch and clock_divider_offset are digital teaching timing faults; no analog metastability or MTBF claims','Diode board variant is generated at acceptance time from the reviewed phase 3 board; registry stays single-source','Agent bridge sends student projections only; grading stays with deterministic teaching.grade.submit','Web remains deferred; binary frames are consumed by the reference decoder, not a browser GUI']
    result=dict(schema='sigflow.phase3-acceptance.v1',date=datetime.now().astimezone().isoformat(),status='passed' if good else 'failed',Phase3_complete=bool(good),base_commit='4edd496',branch=subprocess.check_output(['git','branch','--show-current'],cwd=ROOT,text=True).strip(),test_total=len(tests),test_passed=sum(t['passed'] for t in tests),original_tests=41,new_tests=3,tests=tests,commands=commands,source_unchanged_during_validation=stable,source_sha256=before,historical_archive_unchanged=preserved,historical_count=len(protected),historical_sha256=protected,native=native,codec=reports['codec'],peripherals=reports['peripherals'],gui=reports['gui'],desktop_binary=dict(path=str(build/'sigflow.exe'),sha256=sha(build/'sigflow.exe')),artifacts=artifacts,scope_limits=limits)
    write(OUT/'P3-acceptance-summary.json',result)
    lines=['# Phase 3 桌面产品化深化与跨端同源实测报告','',f'验收时间：{result["date"]}；基线 `4edd496`；分支 `{result["branch"]}`。','',f'最终状态：**{result["status"]}**，`Phase3_complete={str(good).lower()}`。','', '| 验收 | 实测结果 |','|---|---|',f'| 全部 CTest | {result["test_passed"]}/{len(tests)}；原 41 项 + 新 3 项 |',f'| SFVB 原生代数/故障/键盘 | {native.get("checks",0)} 项检查；含 64 字节头逐字段小端对照 |',f'| SFVB 跨端一致性 | {reports["codec"].get("check_count",0)} 项；真实 Verilator 轨迹 C++↔JS 逐字段一致 |',f'| 矩阵键盘/时序故障 | {reports["peripherals"].get("check_count",0)} 项真实 RTL 断言；鬼键 vs 二极管、CON 争用守卫 |',f'| GUI | {reports["gui"].get("check_count",0)} 项真实 wxApp 断言；20 次弹出↔回归 + 修复后 Light/Dark 截图 |',f'| 历史归档 | {len(protected)} 份 SHA-256 保持：{preserved}；M0+P1 88 份与全部 P2 归档 |',f'| 参与验收的源码 | 构建前后逐文件哈希一致：{stable} |','','## 实现与证据','','| 模块 | 生产入口 / 证据 |','|---|---|','| SFVB 二进制帧 | BinaryFrameCodec（零 wx）；P3/codec/frames.sfvb、frames.json、decoded.json |','| JS 参考解码器 | tools/virtual/sfvb_reference_decoder.js（DataView/BigInt）；P3/codec/js-decoded.json |','| 时序故障 | Phase3Faults：reset_glitch / clock_divider_offset；P3/peripherals-results.json |','| 矩阵键盘 | sigflow.matrix-keypad@1 + phase3 板卡 + examples/virtual/phase3；鬼键 vs 二极管可配 |','| 独立浮动窗口 | VirtualBoardFloatFrame + 占位页；20 次往返不销毁共享会话 |','| 离屏截图修复 | PrintWindow(PW_RENDERFULLCONTENT) + WM_PRINT 递归合成；P3/gui/teaching-light.png、teaching-dark.png |','| 教学 Agent 联动 | VirtualTeachingPanel::ComposeEvidence → AgentApiClient 通道；P3/gui/agent-evidence.json |','','原生核心测试中的替身仅用于代数/会话隔离单测；SFVB 轨迹、键盘、故障与 GUI 验收均以真实 Verilator DUT 与真实 wxApp 执行。盲测运行经 StudentProjection 投影后再编码，跨端解码仍零泄密。','','## 复现与边界','','执行 `python tools/virtual/verify_phase3_final.py`；需要交互式 Windows 桌面、Node.js 24 与固定工具链。三个新模式亦可单独运行：`python tools/virtual/verify_phase3.py --mode codec|peripherals` 与 `python tools/virtual/verify_phase3_gui.py`。','',*['- '+s for s in limits],'','机器可读结论和逐文件哈希见 [P3-acceptance-summary.json](evidence/P3-acceptance-summary.json)。原 M0/P1/P2 报告及归档保持原字节。']
    (ROOT/'docs/virtual/Phase3产品化深化与跨端同源实测报告.md').write_text('\n'.join(lines),encoding='utf-8')
    print(f'P3={good}; {result["test_passed"]}/{len(tests)} CTest; historical={preserved}; source stable={stable}');return 0 if good else 1
if __name__=='__main__':raise SystemExit(main())
