"""Launch the production-widget wxApp host in its independent Phase 1 mode."""
import argparse,json,os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
    p=argparse.ArgumentParser();p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--gui',type=Path,required=True);a=p.parse_args()
    work=ROOT/'.work/p1/gui';work.mkdir(parents=True,exist_ok=True);result=work/'gui-results.json';result.unlink(missing_ok=True)
    env=dict(os.environ,SIGFLOW_PYTHON3=sys.executable,SIGFLOW_REPOSITORY_ROOT=ROOT.as_posix());env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+env.get('PATH','')
    r=subprocess.run([str(a.gui),str(ROOT),str(a.build_dir),'--phase1'],cwd=ROOT,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=650)
    (work/'gui.log').write_text(r.stdout+r.stderr,encoding='utf-8');print(r.stdout+r.stderr)
    if r.returncode:return r.returncode
    report=json.loads(result.read_text(encoding='utf-8'))
    assert report['status']=='passed' and report['replay']['status']=='passed'
    print('PASS Phase 1 real wxApp: '+str(report['check_count'])+' assertions');return 0
if __name__=='__main__':raise SystemExit(main())
