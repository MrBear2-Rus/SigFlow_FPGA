"""Shared real-tool harness. All module C++ glue comes from the production generator."""
import hashlib,json,os,queue,shutil,subprocess,sys,threading
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
class Harness:
    def __init__(self,build,work):
        self.build=Path(build).resolve();self.work=Path(work).resolve();self.work.mkdir(parents=True,exist_ok=True)
        self.mingw=Path('E:/sigflow_fpga/toolchain/mingw64');self.shell=Path('E:/sigflow_fpga/toolchain/posix/sh.exe')
        posix=self.work/'posix-tools';posix.mkdir(exist_ok=True);shutil.copy2(self.shell.parent/'busybox64.exe',posix/'uname.exe')
        self.vroot=ROOT/'tools/verilator/verilator-install';self.verilator=self.vroot/'bin/verilator_bin_dbg.exe'
        self.env=dict(os.environ);self.env['PATH']=f'{posix};{self.mingw}/bin;{self.shell.parent};'+self.env.get('PATH','');self.env['VERILATOR_ROOT']=self.vroot.as_posix()
        self.commands=[]
    def run(self,args,name,timeout=240,required=True):
        args=[str(a) for a in args];r=subprocess.run(args,cwd=ROOT,env=self.env,capture_output=True,encoding='utf-8',errors='replace',timeout=timeout)
        (self.work/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8')
        self.commands.append(dict(argv=args,exit_code=r.returncode))
        if required and r.returncode:raise RuntimeError(f'{name} failed: {self.work}/{name}.log')
        return r
    def build_runner(self,source,top):
        folder=self.work/top;folder.mkdir(exist_ok=True);export=folder/'ports.json'
        self.run([sys.executable,ROOT/'tools/virtual/export_ports.py','--top',top,'--output',export,'--builder',self.build/'sigflow_port_manifest.exe',source],top+'-export')
        manifest=json.loads(Path(str(export)+'.ports.v2.json').read_text(encoding='utf-8'))['manifest']
        generated=folder/'generated_adapter.cpp';result=self.run([self.build/'sigflow_adapter_codegen.exe',str(export)+'.ports.v2.json','V'+top,generated],top+'-generate')
        definitions=json.loads(result.stdout)['compile_definitions'];assert 'VL_USER_FATAL' in definitions
        obj=folder/'obj';exe=folder/'runner.exe'
        includes=['include','3rd','core/src','InnerPlugin/eda-cst-gowin-cst/include','InnerPlugin/eda-target-tangnano9k/include']
        flags='-std=c++20 '+' '.join('-D'+d for d in definitions)+' '+' '.join('-I'+(ROOT/i).as_posix() for i in includes)
        libraries=[self.build/'core/libeda_virtual_runner.a',self.build/'InnerPlugin/eda-cst-gowin-cst/libeda-cst-gowin-cst.a',
                   self.build/'core/libeda_virtual_board.a',self.build/'core/libeda_platform.a']
        self.run([self.verilator,'--cc',source,'--exe',generated,'--top-module',top,'--Mdir',obj,'--converge-limit','256','-Wno-fatal',
                  '-CFLAGS',flags,'-LDFLAGS',' '.join(p.as_posix() for p in libraries),'-o',exe],top+'-verilate')
        if exe.exists():exe.unlink()
        self.run([self.mingw/'bin/mingw32-make.exe','-C',obj,'-f','V'+top+'.mk','-j4',f'CXX={self.mingw.as_posix()}/bin/g++.exe',
                  f'LINK={self.mingw.as_posix()}/bin/g++.exe',f'AR={self.mingw.as_posix()}/bin/ar.exe',f'SHELL={self.shell.as_posix()}',
                  f'PYTHON3={Path(sys.executable).as_posix()}'],top+'-compile')
        return exe,manifest
class Client:
    def __init__(self,harness,exe,config,synthetic=False):
        args=[str(exe),str(config)]+(['--allow-synthetic-test-profile'] if synthetic else [])
        self.process=subprocess.Popen(args,cwd=ROOT,env=harness.env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',bufsize=1)
        self.lines=queue.Queue();self.stderr=[];self.transcript=[]
        def read():
            for line in self.process.stdout:self.lines.put(line)
            self.lines.put(None)
        def errors():
            for line in self.process.stderr:self.stderr.append(line)
        threading.Thread(target=read,daemon=True).start();threading.Thread(target=errors,daemon=True).start()
    def send(self,op,command_id=None,**kwargs):
        command=dict(op=op,**kwargs)
        if command_id is not None:command['command_id']=command_id
        self.process.stdin.write(json.dumps(command)+'\n');self.process.stdin.flush()
        line=self.lines.get(timeout=20)
        if line is None:raise RuntimeError('runner terminated: '+''.join(self.stderr))
        result=json.loads(line);self.transcript.append(dict(request=command,response=result));return result
    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait(timeout=10)
        if self.process.returncode:raise RuntimeError('runner exit: '+''.join(self.stderr))
    def kill(self):self.process.kill();self.process.wait(timeout=10)
def write(path,data):Path(path).write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8');return Path(path)
def real_config(work,durable=True):
    journal=Path(work)/'counter.journal'
    if journal.exists():journal.unlink()
    return dict(schema='sigflow.virtual-session.v1',run_id='k2-counter-production',generation='1',
                identity=dict(device='GW1NR-LV9QN88PC6/I5',package='QFN88',board_revision='6202'),
                electrical_profile_path=(ROOT/'registry/electrical/tang-nano-9k.6202.reviewed.v1.json').as_posix(),
                cst_path=(ROOT/'tests/virtual/k2/counter.cst').as_posix(),
                clock=dict(package_pin=52,frequency_hz='27000000',enabled=True),durability='durable' if durable else 'memory',journal_path=journal.as_posix(),
                inputs=[dict(target='key_p3',package_pin=3,fixed_drivers=[dict(driver_id='board-pullup',state=7)],initial_manual_state=2),
                        dict(target='reset_p4',package_pin=4,fixed_drivers=[dict(driver_id='board-pullup',state=7)],initial_manual_state=13),
                        dict(target='clock_p52',package_pin=52,fixed_drivers=[],initial_manual_state=2)])
def synthetic_config(work,manifest,clock_name):
    """Explicit synthetic device only; never installed under production registry."""
    folder=Path(work);bindings=[];pads=[];inputs=[];pin=1;clock_pin=None
    for port in manifest['ports']:
        for bit in port['bits']:
            label=port['rtl_name']+('['+str(bit['hdl_index'])+']' if bit['hdl_index'] is not None else '')
            bindings.append(f'IO_LOC "{label}" {pin};\nIO_PORT "{label}" IO_TYPE=LVCMOS18 PULL_MODE=NONE;\n')
            pads.append(dict(pin=pin,reviewed=True,available=True,vendor_bank_id='TEST_BANK',allowed_io_types=['LVCMOS18']))
            if port['direction']=='input':inputs.append(dict(target=label,package_pin=pin,fixed_drivers=[],initial_manual_state=13))
            if port['rtl_name']==clock_name:clock_pin=pin
            pin+=1
    assert clock_pin is not None
    profile=dict(schema='sigflow.electrical.v1',synthetic=True,device='SYNTHETIC_TEST_FPGA',package='TEST_PACKAGE',vendor_package='TEST_PACKAGE',board_revision='test',
                 provenance=[],pads=pads,banks={'TEST_BANK':dict(vccio_mv=1800)},endpoints=[],
                 io_standards={'LVCMOS18':dict(directions=['input','output'],vccio_allowed_mv=[1800],drive_allowed_ma=[4],pull_modes=['none'])})
    profile_path=write(folder/'synthetic-profile.json',profile);cst=folder/'generated.cst';cst.write_text(''.join(bindings),encoding='utf-8')
    return dict(schema='sigflow.virtual-session.v1',run_id=manifest['top_module']+'-synthetic-test',generation='1',
                identity=dict(device=profile['device'],package=profile['package'],board_revision='test'),electrical_profile_path=profile_path.as_posix(),
                cst_path=cst.as_posix(),clock=dict(package_pin=clock_pin,frequency_hz='27000000',enabled=True),durability='memory',inputs=inputs)
