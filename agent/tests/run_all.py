#!/usr/bin/env python3
"""Run the complete sigflow_edu_agent test suite; exit non-zero on failure.

Usage (from the repository root or from ``agent/``)::

    python agent/tests/run_all.py

This is a thin wrapper around :mod:`unittest` discovery that pins the import
root to ``agent/src`` and reports a stable, greppable summary line.
"""

from __future__ import annotations

import os
import sys
import unittest

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_DIR = os.path.dirname(TESTS_DIR)
SRC_DIR = os.path.join(AGENT_DIR, "src")
TOP_LEVEL = AGENT_DIR


def main() -> int:
    """Discover and run the suite; return a process exit code."""
    if SRC_DIR not in sys.path:
        sys.path.insert(0, SRC_DIR)
    loader = unittest.TestLoader()
    suite = loader.discover(start_dir=TESTS_DIR, top_level_dir=TOP_LEVEL)
    runner = unittest.TextTestRunner(verbosity=2, stream=sys.stdout)
    result = runner.run(suite)
    total = result.testsRun
    failed = len(result.failures) + len(result.errors)
    print(
        "\nSUMMARY: run=%d failures=%d errors=%d skipped=%d"
        % (total, len(result.failures), len(result.errors), len(result.skipped))
    )
    if not total:
        print("SUMMARY: no tests were collected", file=sys.stderr)
        return 1
    return 0 if (result.wasSuccessful() and failed == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
