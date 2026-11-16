"""Freeze only evidence produced by an unchanged full desktop build + all CTest."""
import hashlib,json,os,shutil,subprocess,time,xml.etree.ElementTree as ET
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];WORK=ROOT/'.work/m0-final';OUT=ROOT/'docs/virtual/evidence'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def read(p):return json.loads(Path(p).read_text(encoding='utf-8'))
def sources():
    files=set()
    for folder in ['core/src','include/eda/api','main/virtual','main/trace','main/wave','main/fpga','main/ui','tests/virtual','tools/virtual','registry','examples/virtual','InnerPlugin/eda-sim-verilator','InnerPlugin/eda-cst-gowin-cst']:
        files.update(p for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in {'.cpp','.h','.hpp','.json','.py','.sv','.cst','.txt','.pdf'} and '__pycache__' not in p.parts)
    files.update(ROOT/p for p in ['CMakeLists.txt','core/CMakeLists.txt','tests/CMakeLists.txt','main/MainFrame.cpp','main/MainFrame.h'])
    return {p.relative_to(ROOT).as_posix():sha(p) for p in sorted(files)}
def main():
    WORK.mkdir(parents=True,exist_ok=True);OUT.mkdir(parents=True,exist_ok=True);before=sources();env=dict(os.environ);env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+env.get('PATH','')
    cmake=ROOT/'.work/cmake-python/cmake/data/bin/cmake.exe';ctest=cmake.with_name('ctest.exe');build=ROOT/'build-m0';xml=WORK/'ctest.xml';xml.unlink(missing_ok=True);commands=[]
    for name,argv in [('build',[cmake,'--build',build,'--parallel','4']),('ctest',[ctest,'--test-dir',build,'--output-on-failure','-j','3','--output-junit',xml])]:
        print('Running '+name,flush=True);start=time.monotonic();r=subprocess.run([str(x) for x in argv],cwd=ROOT,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=1500)
        (WORK/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8');commands.append(dict(argv=[str(x) for x in argv],exit_code=r.returncode,seconds=round(time.monotonic()-start,3)))
        if r.returncode:print((r.stdout+r.stderr)[-12000:]);break
    after=sources();tests=[]
    if xml.exists():
        for t in ET.parse(xml).getroot().iter('testcase'):tests.append(dict(name=t.attrib['name'],passed=t.find('failure') is None and t.find('skipped') is None and t.attrib.get('status')!='notrun',seconds=float(t.attrib.get('time',0))))
    required={t['name'] for t in read(OUT/'K4-K5-acceptance-summary.json')['tests']}|{'u1_m0_full_product_acceptance'}
    byname={t['name']:t for t in tests};gui=read(ROOT/'.work/u1-acceptance/gui-results.json') if (ROOT/'.work/u1-acceptance/gui-results.json').exists() else {};tsan=read(ROOT/'.work/k23/tsan-results.json') if (ROOT/'.work/k23/tsan-results.json').exists() else {}
    good=len(commands)==2 and all(c['exit_code']==0 for c in commands) and before==after and required.issubset(byname) and all(t['passed'] for t in tests) and gui.get('status')=='passed' and all(gui.get('A'+str(i)) is True for i in range(1,11)) and tsan.get('status')=='passed'
    artifacts={};copies={'M0-build-final.log':WORK/'build.log','M0-ctest-final.log':WORK/'ctest.log','M0-ctest-final.xml':xml,'U1-gui-results.json':ROOT/'.work/u1-acceptance/gui-results.json','U1-gui-progress.log':ROOT/'.work/u1-acceptance/progress.log','U1-gui-stderr.log':ROOT/'.work/u1-acceptance/gui.log','U1-same-tick-details.json':ROOT/'.work/u1-acceptance/same-tick-details.json','U1-same-tick-marker.png':ROOT/'.work/u1-acceptance/same-tick-marker.png','U1-normal.journal.jsonl':ROOT/'.work/u1-acceptance/normal.journal.jsonl','U1-fault.journal.jsonl':ROOT/'.work/u1-acceptance/fault.journal.jsonl','M0-thread-sanitizer.log':ROOT/'.work/k23/linux/tsan-final.log'}
    for p in (ROOT/'.work/u1-acceptance').glob('*dpi.png'):copies['U1-'+p.name]=p
    for name,p in copies.items():
        if p.exists():shutil.copyfile(p,OUT/name);artifacts[name]=dict(sha256=sha(OUT/name),bytes=(OUT/name).stat().st_size)
    result=dict(schema='sigflow.m0-final-acceptance.v1',date='2026-11-16',timezone='Asia/Shanghai',status='passed' if good else 'failed',K0_K5_core_complete=bool(good),U0=bool(good),U1=bool(good),full_product_M0=bool(good),
        **{'A'+str(i):bool(good and gui.get('A'+str(i))) for i in range(1,11)},test_total=len(tests),test_passed=sum(t['passed'] for t in tests),tests=tests,commands=commands,
        source_unchanged_during_validation=before==after,source_sha256=after,source_snapshot_sha256=hashlib.sha256(json.dumps(after,sort_keys=True).encode()).hexdigest(),
        desktop_binary=dict(path=str(build/'sigflow.exe'),sha256=sha(build/'sigflow.exe')),gui_assertions=len(gui.get('checks',[])),gui_host=gui.get('host'),lifecycle_cycles=gui.get('lifecycle_cycles'),
        dpi_scope=gui.get('dpi_scope'),real_monitor_dpi_test=gui.get('real_monitor_dpi_test',False),normal_replays=gui.get('normal_replays'),fault_replays=gui.get('fault_replays'),artifacts=artifacts,
        base_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),branch=subprocess.check_output(['git','branch','--show-current'],cwd=ROOT,text=True).strip(),
        journal_protocol='sigflow.session-journal.m0.v3',digest='sigflow.sha256-chain.v2')
    (OUT/'M0-final-acceptance-summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f"M0={good}; {result['test_passed']}/{len(tests)} CTest; {result['gui_assertions']} GUI assertions; source stable={before==after}")
    return 0 if good else 1
if __name__=='__main__':raise SystemExit(main())
