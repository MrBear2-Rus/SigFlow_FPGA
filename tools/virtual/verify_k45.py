"""Build the shipping desktop and archive K4/K5 Core + U0 acceptance."""
import hashlib,json,os,subprocess,time,xml.etree.ElementTree as ET
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];WORK=ROOT/'.work/k45';OUT=ROOT/'docs/virtual/evidence'
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def read(path):return json.loads(Path(path).read_text(encoding='utf-8'))
def write(path,data):Path(path).write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def sources():
    files=set()
    for folder in ['core/src','include/eda/api','main/virtual','main/trace','tests/virtual','tools/virtual','registry','InnerPlugin/eda-sim-verilator']:
        for p in (ROOT/folder).rglob('*'):
            if p.is_file() and p.suffix in {'.cpp','.h','.hpp','.json','.py','.sv','.cst','.txt','.pdf'} and '__pycache__' not in p.parts:files.add(p)
    for name in ['CMakeLists.txt','core/CMakeLists.txt','tests/CMakeLists.txt','main/MainFrame.cpp','main/MainFrame.h',
                 'main/fpga/FpgaToolWindow.cpp','main/fpga/FpgaToolWindow.h','main/ui/UiTheme.cpp','main/ui/UiTheme.h',
                 'main/wave/TraceViewPanel.cpp','main/wave/TraceViewPanel.h']:files.add(ROOT/name)
    return {p.relative_to(ROOT).as_posix():sha(p) for p in sorted(files)}
