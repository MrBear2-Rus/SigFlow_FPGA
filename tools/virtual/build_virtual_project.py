"""Production virtual-project pipeline. No dependency on test harnesses.

stdin/stdout are not a per-cycle transport. The UI runs this bounded build job
in a child process, reads result.json, then launches the generated runner.
"""
import argparse,contextlib,hashlib,json,os,shutil,subprocess,sys,time,uuid
from pathlib import Path

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def read(path):return json.loads(Path(path).read_text(encoding='utf-8-sig'))
def write(path,value):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    temp=path.with_name(path.name+'.tmp-'+uuid.uuid4().hex)
    temp.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');os.replace(temp,path)
def absolute(value,base):
    path=Path(value);return (path if path.is_absolute() else base/path).resolve()
@contextlib.contextmanager
def exclusive(path):
    path.parent.mkdir(parents=True,exist_ok=True)
    with path.open('a+b') as lock:
        lock.seek(0,2)
        if lock.tell()==0:lock.write(b'0');lock.flush()
        until=time.monotonic()+180
        while True:
            try:
                lock.seek(0)
                if os.name=='nt':
                    import msvcrt;msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1)
                else:
                    import fcntl;fcntl.flock(lock.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
                break
            except OSError:
                if time.monotonic()>until:raise RuntimeError('E_BUILD_CACHE_BUSY')
                time.sleep(.1)
        try:yield
        finally:
            lock.seek(0)
            if os.name=='nt':msvcrt.locking(lock.fileno(),msvcrt.LK_UNLCK,1)
            else:fcntl.flock(lock.fileno(),fcntl.LOCK_UN)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--request',type=Path,required=True);parser.add_argument('--result',type=Path,required=True);args=parser.parse_args()
    result=dict(schema='sigflow.virtual-build-result.v1',ok=False,diagnostics=[],commands=[],model_started=False,cache_hit=False)
    started=time.monotonic()
    try:
        request=read(args.request);repo=Path(request['repo_root']).resolve();build=absolute(request.get('build_dir','build-m0'),repo)
        project=absolute(request['project_root'],repo)
        if request.get('schema')!='sigflow.virtual-project.v1':raise RuntimeError('E_VIRTUAL_PROJECT_SCHEMA')
        files=[absolute(p,project) for p in request['source_files']]
        if not files or len(files)>128 or len(set(files))!=len(files):raise RuntimeError('E_PROJECT_SOURCES')
        cst=absolute(request['cst_path'],project);board=absolute(request.get('board_path','registry/boards/tang-nano-9k.virtual.json'),repo)
        electrical=absolute(request.get('electrical_profile_path','registry/electrical/tang-nano-9k.6202.reviewed.v1.json'),repo)
        tool=request.get('toolchain',{})
        def find(name,default):
            value=tool.get(name) or default
            if not value or not Path(value).is_file():raise RuntimeError('E_TOOL_NOT_FOUND: '+name)
            return Path(value).resolve()
        cxx=find('cxx',shutil.which('g++') or 'E:/sigflow_fpga/toolchain/mingw64/bin/g++.exe')
        make=find('make',cxx.parent/'mingw32-make.exe' if os.name=='nt' else shutil.which('make'))
        shell=find('sh',os.environ.get('SIGFLOW_SH') or shutil.which('sh') or 'E:/sigflow_fpga/toolchain/posix/sh.exe')
        verilator=find('verilator',repo/'tools/verilator/verilator-install/bin/verilator_bin_dbg.exe')
        yosys=find('yosys',repo/'external/fpga-tools/runtime/yosys/bin/yosys.exe')
        validator=find('validator',build/'sigflow_session_validate.exe');codegen=find('codegen',build/'sigflow_adapter_codegen.exe');manifest=find('manifest',build/'sigflow_port_manifest.exe')
        tools={'cxx':cxx,'make':make,'sh':shell,'verilator':verilator,'yosys':yosys,'validator':validator,'codegen':codegen,'manifest':manifest,'python':Path(sys.executable)}
        libraries=[build/'core/libeda_virtual_runner.a',build/'InnerPlugin/eda-cst-gowin-cst/libeda-cst-gowin-cst.a',build/'core/libeda_virtual_board.a',build/'core/libeda_platform.a']
        dependencies=files+[cst,board,electrical,repo/'tools/virtual/export_ports.py',Path(__file__),*tools.values(),*libraries]
        dependencies+=list((repo/'include/eda/api').glob('*.hpp'))+list((repo/'core/src/eda-virtual-board').glob('*.h'))
        dependencies += [p for p in (verilator.parent.parent/'include').rglob('*') if p.is_file() and p.suffix in {'.h','.cpp','.mk'}]
        # Conservatively include all HDL headers in declared include directories.
        include_dirs=[absolute(p,project) for p in request.get('include_dirs',[])]
        for folder in [project,*include_dirs]:
            dependencies+=list(folder.rglob('*.vh'))+list(folder.rglob('*.svh'))
        fingerprint={p.as_posix():sha(p) for p in sorted(set(dependencies))}
        semantic={k:v for k,v in request.items() if k not in {'cache_root','run_id','generation','durability','journal_path','build_dir'}}
        flags=dict(cxx_standard='c++20',defines=['VL_USER_FATAL'],converge_limit=256,include_dirs=[p.as_posix() for p in include_dirs])
        key=hashlib.sha256(json.dumps(dict(inputs=fingerprint,request=semantic,flags=flags),sort_keys=True,ensure_ascii=False).encode()).hexdigest()
        root=absolute(request.get('cache_root','.work/virtual-project-cache'),repo);cache=root/key;cache.mkdir(parents=True,exist_ok=True)
        run_id='virtual-'+uuid.uuid4().hex;session_dir=repo/'.work/virtual-project-sessions'/run_id;session_dir.mkdir(parents=True,exist_ok=True)
        config={k:request[k] for k in ['clock','inputs','allow_unbound_outputs','max_delta','max_commits','phase1','phase2','integration_window_ticks','fault_profile','teaching'] if k in request}
        if 'fault_profile_path' in request:
            fault_path=absolute(request['fault_profile_path'],project);config['fault_profile']=read(fault_path)
        card=read(board);config.update(schema='sigflow.virtual-session.v1',run_id=run_id,generation='1',
            identity=dict(device=card['device'],package=card['package'],board_revision=card['hardware_revision']),
            cst_path=cst.as_posix(),board_path=board.as_posix(),electrical_profile_path=electrical.as_posix(),
            durability=request.get('durability','memory'),journal_path=(session_dir/'journal.jsonl').as_posix())
        if config['durability'] not in ['memory','durable']:raise RuntimeError('E_JOURNAL_CONFIG')
        config_file=session_dir/'session.json';write(config_file,config)
        env=dict(os.environ);env['PATH']=os.pathsep.join([str(cxx.parent),str(shell.parent),env.get('PATH','')]);env['VERILATOR_ROOT']=verilator.parent.parent.as_posix()
        env['SIGFLOW_PYTHON3']=sys.executable;env['SIGFLOW_SH']=str(shell)
        # Expose native uname for GNU make without modifying the tool installation.
        posix=cache/'posix';posix.mkdir(exist_ok=True)
        if (shell.parent/'busybox64.exe').exists():shutil.copy2(shell.parent/'busybox64.exe',posix/'uname.exe')
        env['PATH']=str(posix)+os.pathsep+env['PATH']
        def run(argv,stage,timeout=240,required=True):
            argv=[x.as_posix() if isinstance(x,Path) else str(x) for x in argv]
            print(json.dumps({'stage':stage}),flush=True);begin=time.monotonic()
            done=subprocess.run([str(x) for x in argv],cwd=repo,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=timeout)
            log=session_dir/(stage+'.log');log.write_text(done.stdout+done.stderr,encoding='utf-8')
            result['commands'].append(dict(stage=stage,argv=[str(x) for x in argv],exit_code=done.returncode,seconds=round(time.monotonic()-begin,3),log=str(log)))
            if required and done.returncode:raise RuntimeError('E_BUILD_'+stage.upper()+': '+str(log))
            return done
        with exclusive(cache/'build.lock'):
            exported=cache/'declarations.json';sidecar=Path(str(exported)+'.ports.v2.json');exe=cache/'runner.exe'
            complete=cache/'complete.json';entry=read(complete) if complete.exists() else {}
            valid=bool(entry) and all((cache/p).is_file() and sha(cache/p)==digest for p,digest in entry.get('artifacts',{}).items()) and len(entry.get('artifacts',{}))>=3
            if not valid:
                complete.unlink(missing_ok=True)
                run([sys.executable,repo/'tools/virtual/export_ports.py','--top',request['top_module'],'--output',exported,'--builder',manifest,'--yosys',yosys,'--verilator',verilator,*files],'export')
            validation=run([validator,sidecar,config_file],'validate',required=False)
            try:gate=json.loads(validation.stdout.strip().splitlines()[-1])
            except Exception:raise RuntimeError('E_VALIDATOR_RESPONSE')
            result['validation']=gate;result['diagnostics']=gate.get('diagnostics',[])
            if validation.returncode or not gate.get('ok'):write(args.result,result);return 2
            if not valid:
                generated=cache/'generated_adapter.cpp';generated_result=run([codegen,sidecar,'V'+request['top_module'],generated],'generate')
                definitions=json.loads(generated_result.stdout)['compile_definitions']
                includes=[repo/p for p in ['include','3rd','core/src','InnerPlugin/eda-cst-gowin-cst/include','InnerPlugin/eda-target-tangnano9k/include']]
                cppflags='-std=c++20 '+' '.join('-D'+d for d in definitions)+' '+' '.join('-I"'+p.as_posix()+'"' for p in includes)
                obj=cache/'obj';vflags=['-I'+p.as_posix() for p in include_dirs]
                run([verilator,'--cc',*files,'--exe',generated,'--top-module',request['top_module'],'--Mdir',obj,'--converge-limit','256','-Wno-fatal',*vflags,'-CFLAGS',cppflags,'-LDFLAGS',' '.join(p.as_posix() for p in libraries),'-o',exe],'verilate')
                exe.unlink(missing_ok=True)
                run([make,'-C',obj,'-f','V'+request['top_module']+'.mk','-j4',f'CXX={cxx.as_posix()}',f'LINK={cxx.as_posix()}',f'AR={(cxx.parent/"ar.exe").as_posix()}',f'SHELL={shell.as_posix()}',f'PYTHON3={Path(sys.executable).as_posix()}'],'compile')
                if any(sha(p)!=digest for p,digest in fingerprint.items()):raise RuntimeError('E_BUILD_INPUT_CHANGED: 构建期间输入发生变化，请重新构建')
                artifacts={p.name:sha(p) for p in [exe,sidecar,generated]}
                write(complete,dict(key=key,artifacts=artifacts,inputs=fingerprint,flags=flags))
            else:result['cache_hit']=True
        if any(sha(p)!=digest for p,digest in fingerprint.items()):raise RuntimeError('E_BUILD_INPUT_CHANGED')
        result.update(ok=True,cache_key=key,runner_path=str(exe),runner_sha256=sha(exe),session_config=str(config_file),working_directory=str(repo),
            project_root=str(project),toolchain_sha256={k:sha(v) for k,v in tools.items()},input_sha256=fingerprint)
        return 0
    except Exception as error:
        result['diagnostics'].append(dict(code=str(error).split(':')[0] if str(error).startswith('E_') else 'E_PROJECT_BUILD',message=str(error),port_ref='',package_pin=None,bank_id='',vccio_mv=None,suggestion='检查实验配置、构建日志和已安装工具链。'))
        return 1
    finally:
        result['seconds']=round(time.monotonic()-started,3);write(args.result,result)
if __name__=='__main__':raise SystemExit(main())
