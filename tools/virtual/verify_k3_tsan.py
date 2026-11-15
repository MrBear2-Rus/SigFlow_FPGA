"""Instrument the complete LiveTrace/cache/VCD test under ThreadSanitizer."""
import argparse,json,os,platform,shlex,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--distro',default='Ubuntu-24.04');args=parser.parse_args()
    work=ROOT/'.work/k23/linux';work.mkdir(parents=True,exist_ok=True)
    sources=['tests/virtual/k3/k3_live_trace_tests.cpp','core/src/eda-virtual-board/LiveTraceStore.cpp',
             'core/src/eda-platform/Platform.cpp','core/src/eda-platform/Sha256.cpp','main/trace/LiveTraceSource.cpp',
             'main/trace/TraceCache.cpp','main/trace/TraceMemoryBudget.cpp','main/trace/WaveformBackendTraceSource.cpp',
             'InnerPlugin/eda-wave-vcd/src/VcdWaveformBackend.cpp']
    include=['include','3rd','core/src','main/trace','InnerPlugin/eda-wave-vcd/include']
    linux_root='/mnt/'+ROOT.drive[0].lower()+ROOT.as_posix()[2:] if os.name=='nt' else str(ROOT)
    compile_args=['g++','-std=c++20','-O1','-g','-fsanitize=thread','-pthread',*['-I'+p for p in include],*sources,'-o','.work/k23/linux/k3_tsan']
    # GCC TSan reserves a fixed virtual address range. Disable ASLR for this
    # child only: WSL's randomized mappings can otherwise collide before main.
    # Never retry sanitizer failures or modify the machine's sysctl settings.
    architecture='x86_64' if os.name=='nt' else platform.machine()
    run_args=['setarch',architecture,'-R','.work/k23/linux/k3_tsan',linux_root,linux_root+'/.work/k23/linux']
    command='cd '+shlex.quote(linux_root)+' && '+shlex.join(compile_args)+' && TSAN_OPTIONS=halt_on_error=1 '+shlex.join(run_args)
    argv=['wsl','-d',args.distro,'--exec','bash','-lc',command] if os.name=='nt' else ['bash','-lc',command]
    start=time.monotonic();result=subprocess.run(argv,cwd=ROOT,capture_output=True,encoding='utf-8',errors='replace',timeout=180)
    text=result.stdout+result.stderr;(work/'tsan-final.log').write_text(text,encoding='utf-8')
    passed=result.returncode==0 and 'LiveTrace checks' in text and 'WARNING: ThreadSanitizer' not in text
    (ROOT/'.work/k23/tsan-results.json').write_text(json.dumps(dict(status='passed' if passed else 'failed',exit_code=result.returncode,
        seconds=round(time.monotonic()-start,3),argv=argv,output=text,instrumented_sources=sources,
        address_space='setarch -R for instrumented child only; no retries or suppressions'),indent=2)+'\n',encoding='utf-8')
    print(text,end='')
    if not passed:raise SystemExit(result.returncode or 1)
    print('PASS ThreadSanitizer: no data-race report in instrumented LiveTrace/cache/VCD stress test')
if __name__=='__main__':main()
