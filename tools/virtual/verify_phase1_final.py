"""Archive P1 evidence only after a stable build and all original + new CTests."""
import gzip,hashlib,json,os,shutil,subprocess,sys,time,xml.etree.ElementTree as ET
from pathlib import Path
from verify_m0_final import sources as m0_sources
ROOT=Path(__file__).resolve().parents[2];WORK=ROOT/'.work/p1/final';OUT=ROOT/'docs/virtual/evidence'
BASE='988570e709abe0b13037028063490c7c1ba468a4'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def read(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def sources():
    result=m0_sources()
    for p in (ROOT/'tests/virtual/p1').glob('*.inc'):result[p.relative_to(ROOT).as_posix()]=sha(p)
    return result
def m0_archive():
    files=subprocess.check_output(['git','ls-tree','-r','--name-only','-z',BASE,'--','docs/virtual'],cwd=ROOT).decode('utf-8').rstrip('\0').split('\0')
    return {p:sha(ROOT/p) for p in files}
def main():
    sys.stdout.reconfigure(encoding='utf-8',errors='replace');WORK.mkdir(parents=True,exist_ok=True);before=sources();archive=m0_archive();commands=[];tests=[]
    env=dict(os.environ);env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+env.get('PATH','');temp=WORK/'temp';temp.mkdir(exist_ok=True);env['TMP']=env['TEMP']=str(temp)
    cmake=ROOT/'.work/cmake-python/cmake/data/bin/cmake.exe';ctest=cmake.with_name('ctest.exe');build=ROOT/'build-m0';xml=WORK/'ctest.xml';xml.unlink(missing_ok=True)
    for name,args in [('build',[cmake,'--build',build,'--parallel','4']),('ctest',[ctest,'--test-dir',build,'--output-on-failure','-j','1','--output-junit',xml])]:
        print('Running P1 final '+name,flush=True);start=time.monotonic();r=subprocess.run([str(a) for a in args],cwd=ROOT,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=1800)
        (WORK/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8');commands.append(dict(argv=[str(a) for a in args],exit_code=r.returncode,seconds=round(time.monotonic()-start,3)))
        if r.returncode:print((r.stdout+r.stderr)[-10000:]);break
    if xml.exists():
        for t in ET.parse(xml).getroot().iter('testcase'):tests.append(dict(name=t.attrib['name'],passed=t.find('failure') is None and t.find('skipped') is None and t.attrib.get('status')!='notrun',seconds=float(t.attrib.get('time',0))))
    original={t['name'] for t in read(OUT/'M0-final-acceptance-summary.json')['tests']};required=original|{'p1_four_faults_and_rng_replay','p1_peripherals_pwm_seg_uart','p1_gui_mvp_acceptance'}
    byname={t['name']:t for t in tests};after=sources();stable=before==after;preserved=archive==m0_archive();reports={}
    for name,p in [('faults',ROOT/'.work/p1/faults-results.json'),('peripherals',ROOT/'.work/p1/peripherals-results.json'),('gui',ROOT/'.work/p1/gui/gui-results.json')]:reports[name]=read(p) if p.exists() else {}
    good=len(commands)==2 and all(c['exit_code']==0 for c in commands) and stable and preserved and required.issubset(byname) and all(t['passed'] for t in tests) and all(r.get('status')=='passed' for r in reports.values())
    artifacts={};targetDir=OUT/'P1';targetDir.mkdir(exist_ok=True)
    copies={'build.log':WORK/'build.log','ctest.log':WORK/'ctest.log','ctest.xml':xml,'faults-results.json':ROOT/'.work/p1/faults-results.json','peripherals-results.json':ROOT/'.work/p1/peripherals-results.json','gui-results.json':ROOT/'.work/p1/gui/gui-results.json','gui-progress.log':ROOT/'.work/p1/gui/progress.log','gui.log':ROOT/'.work/p1/gui/gui.log','uart-export.json':ROOT/'.work/p1/gui/uart-export.json'}
    for folder in ['faults','peripherals']:
        copies[folder+'-native.json']=ROOT/'.work/p1'/folder/(folder+'-native.json')
        copies[folder+'-progress.log']=ROOT/'.work/p1'/folder/'progress.log'
    for p in (ROOT/'.work/p1/gui').glob('*.png'):copies[p.name]=p
    for name,p in copies.items():
        if p.exists():target=targetDir/name;shutil.copyfile(p,target);artifacts[name]=dict(sha256=sha(target),bytes=target.stat().st_size)
    # Keep representative raw journals reviewable without committing huge JSONL.
    # Configs name UUID paths, so select the exact most recent acceptance run.
    for folder,names in [('faults',['bounce','clock-stop','replace_source','open-window']),('peripherals',['pwm-segments','uart-8n1','uart-bad-stop-short_to_rail'])]:
        for name in names:
            cfg=ROOT/'.work/p1'/folder/(name+'.json')
            if cfg.exists():
                path=Path(read(cfg)['journal_path'])
                if path.exists():
                    target=targetDir/(name+'.journal.jsonl.gz');target.write_bytes(gzip.compress(path.read_bytes(),mtime=0));artifacts[target.name]=dict(sha256=sha(target),bytes=target.stat().st_size,uncompressed_sha256=sha(path),compression='gzip',raw_bytes=path.stat().st_size)
    result=dict(schema='sigflow.phase1-acceptance.v1',date='2026-11-17',timezone='Asia/Shanghai',status='passed' if good else 'failed',Phase1_complete=bool(good),base_commit=BASE,branch=subprocess.check_output(['git','branch','--show-current'],cwd=ROOT,text=True).strip(),test_total=len(tests),test_passed=sum(t['passed'] for t in tests),original_m0_tests=len(original),tests=tests,commands=commands,source_unchanged_during_validation=stable,source_sha256=after,m0_archive_unchanged=preserved,m0_archive_sha256=archive,desktop_binary=dict(path=str(build/'sigflow.exe'),sha256=sha(build/'sigflow.exe')),faults=reports['faults'],peripherals=reports['peripherals'],gui=reports['gui'],artifacts=artifacts,journal_protocol='sigflow.session-journal.p1.v1',scheduler='sigflow.scheduler.p1.v1',digest='sigflow.sha256-chain.p1.v1',rng='pcg32-v1',scope_limits=['Digital functional models; no analog current or metastability model','10ms bounce acceptance uses explicitly declared 1kHz teaching fixture; default GUI board remains actual 27MHz with every edge evaluated','wx injected DPI surfaces, no physical multi-monitor switch','Web deferred; core has no wx dependency'])
    (OUT/'P1-acceptance-summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('P1='+str(good)+'; '+str(result['test_passed'])+'/'+str(len(tests))+' CTest; source stable='+str(stable)+'; M0 archive preserved='+str(preserved));return 0 if good else 1
if __name__=='__main__':raise SystemExit(main())
