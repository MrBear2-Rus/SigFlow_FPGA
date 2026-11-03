"""Exercise the production pack provider using isolated stand-in tools."""
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

try:
    from jsonschema import Draft7Validator
except ImportError:
    Draft7Validator = None
schema = json.loads((Path(__file__).resolve().parents[1] / "contracts/run-v1/gowin-pack-params.schema.json").read_text())
if Draft7Validator:
    Draft7Validator.check_schema(schema)
    Draft7Validator(schema).validate({"pnr_json":"design.json", "device":"GW1N-9C"})
    assert not Draft7Validator(schema).is_valid({"pnr_json":"design.json", "device":"-bad"})

binary, build = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="pack-", dir=build) as tmp:
    root = Path(tmp) / "工程 space"
    root.mkdir()
    pnr = root / "placed design.json"
    pnr.write_text('{"modules":{"top":{}}}')
    tool = root / "fake gowin_pack"
    tool.write_text("""#!/usr/bin/python3
import os,sys,time
from pathlib import Path
a=sys.argv[1:]
assert len(a)==5 and a[0]=='-d' and a[1]=='GW1N-9C' and a[2]=='-o'
assert Path(a[4]).is_file()
print('packing design',flush=True)
mode=os.environ.get('PACK_MODE','success')
if mode=='fail': sys.exit(23)
if mode=='missing': sys.exit(0)
if mode=='timeout': time.sleep(30)
if mode=='small': Path(a[3]).write_bytes(b'x'*31);sys.exit(0)
if mode=='symlink': Path(a[3]).symlink_to(os.environ['OUTSIDE']);sys.exit(0)
Path(a[3]).write_bytes(b'x'*64)
""")
    tool.chmod(0o700)
    count = 0
    def run(params=None, mode="success", available=True, extra=()):
        global count
        count += 1
        job = f"job-{count}"
        work = root / ".sigflow/agent/jobs/pack" / job
        work.mkdir(parents=True)
        payload = {"pnr_json": str(pnr), "device": "GW1N-9C"}
        payload.update(params or {})
        env = dict(os.environ, PACK_MODE=mode, PATH="/nonexistent", OUTSIDE=str(pnr))
        env.pop("SIGFLOW_GOWIN_PACK", None)
        env.pop("SIGFLOW_TOOL_RUNTIME_DIR", None)
        if available: env["SIGFLOW_GOWIN_PACK"] = str(tool)
        args = [binary, "run", "--job-type", "pack", "--work-dir", str(work), "--job-id", job,
                "--project", str(root), "--project-id", "p1", "--params", "-", *extra]
        result = subprocess.run(args, input=json.dumps(payload), text=True, capture_output=True,
                                env=env, timeout=12)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        assert sum(e["type"] == "result" for e in events) == 1, result.stdout
        report = json.loads((work / "reports/report.json").read_text())
        return result, report, work

    result, report, work = run()
    assert result.returncode == 0, (result.stdout, result.stderr)
    artifact = report["artifacts"][0]
    assert artifact["id"] == "bitstream" and artifact["path"] == "artifacts/design.fs"
    digest = hashlib.sha256((work / artifact["path"]).read_bytes()).hexdigest()
    assert artifact["sha256"] == digest
    manifest = json.loads((work / "artifacts/design.pack.manifest.json").read_text())
    assert manifest["output"]["sha256"] == digest and manifest["input"]["pnr_json"] == str(pnr)
    assert manifest["executable"]["sha256"] == hashlib.sha256(tool.read_bytes()).hexdigest()
    result, report, work = run({"output":"nested/custom.fs", "gowin_pack_path":str(tool)}, available=False)
    assert result.returncode == 0 and report["artifacts"][0]["path"] == "artifacts/nested/custom.fs"
    for mode in ("fail", "missing", "small", "symlink"):
        result, report, _ = run(mode=mode)
        assert result.returncode == 4 and report["state"] == "Failed" and not report["artifacts"], (mode, result.stdout)
        if mode == "fail": assert report["exit_code"] == 23
    result, report, _ = run(available=False)
    assert result.returncode == 4 and report["exit_code"] == -1
    result, report, _ = run(mode="timeout", extra=("--timeout-sec", "1"))
    assert result.returncode == 4 and report["state"] == "TimedOut"
    outside = Path(tmp) / "outside.json"
    outside.write_text('{}')
    link = root / "link.json"
    link.symlink_to(outside)
    empty = root / "empty.json"
    empty.touch()
    for params in ({"pnr_json":str(outside)}, {"pnr_json":str(link)}, {"pnr_json":str(empty)},
                   {"pnr_json":"missing.json"}, {"pnr_json":None}, {"device":"-invalid"},
                   {"output":"../escape.fs"}, {"output":str(outside)}, {"output":None}):
        result, report, _ = run(params)
        assert result.returncode == 4 and report["state"] == "Failed" and not report["artifacts"], (params, result.stdout)

    # Exercise actual provider boundaries through fake synth, PNR, and pack executables.
    source = root / "top.v"
    source.write_text('module top; endmodule\n')
    yosys = root / "yosys"
    yosys.write_text("""#!/usr/bin/python3
import json,re,sys
from pathlib import Path
script=Path(sys.argv[2]).read_text()
out=Path(re.search(r'write_json "([^\\n]+)"',script).group(1))
out.write_text(json.dumps({'modules':{'top':{'ports':{},'cells':{},'netnames':{}}}}))
""")
    yosys.chmod(0o700)
    nextpnr = root / "nextpnr"
    nextpnr.write_text("""#!/usr/bin/python3
import json,sys
from pathlib import Path
a=sys.argv
assert Path(a[a.index('--json')+1]).is_file()
Path(a[a.index('--write')+1]).write_text(json.dumps({'modules':{'top':{}}}))
""")
    nextpnr.chmod(0o700)
    def stage(kind, payload, env):
        work = root / '.sigflow/agent/jobs' / kind / 'chain'
        work.mkdir(parents=True)
        args = [binary, 'run', '--job-type', kind, '--work-dir', str(work), '--job-id', 'chain',
                '--project', str(root), '--project-id', 'p1', '--params', '-']
        result = subprocess.run(args, input=json.dumps(payload), text=True, capture_output=True,
                                env=env, timeout=10)
        assert result.returncode == 0, (kind, result.stdout, result.stderr)
        report = json.loads((work / 'reports/report.json').read_text())
        return work / report['artifacts'][0]['path']
    synth_out = stage('synth', {'top_module':'top', 'source_files':[str(source)]},
                      dict(os.environ, SIGFLOW_YOSYS=str(yosys)))
    pnr_out = stage('pnr', {'top_module':'top', 'netlist':str(synth_out), 'device':'GW1N-9C'},
                    dict(os.environ, SIGFLOW_NEXTPNR=str(nextpnr)))
    bitstream = stage('pack', {'pnr_json':str(pnr_out), 'device':'GW1N-9C'},
                      dict(os.environ, SIGFLOW_GOWIN_PACK=str(tool)))
    assert bitstream.read_bytes() == b'x'*64

    count += 1
    job = f"job-{count}"
    work = root / '.sigflow/agent/jobs/pack' / job
    work.mkdir(parents=True)
    args = [binary, 'run', '--job-type', 'pack', '--work-dir', str(work), '--job-id', job,
            '--project', str(root), '--project-id', 'p1', '--params', '-']
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True,
                            env=dict(os.environ, SIGFLOW_GOWIN_PACK=str(tool), PACK_MODE='timeout'))
    proc.stdin.write(json.dumps({'pnr_json':str(pnr), 'device':'GW1N-9C'}))
    proc.stdin.close(); proc.stdin = None
    try:
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            ready, _, _ = select.select([proc.stdout], [], [], .1)
            if ready and 'packing design' in proc.stdout.readline(): break
            if proc.poll() is not None: raise AssertionError('pack exited before cancellation')
        else: raise AssertionError('pack did not start')
        os.killpg(proc.pid, signal.SIGTERM)
        stdout, stderr = proc.communicate(timeout=8)
        assert proc.returncode == -signal.SIGTERM, (stdout, stderr)
        assert json.loads((work / 'reports/report.json').read_text())['state'] == 'Cancelled'
    finally:
        if proc.poll() is None: os.killpg(proc.pid, signal.SIGKILL); proc.wait()
    print(f'Gowin pack: {count} fake-tool cases passed, plus synth→pnr→pack chain')
