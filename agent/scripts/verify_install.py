#!/usr/bin/env python3
"""Verify an installed (or source-tree) sigflow-edu-agent without network or side effects.

Checks performed (read-only):
  * the package imports and exposes AGENT_VERSION / PROTOCOL;
  * the declared version matches the expected release version;
  * both launchers exist and are shaped correctly (LF-only POSIX sh, `#!/bin/sh`);
  * pyproject.toml declares no runtime dependencies;
  * every module listed in EXPECTED_MODULES is importable;
  * the package contains no absolute local paths or UCAgent references.

Exit code 0 means every check passed; 1 means at least one failed.
Usage:  python agent/scripts/verify_install.py [--expected-version 0.1.0]
"""

from __future__ import annotations

import argparse
import importlib
import re
import sys
from pathlib import Path

EXPECTED_VERSION = "0.1.0"
EXPECTED_PROTOCOL = "edu.api.v1"
EXPECTED_MODULES = [
    "sigflow_edu_agent",
    "sigflow_edu_agent.version",
    "sigflow_edu_agent.errors",
    "sigflow_edu_agent.util",
    "sigflow_edu_agent.bootstrap",
    "sigflow_edu_agent.gateway_client",
    "sigflow_edu_agent.model_client",
    "sigflow_edu_agent.rules",
    "sigflow_edu_agent.cards",
    "sigflow_edu_agent.dispatch",
    "sigflow_edu_agent.session",
    "sigflow_edu_agent.http_server",
    "sigflow_edu_agent.__main__",
]

_failures = 0


def check(ok: bool, message: str, detail: str = "") -> bool:
    global _failures
    if ok:
        print("  ok  : %s" % message)
    else:
        _failures += 1
        print("  FAIL: %s%s" % (message, (" -- " + detail) if detail else ""))
    return ok


def agent_root() -> Path:
    """Repository `agent/` directory (this file lives in agent/scripts/)."""
    return Path(__file__).resolve().parent.parent


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected-version", default=EXPECTED_VERSION)
    args = parser.parse_args()

    root = agent_root()
    print("agent root: %s" % root)

    # Make the source tree importable even when the package was not pip-installed.
    src = root / "src"
    if src.is_dir() and str(src) not in sys.path:
        sys.path.insert(0, str(src))

    try:
        version_module = importlib.import_module("sigflow_edu_agent.version")
    except Exception as error:  # noqa: BLE001 - report any import failure verbatim
        check(False, "package imports", str(error))
        print("FAILURES")
        return 1

    check(True, "package imports")
    check(
        getattr(version_module, "AGENT_VERSION", None) == args.expected_version,
        "AGENT_VERSION is %s" % args.expected_version,
        "got %r" % getattr(version_module, "AGENT_VERSION", None),
    )
    check(
        getattr(version_module, "PROTOCOL", None) == EXPECTED_PROTOCOL,
        "PROTOCOL is %s" % EXPECTED_PROTOCOL,
        "got %r" % getattr(version_module, "PROTOCOL", None),
    )

    for module in EXPECTED_MODULES:
        try:
            importlib.import_module(module)
            check(True, "module %s imports" % module)
        except Exception as error:  # noqa: BLE001
            check(False, "module %s imports" % module, str(error))

    launcher_sh = root / "bin" / "sigflow-edu-agent"
    launcher_cmd = root / "bin" / "sigflow-edu-agent.cmd"
    check(launcher_sh.is_file(), "POSIX launcher exists")
    check(launcher_cmd.is_file(), "Windows launcher exists")
    if launcher_sh.is_file():
        raw = launcher_sh.read_bytes()
        check(raw.startswith(b"#!/bin/sh"), "POSIX launcher starts with #!/bin/sh")
        check(b"\r\n" not in raw, "POSIX launcher is LF-only")
    if launcher_cmd.is_file():
        text = launcher_cmd.read_text(encoding="utf-8", errors="replace")
        check("%~dp0" in text, "Windows launcher resolves its own directory")
        check(
            "SIGFLOW_EDU_AGENT_PYTHON" in text,
            "Windows launcher honours SIGFLOW_EDU_AGENT_PYTHON",
        )

    pyproject = root / "pyproject.toml"
    check(pyproject.is_file(), "pyproject.toml exists")
    if pyproject.is_file():
        text = pyproject.read_text(encoding="utf-8", errors="replace")
        match = re.search(r"^dependencies\s*=\s*\[(.*?)\]", text, re.MULTILINE | re.DOTALL)
        check(match is not None, "pyproject.toml declares a dependencies list")
        if match is not None:
            check(
                match.group(1).strip() == "",
                "runtime dependencies are empty (stdlib only)",
                "got %r" % match.group(1).strip(),
            )

    # Hygiene: no absolute local paths and no UCAgent coupling in the shipped package.
    offenders: list[str] = []
    for path in sorted(src.rglob("*.py")):
        text = path.read_text(encoding="utf-8", errors="replace")
        if re.search(r"[A-Za-z]:\\\\|/home/|/tmp/", text):
            offenders.append("%s: absolute path literal" % path.name)
        if re.search(r"ucagent|UCTool", text, re.IGNORECASE):
            offenders.append("%s: UCAgent reference" % path.name)
    check(not offenders, "package has no absolute paths or UCAgent coupling", "; ".join(offenders))

    print("ALL PASS" if _failures == 0 else "FAILURES")
    return 0 if _failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