def main():
    WORK.mkdir(parents=True,exist_ok=True);OUT.mkdir(parents=True,exist_ok=True);before=sources();env=dict(os.environ)
    env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+env.get('PATH','');commands=[];failure=None
    cmake=ROOT/'.work/cmake-python/cmake/data/bin/cmake.exe';ctest=cmake.with_name('ctest.exe');build=ROOT/'build-m0';xml=WORK/'ctest-final.xml';xml.unlink(missing_ok=True)
    for name,argv in [('build-final',[cmake,'--build',build,'--parallel','4']),('ctest-final',[ctest,'--test-dir',build,'--output-on-failure','-j','3','--output-junit',xml])]:
        started=time.monotonic();r=subprocess.run([str(a) for a in argv],cwd=ROOT,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=1200)
        (WORK/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8');commands.append(dict(argv=[str(a) for a in argv],exit_code=r.returncode,seconds=round(time.monotonic()-started,3)))
        if r.returncode:failure=name+' failed';break
    tests=[]
    if xml.exists():
        for t in ET.parse(xml).getroot().iter('testcase'):tests.append(dict(name=t.attrib['name'],passed=t.find('failure') is None and t.find('skipped') is None and t.attrib.get('status')!='notrun',seconds=float(t.attrib.get('time',0)),output=t.findtext('system-out','')))
    after=sources();stable=before==after;byname={t['name']:t for t in tests}
    required={t['name'] for t in read(OUT/'K3-live-trace-summary.json')['tests']}|{'k4_board_wiring_and_cst_variants','k5_topology_transactions','k5_open_fault_guard_and_replay','k5_legacy_real_build_run_vcd','u0_virtual_board_gui_smoke'}
    good=not failure and stable and required.issubset(byname) and all(t['passed'] for t in tests)
    def result(path):return read(path) if Path(path).exists() else {'status':'missing'}
    real=result(WORK/'real-results.json');gui=result(WORK/'gui/gui-results.json');legacy=result(WORK/'legacy/legacy-results.json');tsan=result(ROOT/'.work/k23/tsan-results.json')
    good=good and real['status']=='passed' and gui['status']=='passed' and legacy['status']=='passed' and tsan['status']=='passed'
    for name in ['normal_replays','fault_replays']:
        runs=real.get(name,[]);good=good and len(runs)==3 and all(r==runs[0] for r in runs)
    mapping={
        'A1':['k5_open_fault_guard_and_replay','u0_virtual_board_gui_smoke'],
        'A2':['k1_port_index_roundtrip','k1_real_tool_gui_cst'],
        'A3':['k1_electrical_acceptance'],
        'A4':['k4_board_wiring_and_cst_variants','k5_open_fault_guard_and_replay'],
        'A5':['k2_real_verilated_session','u0_virtual_board_gui_smoke'],
        'A6':['k2_session_pause_guard_replay','k2_real_verilated_session'],
        'A7':['k5_topology_transactions','k5_open_fault_guard_and_replay'],
        'A8':['k3_live_trace_concurrency_and_cases','k3_live_trace_thread_sanitizer'],
        'A9':['k5_topology_transactions','k5_open_fault_guard_and_replay'],
        'A10':['k5_open_fault_guard_and_replay','k5_legacy_real_build_run_vcd','eda_wave_vcd_smoke','k3_live_trace_concurrency_and_cases']}
    acceptance={key:dict(core_runner_passed=all(byname.get(n,{}).get('passed',False) for n in names),tests=names) for key,names in mapping.items()}
    good=good and all(a['core_runner_passed'] for a in acceptance.values())
    acceptance['A8']['gui_same_tick_marker']='U1 pending; QueryExtended contract already available'
    artifacts={}
    copies={
        'K45-build-final.log':WORK/'build-final.log','K45-ctest-final.log':WORK/'ctest-final.log','K45-ctest-final.xml':xml,
        'K45-thread-sanitizer.log':ROOT/'.work/k23/linux/tsan-final.log','K45-legacy-jobs.log':WORK/'legacy/jobs.log',
        'K45-normal-journal.json':WORK/'real/normal-journal.json','K45-fault-journal.json':WORK/'real/fault-journal.json',
        'U0-board-light.png':WORK/'gui/u0-board-light.png','U0-board-pending.png':WORK/'gui/u0-board-pending.png','U0-board-open-dark-150.png':WORK/'gui/u0-board-open-dark-150.png'}
    for name,p in copies.items():
        if p.exists():target=OUT/name;target.write_bytes(p.read_bytes());artifacts[name]=dict(sha256=sha(target),bytes=target.stat().st_size)
    report=dict(date='2026-11-16',timezone='Asia/Shanghai',status='passed' if good else 'failed',K0_K5_core_complete=bool(good),K4=bool(good),K5=bool(good),U0=bool(good),U1=False,full_product_M0=False,
        scope='Core/Runner A1-A10 plus U0 real controller/rendering; U1 joint product acceptance and same-tick GUI marker remain pending',
        branch=subprocess.check_output(['git','branch','--show-current'],cwd=ROOT,text=True).strip(),base_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        feature_ancestor=subprocess.run(['git','merge-base','--is-ancestor','feat/virtual-fpga-m0','HEAD'],cwd=ROOT).returncode==0,
        source_unchanged_during_validation=stable,source_sha256=after,source_snapshot_sha256=hashlib.sha256(json.dumps(after,sort_keys=True).encode()).hexdigest(),
        test_total=len(tests),test_passed=sum(t['passed'] for t in tests),tests=tests,commands=commands,acceptance=acceptance,real_runner=real,gui=gui,legacy_real_jobs=legacy,thread_sanitizer=tsan,
        desktop_binary=dict(path=str(build/'sigflow.exe'),sha256=sha(build/'sigflow.exe')),
        journal_protocol='sigflow.session-journal.m0.v3',scheduler='sigflow.scheduler.m0.v3',digest='sigflow.sha256-chain.v2',
        legacy_environment=dict(CXX='MinGW GCC 14.2',LINK='MinGW GCC 14.2',AR='MinGW ar',PYTHON3='current Python executable',MAKEFLAGS='-e',scope='test child only; locked Verilator runtime embeds unavailable build-machine defaults'),
        artifacts=artifacts,failure=failure)
    write(OUT/'K4-K5-acceptance-summary.json',report)
    print(f"K4/K5/U0: {report['test_passed']}/{len(tests)} CTest; core complete={bool(good)}; source stable={stable}")
    if not good:raise SystemExit(1)
if __name__=='__main__':main()
