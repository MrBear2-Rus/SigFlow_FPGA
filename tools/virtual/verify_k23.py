"""Build and archive the complete K2/G2 + K3/G3/A8 production acceptance run."""
import argparse,hashlib,json,os,re,subprocess,time,xml.etree.ElementTree as ET
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'.work/k23'
OUT=ROOT/'docs/virtual/evidence'
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def write(path,data):Path(path).write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def source_hashes():
    files=set()
    for folder in ['core/src/eda-virtual-board','main/trace','tests/virtual','tools/virtual','registry']:
        for path in (ROOT/folder).rglob('*'):
            if path.is_file() and path.suffix in {'.h','.hpp','.cpp','.sv','.cst','.json','.py','.ps1','.pdf','.txt'} and '__pycache__' not in path.parts:files.add(path)
    for name in ['CMakeLists.txt','core/CMakeLists.txt','tests/CMakeLists.txt','core/src/eda-platform/DurableFile.h','core/src/eda-platform/DurableFile.cpp',
                 'include/eda/api/live_trace.hpp','include/eda/api/port_manifest.hpp','include/eda/api/logic_state.hpp','include/eda/api/electrical_profile.hpp']:
        files.add(ROOT/name)
    return {p.relative_to(ROOT).as_posix():sha(p) for p in sorted(files)}
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--build-dir',type=Path,default=ROOT/'build-m0');args=parser.parse_args();build=args.build_dir.resolve()
    WORK.mkdir(parents=True,exist_ok=True);OUT.mkdir(parents=True,exist_ok=True)
    before=source_hashes();env=dict(os.environ);env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+env.get('PATH','')
    cmake=ROOT/'.work/cmake-python/cmake/data/bin/cmake.exe';ctest=cmake.with_name('ctest.exe')
    junit=WORK/'ctest-final.xml';junit.unlink(missing_ok=True);commands=[];failure=None
    for name,argv in [('build-final',[cmake,'--build',build,'--parallel','4']),
                      ('ctest-final',[ctest,'--test-dir',build,'--output-on-failure','-j','3','--output-junit',junit])]:
        start=time.monotonic();result=subprocess.run([str(x) for x in argv],cwd=ROOT,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=1200)
        (WORK/(name+'.log')).write_text(result.stdout+result.stderr,encoding='utf-8')
        commands.append(dict(argv=[str(x) for x in argv],exit_code=result.returncode,seconds=round(time.monotonic()-start,3)))
        if result.returncode:failure=name+' failed';break
    tests=[]
    if junit.exists():
        for t in ET.parse(junit).getroot().iter('testcase'):
            tests.append(dict(name=t.attrib['name'],passed=t.find('failure') is None and t.find('skipped') is None and t.attrib.get('status')!='notrun',
                              seconds=float(t.attrib.get('time',0)),output=t.findtext('system-out','')))
    after=source_hashes();unchanged=before==after
    baseline=json.loads((OUT/'K1-implementation-summary.json').read_text(encoding='utf-8'))
    required={t['name'] for t in baseline['tests']}|{'k3_live_trace_concurrency_and_cases'}
    names={t['name'] for t in tests};tsan_enabled='SIGFLOW_ENABLE_WSL_TSAN:BOOL=ON' in (build/'CMakeCache.txt').read_text(encoding='utf-8')
    if tsan_enabled:required.add('k3_live_trace_thread_sanitizer')
    all_passed=not failure and unchanged and bool(tests) and all(t['passed'] for t in tests) and required.issubset(names)
    actual_path=WORK/'generated-runner-results.json';actual=json.loads(actual_path.read_text(encoding='utf-8')) if actual_path.exists() else {}
    tsan_path=WORK/'tsan-results.json';tsan=json.loads(tsan_path.read_text(encoding='utf-8')) if tsan_path.exists() else {}
    g2=all_passed and actual.get('status')=='passed' and actual.get('G2') is True and len(actual.get('modules',[]))==2
    g3=all_passed and (not tsan_enabled or tsan.get('status')=='passed')
    fingerprint=hashlib.sha256(json.dumps(after,sort_keys=True,ensure_ascii=False).encode('utf-8')).hexdigest()
    common=dict(date='2026-11-14',timezone='Asia/Shanghai',base_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                commands=commands,test_total=len(tests),test_passed=sum(t['passed'] for t in tests),tests=tests,
                source_unchanged_during_validation=unchanged,source_sha256=after,source_snapshot_sha256=fingerprint,
                desktop_binary=dict(path=str(build/'sigflow.exe'),sha256=sha(build/'sigflow.exe')),
                failure=failure,execution='Windows native GCC 14.2; full process regressions outside restricted sandbox',
                scope='G2/K2a-c and G3/A8 only; no GUI virtual-board/fault or complete M0 acceptance claim')
    k2=dict(common,stage='K2-closeout',status='passed' if g2 else 'failed',G2=g2,K2_closed=g2,
            generated_runner_evidence=actual,protocol='write-ahead begin -> evaluated prepare -> checksummed marker + OS sync -> immutable trace publication -> applied ACK',
            recovery_policy='Truncate incomplete tail to batch begin; reject committed checksum corruption; exclusive writer lock.',
            digests=['normalized position/effective inputs/outputs','control journey with superseded/blocked/Play/Step/Pause/Stop'])
    native=next((t for t in tests if t['name']=='k3_live_trace_concurrency_and_cases'),{})
    k3=dict(common,stage='K3-live-trace',status='passed' if g3 else 'failed',G3=g3,A8=g3,
            native_stress=native,thread_sanitizer=tsan if tsan_enabled else {'status':'not_enabled'},
            fixture_sha256=sha(ROOT/'tests/virtual/k3/fixtures/live-trace-cases.json'),
            stress=dict(writer_threads=1,reader_threads=4,batches=1000,transitions=3000,includes_same_tick_multi_delta=True),
            cache_identity=['run_id','generation','trace_revision','signal','range','query_mode'],
            storage='Immutable chunks with copy-on-write 128-chunk index pages; owned read-view leases and explicit quotas',
            cursor='Pinned run/generation/revision/view/signal/range identity; no implicit switch to latest',
            time_projection='Raw integer ticks preserved; delta is never mapped to fake ps')
    write(OUT/'K2-closeout-summary.json',k2);write(OUT/'K3-live-trace-summary.json',k3)
    for name in ['ctest-final.xml','ctest-final.log','build-final.log']:
        if (WORK/name).exists():(OUT/('K23-'+name)).write_bytes((WORK/name).read_bytes())
    if tsan_enabled and (WORK/'linux/tsan-final.log').exists():(OUT/'K3-thread-sanitizer.log').write_bytes((WORK/'linux/tsan-final.log').read_bytes())
    for name in ['k2_counter','k2_shifted','k2_nonconvergent']:
        for kind in ['generate','compile']:
            p=WORK/'generated'/(name+'-'+kind+'.log')
            if p.exists():(OUT/('K2-'+name+'-'+kind+'.log')).write_bytes(p.read_bytes())
    print(f"K23: {common['test_passed']}/{len(tests)} CTest; G2={g2}, G3/A8={g3}; source stable={unchanged}")
    if not g2 or not g3:raise SystemExit(1)
if __name__=='__main__':main()
