"""Run the Phase 5 production widgets in the existing wxApp acceptance host."""
import argparse,hashlib,json,os,shutil,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
    sys.stdout.reconfigure(encoding='utf-8',errors='replace');sys.stderr.reconfigure(encoding='utf-8',errors='replace')
    p=argparse.ArgumentParser();p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--gui',type=Path,required=True);a=p.parse_args()
    work=ROOT/'.work/p5/gui';work.mkdir(parents=True,exist_ok=True);result=work/'gui-results.json';result.unlink(missing_ok=True)
    if os.name=='nt':
        # A normal Explorer launch must work with the host MinGW ahead of ours.
        hostile=dict(os.environ)
        paths=[part for part in hostile.get('PATH','').split(os.pathsep) if 'e:/sigflow_fpga' not in part.replace('\\','/').lower()]
        hostile['PATH']='C:/mingw64/bin;'+os.pathsep.join(paths)
        for key in ('CC','CXX','LINK','AR','MAKEFLAGS','SIGFLOW_SH','VERILATOR_ROOT'):
            hostile.pop(key,None)
        assert Path(shutil.which('g++',path=hostile['PATH'])).resolve()==Path('C:/mingw64/bin/g++.exe').resolve()
        request=json.loads((ROOT/'examples/virtual/counter/virtual.project.json').read_text(encoding='utf-8-sig'))
        request.update(repo_root=ROOT.as_posix(),project_root=(ROOT/'examples/virtual/counter').as_posix(),build_dir=a.build_dir.resolve().as_posix(),cache_root=(work/('explorer-cache-'+str(time.time_ns()))).as_posix())
        request_file=work/'explorer-request.json';request_file.write_text(json.dumps(request),encoding='utf-8')
        build_result=work/'explorer-build-result.json'
        completed=subprocess.run([sys.executable,str(ROOT/'tools/virtual/build_virtual_project.py'),'--request',str(request_file),'--result',str(build_result)],cwd=ROOT,env=hostile,capture_output=True,encoding='utf-8',errors='replace',timeout=600)
        (work/'explorer-build.log').write_text(completed.stdout+completed.stderr,encoding='utf-8')
        assert completed.returncode==0,completed.stdout+completed.stderr
        built=json.loads(build_result.read_text(encoding='utf-8-sig'))
        assert built['ok'] and not built['cache_hit']
        for key,path in [('cxx','E:/sigflow_fpga/toolchain/mingw64/bin/g++.exe'),('sh','E:/sigflow_fpga/toolchain/posix/sh.exe')]:
            assert built['toolchain_sha256'][key]==hashlib.sha256(Path(path).read_bytes()).hexdigest()
        verilate=next(command for command in built['commands'] if command['stage']=='verilate')
        assert verilate['argv'][verilate['argv'].index('-LDFLAGS')+1].endswith(' -lpthread')
        compile_command=next(command for command in built['commands'] if command['stage']=='compile')
        compile_log=Path(compile_command['log']).read_text(encoding='utf-8').replace('\\','/').lower()
        assert 'e:/sigflow_fpga/toolchain/mingw64/bin/g++.exe' in compile_log and 'c:/mingw64/bin/g++.exe' not in compile_log
    env=dict(os.environ,SIGFLOW_PYTHON3=sys.executable,SIGFLOW_REPOSITORY_ROOT=ROOT.as_posix())
    env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+str(a.build_dir.resolve())+';'+env.get('PATH','')
    r=subprocess.run([str(a.gui),str(ROOT),str(a.build_dir),'--phase5'],cwd=ROOT,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=1800)
    (work/'gui.log').write_text(r.stdout+r.stderr,encoding='utf-8');print(r.stdout+r.stderr)
    if r.returncode:return r.returncode
    report=json.loads(result.read_text(encoding='utf-8'))
    assert report['status']=='passed','GUI acceptance failed: '+str(report.get('error'))
    for name in ('full-showcase-light.png','full-showcase-dark.png','float-showcase.png'):
        assert (work/name).exists() and (work/name).stat().st_size>4000,'missing '+name
    print('PASS real wxApp Phase 5: '+str(report['check_count'])+' assertions');return 0
if __name__=='__main__':raise SystemExit(main())
