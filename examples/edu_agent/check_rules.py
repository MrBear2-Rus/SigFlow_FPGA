#!/usr/bin/env python3
"""Run the deterministic, model-free rule engine over the sample RTL.

Usage (from anywhere)::

    python examples/edu_agent/check_rules.py                  # run all samples
    python examples/edu_agent/check_rules.py rtl/某些文件.v    # ad-hoc file(s)

With no arguments the script replays every file listed in
``expected_findings.json`` and compares the engine output with the recorded
expectation, exiting non-zero on the first mismatch.  With file arguments it
prints the findings for each file: that is the manual review path for "why did
you report / not report this snippet?".

Output is intentionally ASCII (rule ids, file names, PASS/FAIL) except for the
finding title, so it survives a GBK Windows console.
"""

from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
SRC_DIR = os.path.join(REPO_ROOT, "agent", "src")
if SRC_DIR not in sys.path:
    sys.path.insert(0, SRC_DIR)

from sigflow_edu_agent.rules import Selection, analyse  # noqa: E402

EXPECTED_PATH = os.path.join(HERE, "expected_findings.json")


def read_source(path: str) -> str:
    """Read one UTF-8 RTL file."""
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def analyse_file(path: str) -> list:
    """Run ``analyse`` on one file, tagging the selection with its basename."""
    return analyse(Selection(text=read_source(path), source_id=os.path.basename(path)))


def load_expected() -> dict:
    """Load the sample -> expected findings map."""
    with open(EXPECTED_PATH, "r", encoding="utf-8") as handle:
        return json.load(handle)["samples"]


def describe(findings: list) -> str:
    """Render one finding list as ``RULE(signal)`` pairs."""
    if not findings:
        return "no findings"
    return ", ".join(
        "%s(%s)" % (finding.rule, finding.signal or "-") for finding in findings
    )


def run_samples() -> int:
    """Replay every recorded sample; return a process exit code."""
    expected = load_expected()
    failures = 0
    for relative in sorted(expected):
        path = os.path.join(HERE, relative)
        findings = analyse_file(path)
        got_rules = [finding.rule for finding in findings]
        got_signals = [finding.signal for finding in findings]
        want = expected[relative]
        ok = got_rules == list(want["rules"]) and got_signals == list(want["signals"])
        if not ok:
            failures += 1
        print(
            "%s %s -> %s"
            % ("PASS" if ok else "FAIL", relative, describe(findings))
        )
        if not ok:
            print(
                "     expected: rules=%s signals=%s"
                % (want["rules"], want["signals"])
            )
    print(
        "\nSUMMARY: samples=%d passed=%d failed=%d"
        % (len(expected), len(expected) - failures, failures)
    )
    return 0 if failures == 0 else 1


def run_files(paths: list) -> int:
    """Print the findings for ad-hoc files; return a process exit code."""
    for path in paths:
        if not os.path.isfile(path):
            print("MISSING %s" % path)
            return 2
        findings = analyse_file(path)
        print("%s: %d finding(s) -> %s" % (path, len(findings), describe(findings)))
        for finding in findings:
            print(
                "  - %s signal=%s line_hint=%s"
                % (finding.rule, finding.signal, finding.line_hint)
            )
            print("    %s" % finding.title)
            print("    %s" % finding.body.splitlines()[0])
    return 0


def main(argv: list) -> int:
    """Entry point: samples when no path is given, ad-hoc files otherwise."""
    try:  # keep Chinese titles printable on a GBK console
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):  # pragma: no cover - old interpreters
        pass
    if argv:
        return run_files(argv)
    return run_samples()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
