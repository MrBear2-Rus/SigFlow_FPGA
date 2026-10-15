from __future__ import annotations

import json
import shutil
import sqlite3
from contextlib import closing
import unittest
from pathlib import Path

from sigflow_edu_agent.rag.citations import CitationValidationError, render_citations, validate_citations
from sigflow_edu_agent.rag.evaluation import run_evaluation
from sigflow_edu_agent.rag.database import open_database
from sigflow_edu_agent.rag.importer import ImportValidationError, build_index, content_hash, DEFAULT_RESOURCES_DIR
from sigflow_edu_agent.rag.retriever import format_agent_context, retrieve_course_passages
import os
SNAPSHOT_DIR = Path(os.environ.get('SIGFLOW_COURSE_SNAPSHOT', str(Path(__file__).resolve().parents[4] / 'doc/sources/ysyx/2607')))



PROJECT_ROOT = Path(__file__).resolve().parents[2]
RESOURCE_ROOT = DEFAULT_RESOURCES_DIR


@unittest.skipUnless((SNAPSHOT_DIR / "manifest.jsonl").is_file(), "Set SIGFLOW_COURSE_SNAPSHOT for local original-text regression")
class CourseKnowledgeBaseTests(unittest.TestCase):
    def setUp(self) -> None:
        test_root = PROJECT_ROOT / "tests" / ".rag-tmp"
        test_root.mkdir(parents=True, exist_ok=True)
        self.root = test_root / self._testMethodName
        if self.root.exists():
            shutil.rmtree(self.root)
        self.root.mkdir()
        self.resources = self.root / "resources"
        shutil.copytree(RESOURCE_ROOT, self.resources)
        self.db_path = self.root / "index" / "course.sqlite"

    def tearDown(self) -> None:
        shutil.rmtree(self.root)
        test_root = self.root.parent
        if test_root.exists() and not any(test_root.iterdir()):
            test_root.rmdir()

    def _build(self) -> dict:
        return build_index(self.resources, self.db_path, snapshot_dir=SNAPSHOT_DIR)

    def _passages_file(self) -> Path:
        return self.resources / "passages" / "ysyx.jsonl"

    def _read_passages(self) -> list[dict]:
        return [json.loads(line) for line in self._passages_file().read_text(encoding="utf-8").splitlines() if line.strip()]

    def _write_passages(self, rows: list[dict]) -> None:
        self._passages_file().write_text(
            "".join(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n" for row in rows),
            encoding="utf-8",
        )

    def test_importer_builds_complete_seed_and_fts_schema(self) -> None:
        result = self._build()
        self.assertEqual(result["passage_count"], 554)
        self.assertEqual(result["registered_passage_count"], 599)
        self.assertEqual(result["passage_count"], 554)
        self.assertEqual(result["excluded_partial_count"], 45)
        self.assertEqual(result["practice_count"], 9)
        self.assertEqual(result["fts_tokenizer"], "trigram")
        connection = sqlite3.connect(self.db_path)
        try:
            tables = {row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type IN ('table','view')")}
            self.assertTrue({"sources", "concepts", "concept_aliases", "passages", "passage_sources", "source_documents", "passage_spans", "passage_concepts", "practices", "practice_concepts", "passage_fts", "index_meta", "source_file_provenance", "passage_mappings", "passage_translations"}.issubset(tables))
            self.assertEqual(connection.execute("PRAGMA user_version").fetchone()[0], 4)
        finally:
            connection.close()

    def test_hash_is_verified_against_stored_content(self) -> None:
        self._build()
        row = next(row for row in self._read_passages() if row["passage_id"] == "ysyx:latch:level-sensitive-hold")
        self.assertEqual(row["content_hash"], content_hash(row["content"]))
        self.assertTrue(row["content_hash"].startswith("sha256:"))

    def test_duplicate_passage_id_is_rejected(self) -> None:
        rows = self._read_passages()
        self._write_passages(rows + [dict(rows[0])])
        with self.assertRaisesRegex(ImportValidationError, "duplicate passage_id"):
            self._build()

    def test_unknown_source_is_rejected(self) -> None:
        rows = self._read_passages()
        rows[0]["source_id"] = "missing"
        self._write_passages(rows)
        with self.assertRaisesRegex(ImportValidationError, "unknown source_id"):
            self._build()

    def test_unknown_concept_is_rejected(self) -> None:
        rows = self._read_passages()
        rows[0]["concept_ids"].append("not_in_map")
        self._write_passages(rows)
        with self.assertRaisesRegex(ImportValidationError, "unknown concept_id"):
            self._build()

    def test_content_hash_mismatch_is_rejected(self) -> None:
        rows = self._read_passages()
        rows[0]["content"] += "内容被改但 hash 未更新。"
        self._write_passages(rows)
        with self.assertRaisesRegex(ImportValidationError, "content_hash mismatch"):
            self._build()

    def test_concept_search_has_priority_and_top_k_is_capped(self) -> None:
        self._build()
        result = retrieve_course_passages(
            "D 锁存器关闭后如何保持输出",
            concept_ids=["latch"],
            top_k=3,
            db_path=self.db_path,
        )
        self.assertEqual(result["status"], "ok")
        self.assertLessEqual(len(result["passages"]), 3)
        self.assertEqual(result["passages"][0]["passage_id"], "ysyx:latch:level-sensitive-hold")
        self.assertEqual(result["passages"][0]["match_reason"], "exact_concept")
        self.assertNotIn("ysyx:latch:complete-assignment", result["retrieved_passage_ids"])

    def test_query_aliases_map_without_explicit_concepts(self) -> None:
        self._build()
        result = retrieve_course_passages("锁存器关闭后输出会怎样？", db_path=self.db_path)
        self.assertIn("latch", result["matched_concept_ids"])
        self.assertIn("ysyx:latch:level-sensitive-hold", result["retrieved_passage_ids"])

    def test_fts_text_search_works_without_concept_hint(self) -> None:
        self._build()
        result = retrieve_course_passages(
            "不同合法处理顺序会得到不同结果",
            concept_ids=[],
            top_k=3,
            db_path=self.db_path,
        )
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["passages"][0]["passage_id"], "ysyx:simulation:event-order-and-races")
        self.assertEqual(result["passages"][0]["match_reason"], "fts_text")

    def test_top_k_greater_than_three_is_rejected(self) -> None:
        self._build()
        with self.assertRaisesRegex(ValueError, "between 1 and 3"):
            retrieve_course_passages("锁存器", top_k=4, db_path=self.db_path)

    def test_unknown_query_returns_no_match_and_no_practices(self) -> None:
        self._build()
        result = retrieve_course_passages("Krita alpha channel export", concept_ids=[], db_path=self.db_path)
        self.assertEqual(result["status"], "no_match")
        self.assertEqual(result["passages"], [])
        self.assertEqual(result["related_practices"], [])

    def test_citations_are_limited_to_this_retrieval(self) -> None:
        self._build()
        result = retrieve_course_passages("D latch 锁存器", concept_ids=["latch"], db_path=self.db_path)
        self.assertTrue(validate_citations(result["retrieved_passage_ids"], result["retrieved_passage_ids"]))
        with self.assertRaises(CitationValidationError):
            validate_citations(["ysyx:not:retrieved"], result["retrieved_passage_ids"])

    def test_citation_url_and_cross_source_refs_come_from_sqlite(self) -> None:
        self._build()
        result = retrieve_course_passages("D latch 锁存器", concept_ids=["latch"], db_path=self.db_path)
        passage = result["passages"][0]
        with closing(open_database(self.db_path)) as connection:
            expected_url = connection.execute("SELECT source_url FROM passages WHERE passage_id=?",
                                              (passage["passage_id"],)).fetchone()[0]
            expected_span = connection.execute("SELECT document_id,line_start,line_end,text_hash FROM passage_spans WHERE passage_id=? ORDER BY span_order",
                                               (passage["passage_id"],)).fetchone()
        passage["source_url"] = "https://attacker.invalid/fake"
        rendered = render_citations([passage["passage_id"]], result, db_path=self.db_path)[0]
        self.assertEqual(rendered["source_url"], expected_url)
        span = rendered["source_spans"][0]
        self.assertEqual((span["document_id"],span["line_start"],span["line_end"],span["text_hash"]),tuple(expected_span))

    def test_practices_are_separate_and_use_verified_metadata(self) -> None:
        self._build()
        result = retrieve_course_passages("四位计数器 counter", concept_ids=["counter"], db_path=self.db_path)
        self.assertTrue(result["passages"])
        self.assertTrue(result["related_practices"])
        practice = result["related_practices"][0]
        self.assertEqual(practice["source"], "hdlbits")
        self.assertEqual(practice["source_url"], "https://hdlbits.01xz.net/wiki/Count15")
        self.assertNotIn("content", practice)

    def test_index_can_be_deleted_and_rebuilt(self) -> None:
        first = self._build()
        self.db_path.unlink()
        second = self._build()
        self.assertEqual(first["index_version"], second["index_version"])
        self.assertTrue(self.db_path.exists())
        self.assertEqual(list(self.db_path.parent.iterdir()), [self.db_path])


    def test_source_text_is_exact_snapshot_slice_through_sqlite_and_context(self) -> None:
        built = self._build()
        self.assertEqual((built["passage_count"], built["excluded_partial_count"]), (554, 45))
        result = retrieve_course_passages("阻塞赋值和非阻塞赋值有什么区别", db_path=self.db_path)
        passage = result["passages"][0]
        self.assertEqual(passage["passage_id"], "ysyx:simulation:blocking-nonblocking")
        span = passage["source_spans"][0]
        lines = (SNAPSHOT_DIR / "clean" / "D6.md").read_text(encoding="utf-8").splitlines()
        self.assertEqual(span["text"], "\n".join(lines[span["line_start"] - 1:span["line_end"]]))
        connection = sqlite3.connect(self.db_path)
        try:
            stored = connection.execute("SELECT text FROM passage_spans WHERE passage_id = ? AND span_order = 0", (passage["passage_id"],)).fetchone()[0]
        finally:
            connection.close()
        self.assertEqual(stored, span["text"])
        self.assertIn(span["text"], result["agent_context"])
        self.assertNotIn(passage["reviewed_summary"], result["agent_context"])
        self.assertEqual(format_agent_context(result), result["agent_context"])
        citation = render_citations([passage["passage_id"]], result, db_path=self.db_path)[0]
        self.assertEqual(citation["source_spans"][0]["line_start"], span["line_start"])

    def test_authored_note_keeps_identity_without_displacing_official_context(self) -> None:
        self._build()
        result = retrieve_course_passages(
            "Verilog signed unsigned 位宽 符号扩展",
            concept_ids=["bit_width", "signed_unsigned", "truncation_extension"],
            course_id="fpga-verilog",
            db_path=self.db_path,
        )
        self.assertTrue(any(item["content_type"] == "source_excerpt" for item in result["passages"]))
        self.assertNotIn("lowrisc_style:verilog:width-signed", result["retrieved_passage_ids"])
        connection = sqlite3.connect(self.db_path)
        try:
            note = connection.execute(
                "SELECT content_type, evidence_status, source_id FROM passages WHERE passage_id = ?",
                ("lowrisc_style:verilog:width-signed",),
            ).fetchone()
            self.assertEqual(note, ("authored_note", "grounded", "lowrisc_style"))
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM passages WHERE content_type = 'authored_note'").fetchone()[0], 9)
        finally:
            connection.close()

        xz_result = retrieve_course_passages(
            "Verilator 的 X Z 二态语义有哪些限制",
            course_id="fpga-verilog",
            db_path=self.db_path,
        )
        self.assertEqual(xz_result["passages"][0]["passage_id"], "verilator_5052:semantics:two-state-xz")

    def test_partial_evidence_is_not_passed_to_agent(self) -> None:
        self._build()
        result = retrieve_course_passages("补码有符号数范围", concept_ids=["twos_complement"], db_path=self.db_path)
        self.assertEqual(result["status"], "no_match")
        self.assertNotIn("ysyx:signed:twos-complement-range", result["retrieved_passage_ids"])
        self.assertEqual(result["agent_context"], "")
        latch_result = retrieve_course_passages("为什么组合逻辑分支没赋值会锁存", concept_ids=["complete_assignment", "latch"], db_path=self.db_path)
        self.assertEqual(latch_result["status"], "ok")
        self.assertNotIn("lowrisc_style:verilog:complete-assignment", latch_result["retrieved_passage_ids"])
        self.assertTrue(any(item["content_type"] == "source_excerpt" for item in latch_result["passages"]))
        connection = sqlite3.connect(self.db_path)
        try:
            status = connection.execute("SELECT evidence_status FROM passages WHERE passage_id = ?", ("ysyx:signed:twos-complement-range",)).fetchone()
        finally:
            connection.close()
        self.assertIsNone(status)

    def test_mutated_snapshot_is_rejected(self) -> None:
        snapshot = self.root / "snapshot"
        shutil.copytree(SNAPSHOT_DIR, snapshot)
        clean = snapshot / "clean" / "F3.md"
        clean.write_text(clean.read_text(encoding="utf-8") + "changed", encoding="utf-8")
        with self.assertRaisesRegex(ImportValidationError, "snapshot hash mismatch"):
            build_index(self.resources, self.db_path, snapshot_dir=snapshot)

    def test_registered_sections_cover_every_body_line_once(self) -> None:
        catalog = [json.loads(line) for line in (RESOURCE_ROOT / "ysyx_section_catalog.jsonl").read_text(encoding="utf-8").splitlines()]
        self.assertEqual(len(catalog), 290)
        for chapter in {row["chapter"] for row in catalog}:
            entries = [row for row in catalog if row["chapter"] == chapter]
            end = max(row["line_end"] for row in entries)
            covered = [sum(row["line_start"] <= number <= row["line_end"] for row in entries) for number in range(13, end + 1)]
            self.assertTrue(all(count == 1 for count in covered), chapter)
            lines = (SNAPSHOT_DIR / "clean" / f"{chapter}.md").read_text(encoding="utf-8").splitlines()
            self.assertTrue(all(not line.strip() or line.strip() in {"[", "]"} for line in lines[end:]), chapter)


    def test_development_evaluation_includes_no_match_case(self) -> None:
        self._build()
        report = run_evaluation(db_path=self.db_path)
        self.assertEqual(report["question_count"], 10)
        self.assertEqual(report["no_match_question_count"], 1)
        self.assertEqual(report["no_match_correct"], 1)
        self.assertGreaterEqual(report["hit_at_3"], 0.85)


if __name__ == "__main__":
    unittest.main()
