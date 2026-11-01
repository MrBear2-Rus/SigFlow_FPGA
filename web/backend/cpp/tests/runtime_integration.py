"""Exercise the actual CLI and Runtime, with a provider compiled only in tests."""
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

fixture, production, build, contracts = sys.argv[1:]
try:
    from jsonschema import Draft7Validator
except ImportError:
    Draft7Validator = None
schemas = {}
if Draft7Validator:
    for kind in ("request", "report", "manifest", "event"):
        schemas[kind] = Draft7Validator(json.loads((Path(contracts)/(kind+".schema.json")).read_text()))

def events(data):
    values=[json.loads(line) for line in data.splitlines()]
    if schemas:
        for value in values: schemas["event"].validate(value)
    return values

def alive(pid):
    try: return Path(f"/proc/{pid}/stat").read_text().split()[2] != "Z"
    except (FileNotFoundError, ProcessLookupError): return False

def wait_until(predicate, limit=5):
    deadline=time.monotonic()+limit
    while time.monotonic()<deadline:
        if predicate(): return
        time.sleep(.02)
    raise AssertionError("deadline exceeded")

with tempfile.TemporaryDirectory(prefix="runtime-",dir=build) as directory:
    project=Path(directory)/"工程 with spaces"; project.mkdir()
    sequence=0; checks=0
    def setup(mode="success", extra=()):
        global sequence
        sequence+=1; job=f"job-{sequence}"
        work=project/".sigflow/agent/jobs/synth"/job; work.mkdir(parents=True)
        args=["run","--job-type","synth","--work-dir",str(work),"--job-id",job,
              "--project",str(project),"--project-id","project-001","--params","-",*extra]
        return work,args,json.dumps({"mode":mode})
    def execute(mode="success",extra=()):
        global checks
        work,args,data=setup(mode,extra)
        result=subprocess.run([fixture,*args],input=data,text=True,capture_output=True,timeout=8)
        output=events(result.stdout)
        checks+=1
        report_path=work/"reports/report.json"
        report=json.loads(report_path.read_text()) if report_path.is_file() else None
        if report and schemas:
            for kind,path in [("report",report_path),("manifest",work/"manifest.json"),("request",work/"request.json")]:
                schemas[kind].validate(json.loads(path.read_text()))
        return result,output,report,work,args

    result,output,report,work,args=execute()
    assert result.returncode==0,(result.stderr,result.stdout)
    assert report["state"]=="Succeeded" and report["exit_code"]==0
    assert sum(e["type"]=="result" for e in output)==1 and output[-1]["type"]=="result"
    artifact=report["artifacts"][0]
    assert artifact["path"]=="artifacts/result.txt"
    assert artifact["sha256"]==hashlib.sha256((work/artifact["path"]).read_bytes()).hexdigest()
    assert (work/"logs/tool.log").is_file()
    assert json.loads((work/"request.json").read_text())["parameters"]=={"mode":"success"}
    original=(work/"reports/report.json").read_bytes()
    repeat=subprocess.run([fixture,*args],input='{}',text=True,capture_output=True,timeout=5)
    assert repeat.returncode==2 and original==(work/"reports/report.json").read_bytes()
    checks+=1

    for mode in ["nonzero","launch","exception","artifact-escape","symlink","digest","duplicate","mutate","bad-cwd","input-escape","bad-progress"]:
        result,output,report,work,args=execute(mode)
        assert result.returncode==4,(mode,result.returncode,result.stderr,result.stdout)
        assert report["state"]=="Failed" and report["artifacts"]==[],(mode,report)
        assert sum(e["type"]=="result" for e in output)==1
        if mode=="nonzero": assert report["exit_code"]==17
        if mode=="launch": assert report["exit_code"]==-1
    result,output,report,work,args=execute("bad-report")
    assert result.returncode==4 and not any(e["type"]=="result" for e in output)
    assert json.loads((work/"manifest.json").read_text())["state"]!="Succeeded"
    result,output,report,work,args=execute("long",["--max-output-bytes","31"])
    assert result.returncode==0 and (work/"logs/tool.log").stat().st_size==31
    assert {"output_truncated":True} in report["metrics"]
    result,output,report,work,args=execute("empty")
    assert result.returncode==0 and report["artifacts"]==[]
    result,output,report,work,args=execute("sleep",["--timeout-sec","1"])
    assert result.returncode==4 and report["state"]=="TimedOut"
    pids=[int(x) for x in (work/"tool.pid").read_text().split()]
    wait_until(lambda: not any(alive(pid) for pid in pids))

    started=time.monotonic()
    result,output,report,work,args=execute("multi",["--timeout-sec","1"])
    assert result.returncode==4 and report["state"]=="TimedOut" and time.monotonic()-started<3

    bad_extras=[["--timeout-sec","-1"],["--timeout-sec","2147483648"],
                ["--max-output-bytes","18446744073709551616"],["--mystery","x"],
                ["--job-id","duplicate"],["--timeout-sec"]]
    for extra in bad_extras:
        work,args,data=setup(extra=extra)
        result=subprocess.run([fixture,*args],input=data,text=True,capture_output=True,timeout=5)
        assert result.returncode==2,(extra,result.stdout)
        assert not list(work.iterdir()); events(result.stdout); checks+=1
    for data in ['[]','{','{} {}','{"mode":'+(' '*2100000)+'"success"}']:
        work,args,_=setup()
        result=subprocess.run([fixture,*args],input=data,text=True,capture_output=True,timeout=5)
        assert result.returncode==2 and not list(work.iterdir()); checks+=1
    work,args,data=setup(); bad=args.copy(); bad[bad.index("--job-type")+1]="test.exec"
    result=subprocess.run([production,*bad],input=data,text=True,capture_output=True,timeout=5)
    assert result.returncode==2; checks+=1
    work,args,data=setup(); args[args.index("--job-type")+1]="pnr"
    work=project/".sigflow/agent/jobs/pnr"/args[args.index("--job-id")+1];work.mkdir(parents=True)
    args[args.index("--work-dir")+1]=str(work)
    result=subprocess.run([production,*args],input=data,text=True,capture_output=True,timeout=5)
    assert result.returncode==3 and not list(work.iterdir()); events(result.stdout); checks+=1
    for key,value in [("--job-id","../escape"),("--work-dir",str(project.parent)),("--project-id","")]:
        work,args,data=setup();bad=args.copy();bad[bad.index(key)+1]=value
        result=subprocess.run([fixture,*bad],input=data,text=True,capture_output=True,timeout=5)
        assert result.returncode==2 and not list(work.iterdir());checks+=1
    work,args,data=setup();bad=args.copy();index=bad.index("--project-id");del bad[index:index+2]
    result=subprocess.run([fixture,*bad],input=data,text=True,capture_output=True,timeout=5)
    assert result.returncode==2 and not list(work.iterdir());checks+=1
    # A symlink for the Job directory must be rejected before any write.
    work,args,data=setup(); linked=work.parent/"linked"; linked.symlink_to(work,target_is_directory=True)
    bad=args.copy(); bad[bad.index("--work-dir")+1]=str(linked)
    result=subprocess.run([fixture,*bad],input=data,text=True,capture_output=True,timeout=5)
    assert result.returncode==2 and not list(work.iterdir()); checks+=1

    # Real subprocesses: Node-style detached CLI, tools in a separate process group.
    for action in ("term","kill","pipe"):
        work,args,data=setup("sleep")
        proc=subprocess.Popen([fixture,*args],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                              text=True,start_new_session=True)
        proc.stdin.write(data);proc.stdin.close();proc.stdin=None
        try:
            wait_until(lambda:(work/"tool.pid").exists() and len((work/"tool.pid").read_text().split())==2)
            pids=[int(x) for x in (work/"tool.pid").read_text().split()]
            if action=="pipe":
                proc.stdout.close();proc.stdout=None
            else: os.killpg(proc.pid,signal.SIGTERM if action=="term" else signal.SIGKILL)
            out,err=proc.communicate(timeout=7)
            wait_until(lambda:not any(alive(pid) for pid in pids))
            if action=="term":
                assert proc.returncode==-signal.SIGTERM,(proc.returncode,out,err)
                report=json.loads((work/"reports/report.json").read_text())
                assert report["state"]=="Cancelled"; events(out)
            elif action=="kill":
                assert proc.returncode==-signal.SIGKILL
                assert not (work/"reports/report.json").exists()
            else: assert proc.returncode==4
            checks+=1
        finally:
            if proc.poll() is None: proc.kill();proc.wait()
            if (work/"tool.pid").exists():
                for pid in map(int,(work/"tool.pid").read_text().split()):
                    if alive(pid): os.kill(pid,signal.SIGKILL)
    # Normal cancellation must also interrupt an incomplete stdin JSON read.
    work,args,data=setup()
    proc=subprocess.Popen([fixture,*args],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
    proc.stdin.write('{');proc.stdin.flush()
    time.sleep(.1);os.killpg(proc.pid,signal.SIGTERM)
    try:
        proc.wait(timeout=3)
        assert proc.returncode==-signal.SIGTERM and not list(work.iterdir())
        checks+=1
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        proc.stdin.close();proc.stdout.close();proc.stderr.close()
    # A live but non-reading stdout consumer must not hang a Job indefinitely.
    work,args,data=setup("long")
    proc=subprocess.Popen([fixture,*args],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    proc.stdin.write(data);proc.stdin.close();proc.stdin=None
    try:
        proc.wait(timeout=7)
        out,err=proc.communicate(timeout=2)
        assert proc.returncode==4 and '"type":"result"' not in out
        checks+=1
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
    # Two clients must not execute one Job concurrently.
    work,args,data=setup("sleep")
    proc=subprocess.Popen([fixture,*args],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
    proc.stdin.write(data);proc.stdin.close();proc.stdin=None
    try:
        wait_until(lambda:(work/"tool.pid").exists())
        duplicate=subprocess.run([fixture,*args],input=data,text=True,capture_output=True,timeout=5)
        assert duplicate.returncode in (2,4)
        os.killpg(proc.pid,signal.SIGTERM);proc.communicate(timeout=6)
        checks+=1
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
    # Provider sees selected CLI settings and can only read authorized input roots.
    source=project/"source.sv";source.write_text("module top; endmodule\n")
    outside=project.parent/"outside.sv";outside.write_text("outside\n")
    static=project.parent/"static";static.mkdir();(static/"debug.sv").write_text("module debug; endmodule\n")
    link=project/"link.sv";link.symlink_to(outside)
    for input_path,expected in [(source,0),(outside,4),(link,4),(project/"missing.sv",4),(static/"debug.sv",0)]:
        work,args,_=setup(extra=["--data-dir",str(static),"--tool-root",str(project/"tools")])
        result=subprocess.run([fixture,*args],input=json.dumps({"input":str(input_path)}),text=True,capture_output=True,timeout=5)
        assert result.returncode==expected,(input_path,result.stdout)
        events(result.stdout)
        report=json.loads((work/"reports/report.json").read_text())
        assert {"data_dir":str(static),"tool_root":str(project/"tools")} in report["metrics"]
        checks+=1
    work,args,data=setup()
    environment=dict(os.environ,SIGFLOW_DATA_DIR=str(static),SIGFLOW_TOOL_RUNTIME_DIR=str(project/"env-tools"))
    result=subprocess.run([fixture,*args],input=data,text=True,capture_output=True,env=environment,timeout=5)
    assert result.returncode==0
    request=json.loads((work/"request.json").read_text())
    assert request["data_dir"]==str(static) and request["tool_root"]==str(project/"env-tools")
    checks+=1
    explicit=project/"explicit-tools";runtime=project/"runtime-tools"
    for root in (explicit,runtime):
        (root/"bin").mkdir(parents=True);tool=root/"bin/yosys";tool.write_text("#!/bin/sh\nexit 0\n");tool.chmod(0o700)
    env_tool=project/"env-yosys";env_tool.write_text("#!/bin/sh\nexit 0\n");env_tool.chmod(0o700)
    for extra,use_env,expected in [(["--tool-root",str(explicit)],True,explicit/"bin/yosys"),([],True,env_tool),([],False,runtime/"bin/yosys")]:
        work,args,_=setup(extra=extra)
        environment=dict(os.environ,SIGFLOW_TOOL_RUNTIME_DIR=str(runtime))
        if use_env:environment["SIGFLOW_YOSYS"]=str(env_tool)
        else:environment.pop("SIGFLOW_YOSYS",None)
        result=subprocess.run([fixture,*args],input=json.dumps({"resolve_tool":"yosys"}),text=True,capture_output=True,env=environment,timeout=5)
        assert result.returncode==0,result.stdout
        report=json.loads((work/"reports/report.json").read_text())
        assert {"resolved_tool":str(expected)} in report["metrics"]
        checks+=1
    # --params file path and relative project/work-dir resolution.
    work,args,data=setup(); params=project/"params.json";params.write_text(data)
    args[args.index("--params")+1]=str(params)
    for key in ("--project","--work-dir"):
        index=args.index(key)+1;args[index]=os.path.relpath(args[index],build)
    result=subprocess.run([fixture,*args],cwd=build,text=True,capture_output=True,timeout=5)
    assert result.returncode==0,(result.stderr,result.stdout);checks+=1
    print(f"Runtime CLI integration: {checks} cases passed; schema validation={bool(schemas)}")
