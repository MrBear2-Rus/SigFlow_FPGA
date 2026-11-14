"""Build and exercise actual RTL using the repository's bundled Verilator."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rtl", type=Path, help="alternate full_adder.v for negative testing")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    repo = project.parents[1]
    output = project / "build"
    output.mkdir(exist_ok=True)
    env = os.environ.copy()
    if os.name == "nt":
        compiler = repo / "3rd/mingw-msvcrt/mingw64/bin"
        installation = repo / "tools/verilator/verilator-install"
        git = shutil.which("git")
        if not git:
            raise RuntimeError("Git for Windows is required for its shell and Perl")
        shell_bin = Path(git).resolve().parents[1] / "usr/bin"
        make_dir = output / "tools"
        make_dir.mkdir(exist_ok=True)
        shutil.copyfile(compiler / "mingw32-make.exe", make_dir / "make.exe")
        env["PATH"] = os.pathsep.join(map(str, (
            make_dir, compiler, shell_bin, Path(sys.executable).parent, env.get("PATH", ""))))
        env["VERILATOR_ROOT"] = installation.as_posix()
        env["MAKEFLAGS"] = "-- CXX=g++ LINK=g++ AR=ar PYTHON3=python"
        env.pop("MFLAGS", None)
        env.pop("GNUMAKEFLAGS", None)
        verilator = installation / "bin/verilator_bin_dbg.exe"
    else:
        verilator = shutil.which("verilator")
        if not verilator:
            raise RuntimeError("Install Verilator and a C++ compiler before running tests")
    rtl = (args.rtl or project / "src/full_adder.v").resolve()
    command = [
        str(verilator), "--cc", "--exe", "--build", "-j", "2", "--trace", "--coverage",
        "--top-module", "top", "--Mdir", str(output / "obj"),
        str(rtl), str(project / "src/top.v"), str(project / "tests/test_full_adder.cpp"),
        "-o", "full_adder_test.exe",
    ]
    # Remove only old result files owned by this runner so a failed test cannot
    # leave a previous PASS looking like the current result.
    for name in ("functional_coverage.json", "coverage.dat", "structural_coverage.json"):
        (output / name).unlink(missing_ok=True)
    subprocess.run(command, cwd=output, env=env, check=True)
    subprocess.run([str(output / "obj/full_adder_test.exe")], cwd=output, env=env, check=True)
    counters = [int(match.group(1)) for match in re.finditer(
        r"^C .* ([0-9]+)$", (output / "coverage.dat").read_text(encoding="utf-8"), re.MULTILINE)]
    if not counters:
        raise RuntimeError("Verilator produced no coverage counters")
    summary = dict(covered=sum(value > 0 for value in counters), total=len(counters))
    summary["percent"] = round(summary["covered"] * 100 / summary["total"], 2)
    (output / "structural_coverage.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print("Verilator instrumented counters:", summary)
    print("Reports and VCD:", output)


if __name__ == "__main__":
    main()
