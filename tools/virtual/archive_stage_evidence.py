"""Copy bounded K0/K1 measurements into reviewable source-controlled reports."""
import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs/virtual/evidence"
OUT.mkdir(parents=True, exist_ok=True)


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


simulation = load(ROOT / ".work/k0/real-simulation-results.json")
if simulation["status"] != "passed":
    raise RuntimeError("K0 simulation did not pass")
ctest = (ROOT / ".work/k0/ctest.log").read_text(encoding="utf-8-sig")
if "100% tests passed, 0 tests failed out of 6" not in ctest:
    raise RuntimeError("Expected selected six regression results are absent")
build_log = (ROOT / ".work/k0/build.log").read_text(encoding="utf-8-sig")
if "Built target sigflow" not in build_log:
    raise RuntimeError("The desktop executable build did not finish")
binary = ROOT / "build-m0/sigflow.exe"
summary = {
    "stage": "K0", "date": "2026-11-13", "timezone": "Asia/Shanghai",
    "base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
    "desktop": {"build": "passed", "binary": "build-m0/sigflow.exe", "bytes": binary.stat().st_size,
                "sha256": digest(binary), "gui_launched": False},
    "regressions": {"selected": 6, "passed": 6,
                    "names": re.findall(r"Test\s+#\d+:\s+(eda_\w+)", ctest)},
    "real_simulation": {"passed": True, "checks": simulation["checks"]},
    "scope": "K0 build + real batch generation + persistent Verilated spike only",
    "limitations": ["No full IJobService-to-Verilator integration test", "No GUI interaction acceptance", "G1/G2/G3 remain open",
                    "Verilator upstream version is unknown; binary hash and observed capability are pinned",
                    "GCC 14.2 and POSIX sh are read-only dependencies from E:/sigflow_fpga/toolchain"],
}
(OUT / "K0-summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
for source, destination in (
    (".work/k0/dependencies.json", "K0-dependencies.json"),
    (".work/k0/real-simulation-results.json", "K0-real-simulation.json"),
    (".work/k0/interactive-transcript.json", "K0-interactive-transcript.json"),
    (".work/k1/analysis-results.json", "K1-analysis.json"),
):
    shutil.copyfile(ROOT / source, OUT / destination)
print("Archived measured K0 results and K1 open-gap analysis; no M0 completion claim.")
