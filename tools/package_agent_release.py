"""Assemble an offline Agent + teaching-core release directory.

The release launcher imports installed wheels from its private ``runtime``
directory.  It never adds a repository ``src`` directory to PYTHONPATH.  A
Python >=3.11 interpreter is supplied by the product installer (or selected
through SIGFLOW_EDU_AGENT_PYTHON for an offline deployment).
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--python", default=sys.executable,
                        help="interpreter used to build wheels")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        shutil.rmtree(output)
    wheels = output / "wheels"
    runtime = output / "runtime"
    contracts = output / "contracts" / "edu-agent" / "v1"
    wheels.mkdir(parents=True)
    contracts.mkdir(parents=True)

    with tempfile.TemporaryDirectory(prefix="sigflow-m1-package-") as temporary:
        source_root = Path(temporary)
        sources = []
        for directory, package in (("agent", "src"), ("sigflowAgent", "sigflow_teaching_core")):
            source = source_root / directory
            source.mkdir(parents=True)
            for name in ("pyproject.toml", "README.md", "LICENSE"):
                if (ROOT / directory / name).exists():
                    shutil.copy2(ROOT / directory / name, source / name)
            shutil.copytree(ROOT / directory / package, source / package)
            if (ROOT / directory / "bin").exists():
                shutil.copytree(ROOT / directory / "bin", source / "bin")
            sources.append(str(source))
        subprocess.run([args.python, "-m", "pip", "wheel", "--no-index", "--no-deps",
                        "--no-build-isolation", "--wheel-dir", str(wheels), *sources],
                       check=True)

    for path in (ROOT / "contracts" / "edu-agent" / "v1").rglob("*"):
        if path.is_file():
            destination = contracts / path.relative_to(ROOT / "contracts" / "edu-agent" / "v1")
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)

    launcher = output / "run-agent.cmd"
    launcher.write_text("""@echo off\nsetlocal\nif defined SIGFLOW_EDU_AGENT_PYTHON (set \"PY=%SIGFLOW_EDU_AGENT_PYTHON%\") else (set \"PY=python\")\nif exist \"%~dp0runtime\\Scripts\\python.exe\" set \"PY=%~dp0runtime\\Scripts\\python.exe\"\n\"%PY%\" -m sigflow_edu_agent %*\nexit /b %ERRORLEVEL%\n""", encoding="utf-8", newline="\r\n")
    posix_launcher = output / "run-agent"
    posix_launcher.write_text("""#!/bin/sh\nset -eu\nPY=${SIGFLOW_EDU_AGENT_PYTHON:-python3}\nif [ -x \"$(dirname \"$0\")/runtime/bin/python\" ]; then PY=\"$(dirname \"$0\")/runtime/bin/python\"; fi\nexec \"$PY\" -m sigflow_edu_agent \"$@\"\n""", encoding="utf-8", newline="\n")
    posix_launcher.chmod(0o755)
    installer = output / "install-agent.cmd"
    installer.write_text("""@echo off\nsetlocal\nif defined SIGFLOW_EDU_AGENT_PYTHON (set \"PY=%SIGFLOW_EDU_AGENT_PYTHON%\") else (set \"PY=python\")\nif not exist \"%~dp0runtime\\Scripts\\python.exe\" (\n  \"%PY%\" -m venv \"%~dp0runtime\"\n  if errorlevel 1 exit /b %ERRORLEVEL%\n)\nfor %%W in (\"%~dp0wheels\\*.whl\") do (\n  \"%~dp0runtime\\Scripts\\python.exe\" -m pip install --no-index --no-deps \"%%~fW\"\n  if errorlevel 1 exit /b %ERRORLEVEL%\n)\nexit /b 0\n""", encoding="utf-8", newline="\r\n")

    manifest = {
        "format": "sigflow-agent-release-v1",
        "python": ">=3.11",
        "launcher": "run-agent.cmd",
        "packages": [
            {"file": path.name, "sha256": sha256(path)}
            for path in sorted(wheels.glob("*.whl"))
        ],
        "contracts": "contracts/edu-agent/v1",
        "source_pythonpath": False,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"M1 Agent release assembled: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
