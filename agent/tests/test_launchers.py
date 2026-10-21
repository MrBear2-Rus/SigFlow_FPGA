"""Regression tests for the shipped POSIX launcher.

The Windows checkout can silently turn the shell script into CRLF text.  Linux
then fails at the shebang or treats the carriage return as part of the command.
These tests keep the launcher byte contract and its import wiring stable.
"""

import subprocess
import sys
import unittest
from pathlib import Path


AGENT_ROOT = Path(__file__).resolve().parents[1]
POSIX_LAUNCHER = AGENT_ROOT / "bin" / "sigflow-edu-agent"


class PosixLauncherTests(unittest.TestCase):
    def test_is_utf8_lf_only_shell_script_without_bom(self) -> None:
        raw = POSIX_LAUNCHER.read_bytes()
        self.assertTrue(raw.startswith(b"#!/bin/sh"))
        self.assertFalse(raw.startswith(b"\xef\xbb\xbf"), "launcher must not have a UTF-8 BOM")
        self.assertNotIn(b"\r", raw, "launcher must be LF-only")
        raw.decode("utf-8")

    def test_uses_env_python_and_the_adjacent_src_tree(self) -> None:
        text = POSIX_LAUNCHER.read_text(encoding="utf-8")
        self.assertIn("SIGFLOW_EDU_AGENT_PYTHON", text)
        self.assertIn("python3", text)
        self.assertIn('PYTHONPATH="$here/../src', text)
        self.assertIn('exec "$PY" -m sigflow_edu_agent "$@"', text)

    def test_install_verifier_accepts_the_launcher(self) -> None:
        result = subprocess.run(
            [sys.executable, str(AGENT_ROOT / "scripts" / "verify_install.py")],
            cwd=str(AGENT_ROOT),
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("FAIL", result.stdout)


if __name__ == "__main__":
    unittest.main()
