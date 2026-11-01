"""Optional WSL interop smoke; requires a user-supplied Windows Yosys path."""
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("usage: python3 windows_yosys_interop.py <sigflow-tool> <windows-yosys.exe>")
cli, windows_yosys = map(lambda value: Path(value).resolve(strict=True), sys.argv[1:])
build = cli.parent.parent
with tempfile.TemporaryDirectory(prefix="yosys-win-adapter-", dir=build) as directory:
    project = Path(directory)
    (project / "top.v").write_text("module top(input a, output y); assign y = a; endmodule\n")
    job = project / ".sigflow/agent/jobs/synth/check"
    job.mkdir(parents=True)
    # Test-only adapter: it translates paths in the generated script for the Windows executable.
    adapter = project / "test-yosys-adapter"
    adapter.write_text("""#!/usr/bin/python3
import os, re, subprocess, sys
from pathlib import Path
windows_yosys = """ + repr(str(windows_yosys)) + """
def translate(path):
    return subprocess.check_output(['wslpath', '-m', path], text=True).strip()
script = Path(sys.argv[2])
text = re.sub(r'"(/mnt/[^"\\n]*)"', lambda match: '"' + translate(match.group(1)) + '"', script.read_text())
converted = script.with_name('test-only-win-paths.ys')
converted.write_text(text)
os.execv(windows_yosys, [windows_yosys, '-s', translate(str(converted))])
""")
    adapter.chmod(0o700)
    arguments = [str(cli), "run", "--job-type", "synth", "--project", str(project),
                 "--project-id", "interop", "--job-id", "check", "--work-dir", str(job),
                 "--params", "-", "--timeout-sec", "90", "--max-output-bytes", "2097152"]
    parameters = {"top_module": "top", "source_files": [str(project / "top.v")],
                  "yosys_path": str(adapter)}
    result = subprocess.run(arguments, input=json.dumps(parameters), text=True,
                            capture_output=True, timeout=100)
    if result.returncode:
        raise RuntimeError(f"CLI exit {result.returncode}: {result.stdout[-4000:]} {result.stderr}")
    report = json.loads((job / "reports/report.json").read_text())
    artifact = report["artifacts"][0]
    netlist = (job / artifact["path"]).read_bytes()
    assert report["state"] == "Succeeded" and report["exit_code"] == 0
    assert "top" in json.loads(netlist)["modules"]
    assert hashlib.sha256(netlist).hexdigest() == artifact["sha256"]
    print(f"Windows Yosys WSL adapter: successful top.json ({len(netlist)} bytes), sha256 verified")
