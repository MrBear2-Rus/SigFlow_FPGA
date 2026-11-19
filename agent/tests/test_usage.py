"""Unit tests for the bounded local usage store.

The store is an enhancement: it must aggregate only counters, survive corrupt
or oversized lines, and never take the service down when its directory is
unavailable.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src"))

from sigflow_edu_agent.usage import UsageStore  # noqa: E402

NOW = datetime(2026, 11, 17, 12, 0, tzinfo=timezone.utc)


def _iso(days_ago: int = 0) -> str:
    moment = NOW - timedelta(days=days_ago)
    return moment.replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _record_line(
    days_ago: int = 0,
    *,
    model: str = "deepseek-chat",
    ok: bool = True,
    prompt: int = 10,
    completion: int = 5,
    total: int = 15,
    unavailable: bool = False,
) -> str:
    return json.dumps(
        {
            "ts": _iso(days_ago),
            "model": model,
            "ok": ok,
            "prompt_tokens": prompt,
            "completion_tokens": completion,
            "total_tokens": total,
            "tokens_unavailable": unavailable,
        },
        separators=(",", ":"),
    )


class UsageStoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self._temporary.cleanup)
        self.root = Path(self._temporary.name)

    def store(self) -> UsageStore:
        return UsageStore(str(self.root))

    def log_path(self) -> Path:
        return self.root / "usage" / "usage.jsonl"

    def write_lines(self, *lines: str) -> None:
        path = self.log_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")

    def test_record_and_summarize_counters(self) -> None:
        store = self.store()
        with mock.patch("sigflow_edu_agent.usage._utc_now", return_value=NOW):
            store.record(
                model="deepseek-chat",
                ok=True,
                usage={"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
            )
            store.record(model="deepseek-chat", ok=False, usage=None)
        summary = store.summarize(30, now=NOW)
        self.assertEqual(summary["schema"], "edu.usage.v1")
        self.assertEqual(summary["since"], "2026-11-17")
        self.assertEqual(
            summary["totals"],
            {
                "calls": 2,
                "failed_calls": 1,
                "prompt_tokens": 10,
                "completion_tokens": 5,
                "total_tokens": 15,
                "tokens_unavailable_calls": 1,
            },
        )
        self.assertEqual(
            summary["daily"],
            [
                {
                    "date": "2026-11-17",
                    "calls": 2,
                    "failed_calls": 1,
                    "prompt_tokens": 10,
                    "completion_tokens": 5,
                    "total_tokens": 15,
                }
            ],
        )

    def test_daily_window_does_not_change_cumulative_totals(self) -> None:
        self.write_lines(
            _record_line(40, prompt=100, completion=50, total=150),
            _record_line(2, prompt=3, completion=4, total=7),
        )
        summary = self.store().summarize(30, now=NOW)
        self.assertEqual(summary["since"], _iso(40)[:10])
        self.assertEqual(summary["totals"]["calls"], 2)
        self.assertEqual(summary["totals"]["total_tokens"], 157)
        self.assertEqual([day["date"] for day in summary["daily"]], [_iso(2)[:10]])
        self.assertEqual(summary["daily"][0]["total_tokens"], 7)

    def test_corrupt_lines_are_skipped_and_reported(self) -> None:
        store = self.store()
        self.write_lines(
            "{not json}",
            _record_line(0),
            json.dumps({"ts": "not-a-time"}),
            "",
        )
        summary = store.summarize(30, now=NOW)
        self.assertEqual(summary["skipped"], 2)
        self.assertEqual(summary["totals"]["calls"], 1)

    def test_initialization_prunes_expired_and_malformed_lines(self) -> None:
        self.write_lines(_record_line(100), "{bad}", _record_line(1))
        store = self.store()
        raw = self.log_path().read_text(encoding="utf-8")
        self.assertNotIn("{bad}", raw)
        self.assertEqual(raw.count("\n"), 1)
        summary = store.summarize(30, now=NOW)
        self.assertEqual(summary["skipped"], 0)
        self.assertEqual(summary["totals"]["calls"], 1)

    def test_disabled_store_is_a_safe_no_op(self) -> None:
        store = UsageStore(None)
        self.assertFalse(store.enabled)
        store.record(model="deepseek-chat", ok=True, usage=None)
        summary = store.summarize(30, now=NOW)
        self.assertEqual(summary["totals"]["calls"], 0)
        self.assertIsNone(summary["since"])
        self.assertEqual(summary["daily"], [])

    def test_unusable_directory_disables_accounting(self) -> None:
        file_path = self.root / "not-a-directory"
        file_path.write_text("x", encoding="utf-8")
        store = UsageStore(str(file_path))
        self.assertFalse(store.enabled)
        store.record(model="deepseek-chat", ok=True, usage=None)

    def test_records_contain_only_counters(self) -> None:
        store = self.store()
        with mock.patch("sigflow_edu_agent.usage._utc_now", return_value=NOW):
            store.record(
                model="deepseek-chat",
                ok=True,
                usage={"prompt_tokens": 1, "completion_tokens": 2, "total_tokens": 3},
            )
        record = json.loads(self.log_path().read_text(encoding="utf-8").strip())
        self.assertEqual(
            set(record),
            {
                "ts",
                "model",
                "ok",
                "prompt_tokens",
                "completion_tokens",
                "total_tokens",
                "tokens_unavailable",
            },
        )
        self.assertNotIn("prompt", record)
        self.assertNotIn("instruction", record)

    def test_invalid_token_values_become_unavailable(self) -> None:
        store = self.store()
        with mock.patch("sigflow_edu_agent.usage._utc_now", return_value=NOW):
            store.record(
                model="deepseek-chat",
                ok=True,
                usage={"prompt_tokens": True, "completion_tokens": -1, "total_tokens": 10**12},
            )
        summary = store.summarize(30, now=NOW)
        self.assertEqual(summary["totals"]["calls"], 1)
        self.assertEqual(summary["totals"]["tokens_unavailable_calls"], 1)
        self.assertEqual(summary["totals"]["total_tokens"], 0)

    def test_unsafe_model_label_is_not_persisted_verbatim(self) -> None:
        store = self.store()
        with mock.patch("sigflow_edu_agent.usage._utc_now", return_value=NOW):
            store.record(model="bad\nmodel", ok=True, usage=None)
        record = json.loads(self.log_path().read_text(encoding="utf-8").strip())
        self.assertEqual(record["model"], "unknown")

    def test_summary_days_are_clamped(self) -> None:
        store = self.store()
        self.assertEqual(store.summarize(0, now=NOW)["days"], 1)
        self.assertEqual(store.summarize(999, now=NOW)["days"], 90)

    def test_prune_keeps_recent_tail_within_size_budget(self) -> None:
        store = UsageStore(str(self.root), max_file_bytes=4096)
        self.write_lines(*[_record_line(0) for _ in range(200)])
        with mock.patch("sigflow_edu_agent.usage._utc_now", return_value=NOW):
            store.record(model="deepseek-chat", ok=True, usage=None)
        self.assertLessEqual(self.log_path().stat().st_size, 4096 + 4096)


if __name__ == "__main__":
    unittest.main()
