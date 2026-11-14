"""K1 analysis probes against unchanged production code and actual tool exports."""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work/k1"


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    mingw = "E:/sigflow_fpga/toolchain/mingw64"
    env["PATH"] = mingw + "/bin;" + env.get("PATH", "")
    env["VERILATOR_ROOT"] = (ROOT / "tools/verilator/verilator-install").as_posix()
    fixture = ROOT.parent / "交付资料/04_SigFlow虚拟FPGA平台规划书/review-fixtures/port-index-cases.json"
    cmake = ROOT / ".work/cmake-python/cmake/data/bin/cmake.exe"
    commands = []

    def run(argv, name, required=True):
        result = subprocess.run([str(arg) for arg in argv], env=env, cwd=ROOT,
                                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
        (WORK / (name + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        commands.append({"name": name, "argv": [str(arg) for arg in argv], "exit_code": result.returncode})
        if required and result.returncode:
            raise RuntimeError(f"{name} failed; see .work/k1/{name}.log")
        return result

    build = ROOT / "build-k0-generator"
    run([cmake, "-S", ROOT / "tests/virtual/k0", "-B", build, "-G", "MinGW Makefiles",
         f"-DCMAKE_CXX_COMPILER={mingw}/bin/g++.exe", f"-DCMAKE_MAKE_PROGRAM={mingw}/bin/mingw32-make.exe"], "configure-probe")
    run([cmake, "--build", build, "--target", "k1_baseline_cst_probe", "--parallel", "3"], "build-probe")
    native = json.loads(run([build / "k1_baseline_cst_probe.exe", fixture], "baseline-cst-probe").stdout)
    (WORK / "baseline-cst-probe.json").write_text(json.dumps(native, indent=2) + "\n", encoding="utf-8")
    yosys = ROOT / "external/fpga-tools/runtime/yosys/bin/yosys.exe"
    verilator = ROOT / "tools/verilator/verilator-install/bin/verilator_bin_dbg.exe"
    yosys_version = run([yosys, "-V"], "yosys-version").stdout.strip()
    run([yosys, "-Q", "-T", "-p",
         "read_verilog -sv tests/virtual/k1/ports.sv; hierarchy -top k1_ports; proc; write_json .work/k1/yosys-ports.json"], "yosys-export")
    # Capability probing, not an assumed XML API: the supplied runtime rejects XML.
    xml = run([verilator, "--xml-only", "--xml-output", ".work/k1/verilator-ports.xml", "--Mdir", ".work/k1/obj-xml",
               "--top-module", "k1_ports", "-Wno-fatal", "tests/virtual/k1/ports.sv"], "verilator-xml", required=False)
    run([verilator, "--json-only", "--Mdir", ".work/k1/obj-json", "--top-module", "k1_ports", "-Wno-fatal",
         "tests/virtual/k1/ports.sv"], "verilator-json")
    yosys_data = json.loads((WORK / "yosys-ports.json").read_text())
    module = yosys_data["modules"]["k1_ports"]
    tree = json.loads((WORK / "obj-json/Vk1_ports.tree.json").read_text())
    type_table = next(node for node in tree["miscsp"] if node["type"] == "TYPETABLE")
    types = {node["addr"]: node for node in type_table["typesp"]}
    ast_module = next(node for node in tree["modulesp"] if node.get("name") == "k1_ports")
    declarations = []
    for node in ast_module["stmtsp"]:
        if node["type"] == "VAR" and node.get("isPrimaryIO"):
            dtype = types[node["dtypep"]]
            declarations.append({"name": node["name"], "direction": node["direction"], "dtype_kind": dtype["type"],
                                 "range": dtype.get("range"), "source_location": node["loc"]})
    try:
        json.loads((WORK / "obj-json/Vk1_ports.tree.meta.json").read_text())
        meta_status = "valid"
    except json.JSONDecodeError as error:
        meta_status = str(error)
    report = {
        "stage": "K1-analysis", "implementation_status": "not_implemented", "g1_closed": False,
        "commands": commands,
        "fixtures_sha256": digest(fixture),
        "toolchain": {"yosys_version": yosys_version, "yosys_sha256": digest(yosys), "verilator_sha256": digest(verilator)},
        "baseline_cst_results": native["cases"],
        "actual_yosys_ports": module["ports"],
        "actual_yosys_single_vector_attributes": module["netnames"]["one_bit_vector"]["attributes"],
        "actual_verilator_declarations": declarations,
        "xml_supported": xml.returncode == 0,
        "json_meta_parse_status": meta_status,
        "production_electrical_data": "not yet verified; synthetic fixtures must not be installed as board data",
    }
    (WORK / "analysis-results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("K1 analysis recorded: baseline [0]/index gaps reproduced; actual Yosys and Verilator AST inspected; G1 remains open.")


if __name__ == "__main__":
    main()
