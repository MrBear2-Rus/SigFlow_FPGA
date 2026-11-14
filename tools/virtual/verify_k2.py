"""Compile actual RTL and exercise the production Core VirtualSession runner.

The explicit reference DUT adapter is a test harness; no GUI/LiveTrace/general
wrapper generator or durable crash recovery is claimed by this first K2 pass.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import time

ROOT=Path(__file__).resolve().parents[2]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--build-dir',type=Path,default=ROOT/'build-m0');args=p.parse_args()
    build=args.build_dir.resolve();work=ROOT/'.work/k2';work.mkdir(parents=True,exist_ok=True)
    mingw=Path('E:/sigflow_fpga/toolchain/mingw64');shell=Path('E:/sigflow_fpga/toolchain/posix/sh.exe')
    tools=work/'posix-tools';tools.mkdir(exist_ok=True);shutil.copy2(shell.parent/'busybox64.exe',tools/'uname.exe')
    vr=ROOT/'tools/verilator/verilator-install';verilator=vr/'bin/verilator_bin_dbg.exe'
    env=dict(os.environ);env['PATH']=f'{tools};{mingw}/bin;{shell.parent};'+env.get('PATH','');env['VERILATOR_ROOT']=vr.as_posix()
    commands=[];transcript=[];checks=[]
    def run(argv,name,timeout=240):
        cmd=[str(x) for x in argv];r=subprocess.run(cmd,cwd=ROOT,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=timeout)
        (work/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8');commands.append(dict(argv=cmd,exit_code=r.returncode))
        if r.returncode:raise RuntimeError(f'{name} failed; see {work}/{name}.log')
    def check(condition,name):
        if not condition:raise AssertionError(name)
        checks.append(name)
    report=dict(stage='K2-first-implementation',status='running',K2_closed=False,commands=commands,checks=checks)
    try:
        output=work/'declarations/ports.json'
        run([sys.executable,ROOT/'tools/virtual/export_ports.py','--top','k2_counter','--output',output,
             '--builder',build/'sigflow_port_manifest.exe',ROOT/'tests/virtual/k2/counter.sv'],'export')
        obj=work/'obj';exe=work/'k2-reference-runner.exe'
        libraries=[build/'InnerPlugin/eda-cst-gowin-cst/libeda-cst-gowin-cst.a',build/'core/libeda_virtual_board.a',build/'core/libeda_platform.a']
        run([verilator,'--cc',ROOT/'tests/virtual/k2/counter.sv','--exe',ROOT/'tests/virtual/k2/verilated_runner.cpp',
             '--top-module','k2_counter','--Mdir',obj,'-Wno-fatal','-CFLAGS',
             f'-std=c++20 -I{ROOT.as_posix()}/include -I{ROOT.as_posix()}/3rd -I{ROOT.as_posix()}/core/src -I{ROOT.as_posix()}/InnerPlugin/eda-cst-gowin-cst/include -I{ROOT.as_posix()}/InnerPlugin/eda-target-tangnano9k/include',
             '-LDFLAGS',' '.join(path.as_posix() for path in libraries),'-o',exe],'verilate')
        # Force relinking this executable: external Core libraries are not
        # prerequisites in the tool-generated makefile.
        if exe.exists():exe.unlink()
        run([mingw/'bin/mingw32-make.exe','-C',obj,'-f','Vk2_counter.mk','-j4',f'CXX={mingw.as_posix()}/bin/g++.exe',
             f'LINK={mingw.as_posix()}/bin/g++.exe',f'AR={mingw.as_posix()}/bin/ar.exe',f'SHELL={shell.as_posix()}',
             f'PYTHON3={Path(sys.executable).as_posix()}'],'compile')
        process=subprocess.Popen([str(exe),str(ROOT),str(output)+'.ports.v2.json'],cwd=ROOT,env=env,
                                 stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8',bufsize=1)
        lines=queue.Queue()
        def read():
            for line in process.stdout:lines.put(line)
            lines.put(None)
        threading.Thread(target=read,daemon=True).start()
        def send(op,command_id=None,**kwargs):
            command=dict(op=op,**kwargs)
            if command_id is not None:command['command_id']=command_id
            process.stdin.write(json.dumps(command)+'\n');process.stdin.flush()
            line=lines.get(timeout=20)
            if line is None:raise RuntimeError('runner terminated: '+process.stderr.read())
            result=json.loads(line);transcript.append(dict(request=command,response=result));return result
        try:
            initial=send('get');check(initial['eval_calls']=='1','guarded initialization evaluated actual RTL')
            before=initial
            send('set','r1',target='reset_p4',state=2);pending=send('get')
            check(pending['input_writes']==before['input_writes'] and pending['eval_calls']==before['eval_calls'],'Paused reset release does not write or eval')
            edge=send('step','s1');check(edge['status']=='applied' and edge['position']['tick']=='18519','Step reaches 27MHz first rising edge')
            send('set','c1',target='key_p3',state=13);send('set','c2',target='key_p3',state=2);send('step','s2')
            audit=send('journal');check(audit['acks']['c1']['status']=='superseded' and audit['acks']['c2']['status']=='applied','press-release-before-step coalesced in production journal')
            send('set','c3',target='key_p3',state=13);pressed=send('step','s3')
            count=sum((not pressed['outputs'][f'led_n[{i}]'])<<i for i in range(6))
            check(count==1,'actual RTL counter incremented through CST pad 3 and real LED bindings')
            send('set','c4',target='key_p3',state=2);send('step','s4')
            before=send('get');send('set','f1',target='reset_p4',state=44);blocked=send('step','s5')
            check(blocked['reason']=='UnknownInput' and blocked['input_writes']==before['input_writes'] and blocked['eval_calls']==before['eval_calls'],'CON asynchronous reset blocked before any DUT write')
            check(blocked['position']==before['position'],'UnknownInput preserves last committed position')
            send('set','fix',target='reset_p4',state=2);send('step','s6');audit=send('journal')
            replay=send('replay',records=audit['records']);check(replay['status']=='passed','actual Verilated model replay matches every committed output')
            corrupt=json.loads(json.dumps(audit['records']));corrupt[0]['source_sha256']='0'*64
            check(send('replay',records=corrupt)['status']=='rejected','replay rejects mismatched source fingerprint')
            corrupt=json.loads(json.dumps(audit['records']))
            first=next(record for record in corrupt if record['kind']=='commit')
            first['outputs']['led_n[0]']=not first['outputs']['led_n[0]']
            check(send('replay',records=corrupt)['status']=='rejected','replay rejects altered output evidence')
            before_play=send('get');send('play','p1');time.sleep(.02);paused=send('pause','p2');time.sleep(.02);frozen=send('get')
            check(int(paused['position']['tick'])>int(before_play['position']['tick']),'Play actually advances simulation time')
            check(paused['position']==frozen['position'] and paused['eval_calls']==frozen['eval_calls'],'Play/Pause freezes actual model after ACK')
            check(frozen['model_instances']==1,'one persistent Verilated model during live session')
            stopped=send('stop','x1');check(stopped['final_calls']==1,'Finalize exactly once')
            check(send('stop','x1')==stopped,'Stop request idempotent')
            final_audit=send('journal');(work/'committed-journal.json').write_text(json.dumps(final_audit,indent=2)+'\n',encoding='utf-8')
            process.stdin.close();check(process.wait(timeout=10)==0,'persistent runner exits cleanly')
        finally:
            if process.poll() is None:process.kill();process.wait(timeout=10)
            (work/'runner-stderr.log').write_text(process.stderr.read(),encoding='utf-8')
        report.update(status='passed',runner_sha256=sha(exe),verilator_sha256=sha(verilator),
                      adapter='explicit reference-fixture adapter; general generated adapter pending',
                      remaining=['durable journal/commit marker recovery','general adapter generator and job lifecycle integration',
                                 'LiveTrace append/read-view and GUI board integration','full resolver table and general event scheduler'])
        report['journal']=dict(record_count=len(final_audit['records']),sha256=sha(work/'committed-journal.json'),durability='memory')
    except Exception as e:
        report.update(status='failed',error=str(e));raise
    finally:
        evidence=ROOT/'docs/virtual/evidence';evidence.mkdir(parents=True,exist_ok=True)
        (work/'transcript.json').write_text(json.dumps(transcript,indent=2)+'\n',encoding='utf-8')
        (evidence/'K2-first-implementation-summary.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f'PASS: {len(checks)} actual Verilated Core session checks; K2 remains in progress.')
if __name__=='__main__':main()
