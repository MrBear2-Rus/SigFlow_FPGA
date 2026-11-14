"""Build and exercise REAL Verilator models; K0 only, not full M0 acceptance."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/virtual/k0"
WORK = ROOT / ".work/k0"


def sha256(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mingw", type=Path, default=Path("E:/sigflow_fpga/toolchain/mingw64"))
    parser.add_argument("--shell", type=Path, default=Path("E:/sigflow_fpga/toolchain/posix/sh.exe"))
    parser.add_argument("--cmake", type=Path, default=ROOT / ".work/cmake-python/cmake/data/bin/cmake.exe")
    args = parser.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    temporary = ROOT / ".work/temp"
    temporary.mkdir(parents=True, exist_ok=True)
    verilator_root = ROOT / "tools/verilator/verilator-install"
    verilator = verilator_root / "bin/verilator_bin_dbg.exe"
    posix_tools = WORK / "posix-tools"
    posix_tools.mkdir(exist_ok=True)
    busybox = args.shell.parent / "busybox64.exe"
    if busybox.is_file():
        # GNU Make's Windows $(shell uname -s) invokes uname directly. A native
        # BusyBox applet avoids depending on a separate Git/MSYS runtime.
        shutil.copy2(busybox, posix_tools / "uname.exe")
    env = dict(os.environ)
    env.update({"PATH": f"{posix_tools};{args.mingw / 'bin'};{args.shell.parent};{env.get('PATH', '')}",
                "TEMP": str(temporary), "TMP": str(temporary), "VERILATOR_ROOT": verilator_root.as_posix()})
    report = {"stage": "K0", "status": "running", "commands": [], "checks": [],
              "toolchain": {"cmake": str(args.cmake), "mingw": str(args.mingw),
                            "verilator": str(verilator), "verilator_sha256": sha256(verilator)},
              "scope": "real batch generator + persistent Verilated spike; no CST, electrical, LiveTrace or GUI acceptance"}

    def run(command, name, cwd=ROOT, timeout=180):
        command = [str(value) for value in command]
        started = time.monotonic()
        completed = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True,
                                   encoding="utf-8", errors="replace", timeout=timeout)
        (WORK / f"{name}.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")
        report["commands"].append({"name": name, "argv": command, "exit_code": completed.returncode,
                                   "seconds": round(time.monotonic() - started, 3)})
        require(completed.returncode == 0, f"{name} failed; see .work/k0/{name}.log")
        return completed.stdout.strip()

    def build_model(main_cpp: Path, output: str, name: str):
        obj = WORK / f"obj-{name}"
        executable = WORK / output
        run([verilator, "--cc", FIXTURE / "counter.sv", "--exe", main_cpp, "--trace", "-Wno-fatal",
             "--top-module", "k0_counter", "--Mdir", obj, "-CFLAGS", f"-std=c++20 -I{ROOT.as_posix()}/3rd",
             "-o", executable], f"{name}-verilate")
        run([args.mingw / "bin/mingw32-make.exe", "-C", obj, "-f", "Vk0_counter.mk", "-j4",
             f"CXX={args.mingw.as_posix()}/bin/g++.exe", f"LINK={args.mingw.as_posix()}/bin/g++.exe",
             f"AR={args.mingw.as_posix()}/bin/ar.exe", f"SHELL={args.shell.as_posix()}",
             f"PYTHON3={Path(sys.executable).as_posix()}"], f"{name}-compile")
        return executable

    try:
        report["toolchain"]["verilator_version_output"] = run([verilator, "--version"], "verilator-version")
        report["toolchain"]["gcc_version_output"] = run([args.mingw / "bin/g++.exe", "--version"], "gcc-version")
        generator_build = ROOT / "build-k0-generator"
        run([args.cmake, "-S", FIXTURE, "-B", generator_build, "-G", "MinGW Makefiles",
             f"-DCMAKE_CXX_COMPILER={args.mingw.as_posix()}/bin/g++.exe",
             f"-DCMAKE_MAKE_PROGRAM={args.mingw.as_posix()}/bin/mingw32-make.exe"], "generator-configure")
        run([args.cmake, "--build", generator_build, "--target", "k0_generate_batch", "--parallel", "3"], "generator-build")
        batch_main = WORK / "generated_batch.cpp"
        run([generator_build / "k0_generate_batch.exe", batch_main], "batch-generate")
        batch_exe = build_model(batch_main, "batch-counter.exe", "batch")
        run([batch_exe, "--sigflow-duration-ticks", "120"], "batch-run", cwd=WORK)
        vcd = (WORK / "batch.vcd").read_text(encoding="utf-8")
        code = None
        for line in vcd.splitlines():
            words = line.split()
            if words[:1] == ["$var"] and len(words) > 4 and words[4] == "observed":
                code = words[3]
                break
        require(code is not None, "reference output absent from generated VCD")
        last = None
        for line in vcd.splitlines():
            words = line.split()
            if len(words) == 2 and words[1] == code and words[0].startswith("b"):
                last = int(words[0][1:], 2)
        require(last == 10, f"unchanged baseline generator counter expected 10, got {last}")
        report["checks"].append({"name": "baseline-generated-batch", "expected": 10, "actual": last,
                                  "vcd_sha256": sha256(WORK / "batch.vcd")})
        executable = build_model(FIXTURE / "interactive_main.cpp", "persistent-counter.exe", "interactive")
        responses = []
        with (WORK / "interactive-stderr.log").open("w", encoding="utf-8") as stderr:
            process = subprocess.Popen([str(executable), str(WORK / "interactive.vcd")], cwd=WORK, env=env,
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                                       text=True, encoding="utf-8", bufsize=1)
            lines: queue.Queue = queue.Queue()
            def reader():
                for line in process.stdout:
                    lines.put(line)
                lines.put(None)
            threading.Thread(target=reader, daemon=True).start()
            def send(op, **fields):
                command = {"op": op, **fields}
                process.stdin.write(json.dumps(command) + "\n")
                process.stdin.flush()
                deadline = time.monotonic() + 10
                while True:
                    line = lines.get(timeout=max(0.01, deadline - time.monotonic()))
                    require(line is not None, f"runner exited before replying to {op}")
                    try:
                        response = json.loads(line)
                        break
                    except json.JSONDecodeError:
                        require(time.monotonic() < deadline, "runner produced no JSON reply")
                responses.append({"request": command, "response": response})
                require(response["status"] != "rejected", str(response))
                require(response["model_instances"] == 1, "model was recreated")
                return response
            try:
                initial = send("get")
                send("step")  # reset held low on the first real rising edge
                before = send("get")
                send("set", port="reset_n", value=True)
                send("set", port="gate_in", value=True)
                pending = send("get")
                for field in ("tick", "eval_calls", "input_writes", "count"):
                    require(pending[field] == before[field], f"paused set changed {field}")
                third = send("step", cycles=3)
                require(third["count"] == 3, "three real DUT edges did not count three times")
                edges = int(third["edges"])
                expected_tick = (edges * 10**12 + 27_000_000) // 54_000_000
                require(int(third["tick"]) == expected_tick, "rational clock mismatch")
                # Last-intent merge while paused: true->false must never create a pulse.
                send("set", port="gate_in", value=True)
                send("set", port="gate_in", value=False)
                merged = send("step")
                require(merged["count"] == 3, "paused merged release caused a false count")
                send("set", port="gate_in", value=True)
                fourth = send("step")
                require(fourth["count"] == 4, "model state did not persist between steps")
                send("play")
                time.sleep(0.03)
                paused = send("pause")
                time.sleep(0.03)
                frozen = send("get")
                for field in ("tick", "edges", "eval_calls", "input_writes", "count"):
                    require(frozen[field] == paused[field], f"pause did not freeze {field}")
                require(int(paused["tick"]) > int(fourth["tick"]), "Play did not advance simulation")
                stopped = send("stop")
                require(stopped["final_calls"] == 1, "Stop did not finalize exactly once")
                process.stdin.close()
                require(process.wait(timeout=10) == 0, "persistent process exit failure")
                report["checks"].extend([
                    {"name": "persistent-model-count", "count_after_steps": fourth["count"]},
                    {"name": "paused-input-no-writes-or-eval", "passed": True},
                    {"name": "same-target-pending-merge", "passed": True},
                    {"name": "rational-27mhz-edge", "edge": str(edges), "tick": str(expected_tick)},
                    {"name": "play-pause-freeze", "paused_tick": paused["tick"], "passed": True},
                    {"name": "final-once", "actual": stopped["final_calls"]},
                ])
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                (WORK / "interactive-transcript.json").write_text(json.dumps(responses, indent=2) + "\n", encoding="utf-8")
        report["status"] = "passed"
        print(f'PASS: real batch result={last}; {len(report["checks"])} K0 checks; persistent Verilated model verified.')
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        (WORK / "real-simulation-results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
