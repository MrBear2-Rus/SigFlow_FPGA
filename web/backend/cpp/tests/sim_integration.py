"""Production sim.build and sim.run with a controlled compiler and executable."""
import hashlib
import json
import os
import select
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

binary, build = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="sim-", dir=build) as temp:
    root = Path(temp) / "project space"
    root.mkdir()
    source = root / "design.v"
    source.write_text("module top(input clk); endmodule\n")
    tb = root / "tb.v"
    tb.write_text("""module tb;
reg clk;
top dut(.clk(clk));
initial begin
clk = 0;
forever #5 clk = ~clk;
end
endmodule
""")
    executable_code = """#!/usr/bin/python3
import os,sys,time
from pathlib import Path
print('simulation running',flush=True)
mode=os.environ.get('SIM_RUN_MODE','success')
if mode=='timeout': time.sleep(30)
if mode=='fail': sys.exit(21)
if mode=='missing': sys.exit(0)
if mode=='empty': Path('waveform/wave.vcd').touch();sys.exit(0)
if len(sys.argv)>1:
    assert sys.argv[1:] == ['--sigflow-duration-ticks','20']
Path('waveform/wave.vcd').write_text('$timescale 1ns $end\\n#0\\n')
"""
    tool = root / "fake verilator"
    tool.write_text("""#!/usr/bin/python3
import os,sys,time
from pathlib import Path
args=sys.argv[1:]
assert args[:6]==['--cc','--exe','--build','--trace','-Wno-fatal','--top-module']
assert args[6]=='top'
harness=Path(args[-3]).read_text()
assert '#include "Vtop.h"' in harness and 'waveform/wave.vcd' in harness
if os.environ.get('EXPECT_CLOCK'): assert 'top->clk' in harness
print('building simulation',flush=True)
mode=os.environ.get('SIM_BUILD_MODE','success')
if mode=='timeout': time.sleep(30)
if mode=='fail': sys.exit(13)
if mode=='missing': sys.exit(0)
out=Path(args[-1])
out.write_text(""" + repr(executable_code) + """)
if mode!='nonexec': out.chmod(0o700)
""")
    tool.chmod(0o700)
    counter = 0
    def prepare(kind, payload, extra=(), project_id='p1'):
        global counter
        counter += 1
        job = f'job-{counter}'
        work = root / '.sigflow/agent/jobs' / kind / job
        work.mkdir(parents=True)
        args = [binary, 'run', '--job-type', kind, '--work-dir', str(work), '--job-id', job,
                '--project', str(root), '--project-id', project_id, '--params', '-', *extra]
        return work, args, json.dumps(payload)
    def run(kind, payload, env=None, extra=(), project_id='p1'):
        work,args,data = prepare(kind, payload, extra, project_id)
        environment = dict(os.environ, SIGFLOW_VERILATOR=str(tool))
        environment.update(env or {})
        result = subprocess.run(args, input=data, text=True, capture_output=True, env=environment, timeout=12)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        assert sum(e['type']=='result' for e in events)==1, result.stdout
        report = json.loads((work/'reports/report.json').read_text())
        return result, report, work
    build_params = {'top_module':'top', 'source_files':[str(source)]}
    result, report, build_work = run('sim.build', build_params)
    assert result.returncode == 0, (result.stdout,result.stderr)
    artifact = report['artifacts'][0]
    assert artifact['id']=='sim-executable' and artifact['path']=='artifacts/sim_main'
    sim_exe = build_work/artifact['path']
    assert artifact['sha256']==hashlib.sha256(sim_exe.read_bytes()).hexdigest()
    result, report, _ = run('sim.build', {'top_module':'top', 'source_files':[str(source),str(tb)], 'testbench':'tb'}, {'EXPECT_CLOCK':'1'})
    assert result.returncode==0, result.stdout
    result, report, run_work = run('sim.run', {'sim_exe':str(sim_exe), 'duration_ticks':'20', 'build_input_fingerprint':'abc'})
    assert result.returncode == 0, (result.stdout,result.stderr)
    assert report['artifacts'][0]['path']=='artifacts/waveform/wave.vcd'
    assert {'duration_ticks':'20'} in report['metrics'] and {'build_input_fingerprint':'abc'} in report['metrics']
    for mode in ('fail','missing'):
        result, report, _ = run('sim.build', build_params, {'SIM_BUILD_MODE':mode})
        assert result.returncode==4 and report['state']=='Failed' and not report['artifacts'], (mode,result.stdout)
    result, report, _ = run('sim.build', build_params, {'SIM_BUILD_MODE':'timeout'}, ['--timeout-sec','1'])
    assert result.returncode==4 and report['state']=='TimedOut'
    outside=Path(temp)/'outside.v';outside.write_text('module top; endmodule')
    for params in ({'top_module':'bad;','source_files':[str(source)]},
                   {'top_module':'top','source_files':[str(outside)]},
                   {'top_module':'top','source_files':[str(source)],'testbench':'missing'},
                   {'top_module':'top','source_files':[str(source)],'executable':'../bad'},
                   {'top_module':'top','source_files':[str(source)],'out_dir':'../bad'}):
        result, report, _ = run('sim.build',params)
        assert result.returncode==4 and report['state']=='Failed' and not report['artifacts'], (params,result.stdout)
    for mode in ('fail','missing','empty'):
        result, report, _ = run('sim.run', {'sim_exe':str(sim_exe)}, {'SIM_RUN_MODE':mode})
        assert result.returncode==4 and report['state']=='Failed' and not report['artifacts'], (mode,result.stdout)
    result, report, _ = run('sim.run', {'sim_exe':str(sim_exe)}, {'SIM_RUN_MODE':'timeout'}, ['--timeout-sec','1'])
    assert result.returncode==4 and report['state']=='TimedOut'
    for params in ({'sim_exe':str(outside)}, {'sim_exe':str(sim_exe),'duration_ticks':'-1'},
                   {'sim_exe':str(sim_exe),'duration_ticks':'18446744073709551616'},
                   {'sim_exe':str(sim_exe),'working_dir':'../bad'},
                   {'sim_exe':str(sim_exe),'vcd_out':'../bad'}):
        result, report, _ = run('sim.run',params)
        assert result.returncode==4 and report['state']=='Failed' and not report['artifacts'], (params,result.stdout)
    result, report, _ = run('sim.run', {'sim_exe':str(sim_exe)}, project_id='other')
    assert result.returncode==4 and not report['artifacts']
    sim_exe.write_text(sim_exe.read_text()+'# altered\n')
    result, report, _ = run('sim.run', {'sim_exe':str(sim_exe)})
    assert result.returncode==4 and not report['artifacts']
    result, report, fresh = run('sim.build', build_params)
    sim_exe = fresh/report['artifacts'][0]['path']
    metadata = fresh/'reports/report.json'
    original = json.loads(metadata.read_text()); modified=dict(original); modified['state']='Failed'
    metadata.write_text(json.dumps(modified))
    result, report, _ = run('sim.run', {'sim_exe':str(sim_exe)})
    assert result.returncode==4 and not report['artifacts']
    modified = json.loads(json.dumps(original)); modified['artifacts'][0]['sha256']=''
    metadata.write_text(json.dumps(modified))
    result, report, _ = run('sim.run', {'sim_exe':str(sim_exe)})
    assert result.returncode==4 and not report['artifacts']
    metadata.write_text(json.dumps(original))
    work,args,data = prepare('sim.run', {'sim_exe':str(sim_exe)})
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True, env=dict(os.environ, SIM_RUN_MODE='timeout'))
    proc.stdin.write(data);proc.stdin.close();proc.stdin=None
    try:
        deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            ready,_,_=select.select([proc.stdout],[],[],.1)
            if ready and 'simulation running' in proc.stdout.readline(): break
            if proc.poll() is not None: raise AssertionError('simulation exited before cancel')
        else: raise AssertionError('simulation did not start')
        os.killpg(proc.pid,signal.SIGTERM)
        out,err=proc.communicate(timeout=8)
        assert proc.returncode==-signal.SIGTERM,(out,err)
        assert json.loads((work/'reports/report.json').read_text())['state']=='Cancelled'
    finally:
        if proc.poll() is None: os.killpg(proc.pid,signal.SIGKILL);proc.wait()
    print(f'Verilator simulation: {counter} fake-tool cases passed')
