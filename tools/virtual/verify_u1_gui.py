"""One CTest entry: actual legacy job providers followed by full wxApp acceptance."""
import argparse,json,os,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
sys.stderr.reconfigure(encoding='utf-8',errors='replace')
def main():
    p=argparse.ArgumentParser();p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--gui',type=Path,required=True);a=p.parse_args()
    work=ROOT/'.work/u1-acceptance';work.mkdir(parents=True,exist_ok=True)
    result=work/'gui-results.json';result.unlink(missing_ok=True)
    begin=time.monotonic();legacy=[sys.executable,str(ROOT/'tools/virtual/verify_legacy_jobs.py'),'--build-dir',str(a.build_dir)]
    r=subprocess.run(legacy,cwd=ROOT,capture_output=True,encoding='utf-8',errors='replace',timeout=240)
    (work/'legacy.log').write_text(r.stdout+r.stderr,encoding='utf-8');(work/'legacy-result.json').write_text(json.dumps(dict(argv=legacy,exit_code=r.returncode,seconds=time.monotonic()-begin)),encoding='utf-8')
    if r.returncode:print(r.stdout+r.stderr);return r.returncode
    env=dict(os.environ);env['SIGFLOW_PYTHON3']=sys.executable;env['SIGFLOW_REPOSITORY_ROOT']=ROOT.as_posix();env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+env.get('PATH','')
    r=subprocess.run([str(a.gui),str(ROOT),str(a.build_dir)],cwd=ROOT,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=650)
    (work/'gui.log').write_text(r.stdout+r.stderr,encoding='utf-8');print(r.stdout+r.stderr,end='')
    if r.returncode:return r.returncode
    report=json.loads(result.read_text(encoding='utf-8'))
    if report['status']!='passed' or not all(report['A'+str(i)] is True for i in range(1,11)):raise RuntimeError('Incomplete A1-A10 evidence')
    print('PASS full wxApp A1-A10; '+str(len(report['checks']))+' assertions')
    return 0
if __name__=='__main__':raise SystemExit(main())
