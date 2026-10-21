#!/usr/bin/env python3
"""Run one real OpenAI-compatible chat-completions acceptance probe.

Set the three model variables before running:

    SIGFLOW_EDU_MODEL_API_KEY
    SIGFLOW_EDU_MODEL_BASE_URL
    SIGFLOW_EDU_MODEL_NAME

The probe prints no prompt text, answer text or credential.  It exits 0 only
when the endpoint returns usable teaching prose that passes the same
``safe_teaching_answer`` gate used by the dispatcher.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

AGENT_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(AGENT_DIR / "src"))

from sigflow_edu_agent.dispatch import safe_teaching_answer  # noqa: E402
from sigflow_edu_agent.model_client import (  # noqa: E402
    API_KEY_ENV,
    BASE_URL_ENV,
    MODEL_ENV,
    ModelClient,
    model_status,
)

INSTRUCTION = (
    "用两句话引导一名 FPGA 初学者检查 always @(*) 代码块在条件不成立时"
    "是否漏掉赋值。不要给出完整代码，不要引用本机路径。"
)
EVIDENCE = "rule=RTL_LATCH_INCOMPLETE_BRANCH\nsignal=q\nlines=3-5"


def main() -> int:
    """Run the real endpoint probe and return a process exit code."""
    key = os.environ.get(API_KEY_ENV, "").strip()
    base_url = os.environ.get(BASE_URL_ENV, "").strip()
    model_name = os.environ.get(MODEL_ENV, "").strip()
    if not key:
        print("SKIP: %s is not set" % API_KEY_ENV)
        return 2

    status = model_status()
    print(
        "model_available=%s provider=%s model=%s base_url_configured=%s name_configured=%s"
        % (
            "true" if status.available else "false",
            status.provider or "<none>",
            status.model or "<none>",
            "true" if bool(base_url) else "false",
            "true" if bool(model_name) else "false",
        )
    )
    if not status.available:
        print("FAIL: %s" % status.reason)
        return 1

    answer = ModelClient().complete(INSTRUCTION, EVIDENCE)
    if not answer.ok:
        print("FAIL: %s" % answer.reason)
        return 1
    safe = safe_teaching_answer(answer, "L1")
    if not safe.ok:
        print("FAIL: %s" % safe.reason)
        return 1

    output = "\n".join(
        (
            status.reason,
            status.provider or "",
            status.model or "",
            answer.reason,
            safe.text,
        )
    )
    if key in output:
        print("FAIL: model key appeared in probe output")
        return 1
    print(
        "PASS: real /chat/completions round-trip; answer_chars=%d; key_in_output=false"
        % len(safe.text)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
