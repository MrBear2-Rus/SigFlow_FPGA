"""Exercise production pnr with a controlled stand-in tool."""
import hashlib
import json
import os
import subprocess
import signal
import select
import time
import sys
import tempfile
from pathlib import Path
try:
    from jsonschema import Draft7Validator
except ImportError:
    Draft7Validator = None
schema_path = Path(__file__).resolve().parents[1] / "contracts/run-v1/nextpnr-metrics.schema.json"
schema = json.loads(schema_path.read_text())
if Draft7Validator:
    Draft7Validator.check_schema(schema)

binary, build = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="pnr-", dir=build) as tmp:
    root = Path(tmp) / "工程 space"
    root.mkdir()
    netlist = root / "synth netlist.json"
    netlist.write_text(json.dumps({"modules": {"top": {"ports": {}, "cells": {}, "netnames": {}}}}))
    cst = root / "pins.cst"
    cst.write_text('IO_LOC "a" 10;\n')
    tool = root / "nextpnr fake"
    tool.write_text("""#!/usr/bin/python3
import json,os,sys,time
from pathlib import Path
args=sys.argv[1:]
assert Path(args[args.index('--json')+1]).is_file()
assert args[args.index('--device')+1]=='GW1NR-LV9QN88PC6/I5'
assert 'family=GW1N-9C' in args
if os.environ.get('EXPECT_CST'):assert 'cst='+os.environ['EXPECT_CST'] in args
output=Path(args[args.index('--write')+1])
mode=os.environ.get('FAKE_PNR_MODE','success')
print('Info: Version 0.7', file=sys.stderr, flush=True)
print('Info: Packing IOBs..', file=sys.stderr, flush=True)
print('Info: Placing...', file=sys.stderr, flush=True)
print('placing and routing',flush=True)
print('IOB: 5/276 1%', file=sys.stderr, flush=True)
print("Info: Max frequency for clock 'clk': 62.64 MHz (PASS at 27.00 MHz)", file=sys.stderr, flush=True)
print('Warning: sample warning', file=sys.stderr, flush=True)
if mode=='badlog': print('IOB: 9999999999999999999999999/1 1%', file=sys.stderr, flush=True)
if mode=='invalidutf': sys.stderr.buffer.write(b'Error: unknown '+bytes((255,10)));sys.stderr.flush()
if mode=='nonzero': print("ERROR: Failed to open CST file 'missing.cst': no such file or directory",file=sys.stderr,flush=True);sys.exit(19)
if mode=='missing':sys.exit(0)
if mode=='invalid':output.write_text('{invalid');sys.exit(0)
if mode=='wrongtop':output.write_text(json.dumps({'modules':{'other':{}}}));sys.exit(0)
if mode=='timeout':time.sleep(30)
if mode=='long':print('x'*100000)
if mode=='longline':print('x'*10000)
output.write_text(json.dumps({'modules':{'top':{'cells':{},'ports':{},'netnames':{}}}}))
""")
    tool.chmod(0o700)
    counter = 0

    def run(params=None, mode="success", tool_available=True, extra=()):
        global counter
        counter += 1
        job = f"job-{counter}"
        work = root / ".sigflow/agent/jobs/pnr" / job
        work.mkdir(parents=True)
        payload = {"top_module": "top", "netlist": str(netlist),
                   "device": "GW1NR-LV9QN88PC6/I5", "family": "GW1N-9C"}
        payload.update(params or {})
        env = dict(os.environ, FAKE_PNR_MODE=mode, PATH="/nonexistent")
        env.pop("SIGFLOW_NEXTPNR", None)
        env.pop("SIGFLOW_TOOL_RUNTIME_DIR", None)
        if tool_available:
            env["SIGFLOW_NEXTPNR"] = str(tool)
        if payload.get("cst"):
            env["EXPECT_CST"] = str(payload["cst"])
        cmd = [binary, "run", "--job-type", "pnr", "--work-dir", str(work), "--job-id", job,
               "--project", str(root), "--project-id", "p1", "--params", "-", *extra]
        result = subprocess.run(cmd, input=json.dumps(payload), text=True, capture_output=True,
                                env=env, timeout=12)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        report = json.loads((work / "reports/report.json").read_text())
        if Draft7Validator:
            for metric in report["metrics"]:
                if "nextpnr" in metric:
                    Draft7Validator(schema).validate(metric["nextpnr"])
        assert sum(event["type"] == "result" for event in events) == 1
        return result, report, work

    result, report, work = run({"cst": str(cst)})
    assert result.returncode == 0, (result.stdout, result.stderr)
    analysis = next(metric["nextpnr"] for metric in report["metrics"] if "nextpnr" in metric)
    assert analysis["parsed"] and not analysis["log_truncated"]
    assert analysis["version"] == "0.7" and all(analysis["stages_observed"].values())
    assert analysis["resources"]["IOB"]["used"] == 5
    assert analysis["timing"]["clock"] == "clk" and analysis["timing"]["passed"] is True
    assert analysis["warnings"] == 1
    artifact = report["artifacts"][0]
    assert artifact["id"] == "pnr-json" and artifact["path"] == "artifacts/top.pnr.json"
    assert hashlib.sha256((work / artifact["path"]).read_bytes()).hexdigest() == artifact["sha256"]
    result, report, work = run({"output": "nested/design.json"})
    assert result.returncode == 0 and report["artifacts"][0]["path"] == "artifacts/nested/design.json"
    for mode in ("nonzero", "missing", "invalid", "wrongtop"):
        result, report, _ = run(mode=mode)
        assert result.returncode == 4 and report["state"] == "Failed" and not report["artifacts"]
        if mode == "nonzero":
            assert report["exit_code"] == 19
            analysis = next(metric["nextpnr"] for metric in report["metrics"] if "nextpnr" in metric)
            assert analysis["errors"] >= 1 and analysis["classified_errors"][0]["detail"] == "missing.cst"
    result, report, _ = run(tool_available=False)
    assert result.returncode == 4 and report["exit_code"] == -1
    result, report, _ = run(mode="timeout", extra=("--timeout-sec", "1"))
    assert result.returncode == 4 and report["state"] == "TimedOut"
    result, report, work = run(mode="long", extra=("--max-output-bytes", "128"))
    assert result.returncode == 0 and {"output_truncated": True} in report["metrics"]
    assert (work / "logs/tool.log").stat().st_size == 128
    analysis = next(metric["nextpnr"] for metric in report["metrics"] if "nextpnr" in metric)
    assert analysis["log_truncated"]
    result, report, _ = run(mode="longline")
    analysis = next(metric["nextpnr"] for metric in report["metrics"] if "nextpnr" in metric)
    assert result.returncode == 0 and analysis["parsed"] and analysis["log_truncated"], (result.returncode, analysis, result.stdout[-1000:])
    result, report, _ = run(mode="badlog")
    analysis = next(metric["nextpnr"] for metric in report["metrics"] if "nextpnr" in metric)
    assert result.returncode == 0 and not analysis["parsed"] and "analysis_error" in analysis
    result, report, _ = run(mode="invalidutf")
    analysis = next(metric["nextpnr"] for metric in report["metrics"] if "nextpnr" in metric)
    assert result.returncode == 0 and analysis["parsed"] and analysis["errors"] >= 1
    outside = Path(tmp) / "outside.json"
    outside.write_text('{}')
    link = root / "linked.json"
    link.symlink_to(outside)
    for invalid in ({"netlist": str(outside)}, {"netlist": str(link)},
                    {"netlist": "missing.json"}, {"netlist": None},
                    {"top_module": "../escape"}, {"device": "-bad"},
                    {"output": "/tmp/escape.json"}, {"cst": str(outside)}):
        result, report, _ = run(invalid)
        assert result.returncode == 4 and report["state"] == "Failed", invalid
        assert not report["artifacts"]
    # Exercise the actual synth provider then pass its artifact to pnr.
    source = root / "top.v"
    source.write_text("module top(input a, output y); assign y=a; endmodule\n")
    yosys = root / "fake-yosys"
    yosys.write_text("""#!/usr/bin/python3
import json,re,sys
from pathlib import Path
script=Path(sys.argv[2]).read_text()
output=Path(re.search(r'write_json "([^\\n]+)"',script).group(1))
output.write_text(json.dumps({'modules':{'top':{'ports':{},'cells':{},'netnames':{}}}}))
""")
    yosys.chmod(0o700)
    synth_job = root / ".sigflow/agent/jobs/synth/chain"
    synth_job.mkdir(parents=True)
    env = dict(os.environ, SIGFLOW_YOSYS=str(yosys))
    cmd = [binary, "run", "--job-type", "synth", "--work-dir", str(synth_job),
           "--job-id", "chain", "--project", str(root), "--project-id", "p1", "--params", "-"]
    synth = subprocess.run(cmd, input=json.dumps({"top_module":"top", "source_files":[str(source)]}),
                           text=True, capture_output=True, env=env, timeout=10)
    assert synth.returncode == 0, (synth.stdout, synth.stderr)
    synth_report = json.loads((synth_job / "reports/report.json").read_text())
    result, report, _ = run({"netlist": str(synth_job / synth_report["artifacts"][0]["path"])})
    assert result.returncode == 0 and report["state"] == "Succeeded"

    # The provider must propagate a signal to a running nextpnr process.
    counter += 1
    job = f"job-{counter}"
    work = root / ".sigflow/agent/jobs/pnr" / job
    work.mkdir(parents=True)
    payload = {"top_module":"top", "netlist":str(netlist),
               "device":"GW1NR-LV9QN88PC6/I5", "family":"GW1N-9C"}
    env = dict(os.environ, FAKE_PNR_MODE="timeout", SIGFLOW_NEXTPNR=str(tool))
    cmd = [binary, "run", "--job-type", "pnr", "--work-dir", str(work), "--job-id", job,
           "--project", str(root), "--project-id", "p1", "--params", "-"]
    child = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, env=env, start_new_session=True)
    child.stdin.write(json.dumps(payload)); child.stdin.close(); child.stdin = None
    try:
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            ready, _, _ = select.select([child.stdout], [], [], 0.1)
            if ready:
                event = json.loads(child.stdout.readline())
                if event.get("type") == "log" and "placing and routing" in event.get("line", ""):
                    break
            if child.poll() is not None:
                raise AssertionError("pnr exited before cancellation")
        else:
            raise AssertionError("pnr did not start")
        os.killpg(child.pid, signal.SIGTERM)
        stdout, stderr = child.communicate(timeout=8)
        assert child.returncode == -signal.SIGTERM, (stdout, stderr)
        assert json.loads((work / "reports/report.json").read_text())["state"] == "Cancelled"
    finally:
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait()
    print(f"nextpnr production CLI: {counter} fake-tool cases passed, plus synth→pnr chain")
