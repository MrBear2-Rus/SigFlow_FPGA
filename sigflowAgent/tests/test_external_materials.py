"""Pinned official tool-doc retrieval/context tests; all DBs are temporary."""
from __future__ import annotations

from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import tempfile
import unittest
from unittest.mock import patch
from dataclasses import replace

from sigflow_edu_agent.courses import SQLiteCourseProvider, pack_courses
from sigflow_edu_agent.domain import (CourseBundle, CourseMaterial, Evidence, RunRequest)
from sigflow_edu_agent.context.models import ContextItem
from sigflow_edu_agent.deepseek import DeepSeekConfig, DeepSeekProvider
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.rag.agent_adapter import prepare_course_context, resolve_course_refs
from sigflow_edu_agent.rag.database import DEFAULT_DB_PATH, RESOURCE_ROOT, open_database
from sigflow_edu_agent.rag.evaluation import load_questions, run_evaluation
from sigflow_edu_agent.rag.external_materials import RAW_ROOT
from sigflow_edu_agent.rag.importer import build_index
from sigflow_edu_agent.rag.retriever import retrieve_course_passages


ROOT = Path(__file__).resolve().parents[1]
RESOURCES = RESOURCE_ROOT
SNAPSHOT = Path(os.environ.get("SIGFLOW_YSYX_SNAPSHOT", r"D:\EDA\doc\sources\ysyx\2607"))


class Source:
    def __init__(self, concept="bit_width"):
        self.evidence = Evidence("ev1", "p", "r", "j", concept, True,
                                 "报告观察到一个位宽相关诊断", False, origin="fixture")

    def collect(self, project_id, revision):
        return self.evidence


class ExternalMaterialIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SNAPSHOT.is_dir() or not (SNAPSHOT / "manifest.jsonl").is_file():
            raise unittest.SkipTest("set SIGFLOW_YSYX_SNAPSHOT to the pinned YSYX snapshot for full external-material DB tests")
        parent = ROOT / "tests" / ".rag-tmp"
        parent.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(dir=parent)
        cls.db = Path(cls.temp.name) / "course.sqlite"
        cls.build = build_index(RESOURCES, cls.db, snapshot_dir=SNAPSHOT)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "temp"):
            cls.temp.cleanup()

    def retrieve(self, query, language="zh-CN", top_k=3):
        return retrieve_course_passages(query, language=language, top_k=top_k, db_path=self.db)

    def test_fixed_file_hashes_license_basis_and_exact_ranges(self):
        manifest = [json.loads(line) for line in (RESOURCES / "external_source_files.jsonl").read_text(encoding="utf-8").splitlines()]
        self.assertEqual(len(manifest), 60)
        for row in manifest:
            raw = (RAW_ROOT / Path(*Path(row["source_path"]).parts)).read_bytes()
            self.assertEqual("sha256:" + hashlib.sha256(raw).hexdigest(), row["sha256"], row["source_path"])
            self.assertEqual(len(raw), row["bytes"])
            self.assertEqual(len(raw.decode("utf-8").splitlines()), row["line_count"])
            self.assertEqual(row["retrieved_at_precision"], "date_only; exact network transfer time was not captured")
            self.assertRegex(row["local_file_created_at_utc"], r"^2026-10-13T\d\d:\d\d:\d\dZ$")
            self.assertTrue(row["license_evidence_path"] in {item["source_path"] for item in manifest})
        by_path = {row["source_path"]: row for row in manifest}
        self.assertEqual(by_path["lowrisc/9c15ff5/VerilogCodingStyle.md"]["license_basis"], "explicit_repository_readme")
        self.assertEqual(by_path["yosys/0.47/docs/source/appendix/primer.rst"]["license_basis"], "repository_inherited_judgment")
        self.assertEqual(by_path["yosys/0.47/passes/cmds/check.cc"]["license_basis"], "explicit_file_notice")
        self.assertEqual(by_path["verilator/v5.052/docs/guide/warnings.rst"]["license_basis"], "explicit_file_spdx")
        self.assertEqual(by_path["verilator/v5.052/docs/gen/ex_ALWNEVER_msg.rst"]["license_basis"], "repository_inherited_unconfirmed")
        passages = [json.loads(line) for line in (RESOURCES / "external_official_passages.jsonl").read_text(encoding="utf-8").splitlines()]
        lowrisc_summary = next(item for item in passages if item["passage_id"] == "lowrisc_style:verilog:summary")
        self.assertNotIn("Table of Contents", lowrisc_summary["content"])
        self.assertEqual(lowrisc_summary["character_count"], len(lowrisc_summary["content"]))
        width = next(item for item in passages if item["passage_id"] == "verilator_5052:warnings:widthtrunc")
        self.assertEqual(width["character_count"], len(width["content"]))
        self.assertEqual(width["byte_count"], len(width["content"].encode("utf-8")))
        self.assertEqual(width["review_status"], "official_source_pending_teacher_review")
        self.assertEqual(width["license_basis"], "explicit_file_spdx")
        self.assertEqual(width["source_ranges"][0]["source_path"], "verilator/v5.052/docs/guide/warnings.rst")
        self.assertEqual((width["source_ranges"][0]["line_start"], width["source_ranges"][0]["line_end"]), (2802, 2806))
        alwnever = next(item for item in passages if item["passage_id"] == "verilator_5052:warnings:alwnever")
        self.assertGreater(alwnever["source_location"]["line_start"], 1)
        generated_range = next(item for item in alwnever["source_ranges"] if item["source_path"].endswith("ex_ALWNEVER_msg.rst"))
        self.assertEqual(generated_range["line_start"], 1)
        self.assertEqual(generated_range["license_basis"], "repository_inherited_unconfirmed")
        self.assertEqual(generated_range["source_file_sha256"], by_path[generated_range["source_path"]]["sha256"])
        with closing(open_database(self.db)) as connection:
            self.assertEqual(connection.execute("PRAGMA user_version").fetchone()[0], 4)
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM source_file_provenance").fetchone()[0], 60)
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM passage_mappings").fetchone()[0], 288)
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM passage_translations").fetchone()[0], 25)
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM passages WHERE content_type='authored_note'").fetchone()[0], 9)
            self.assertEqual(connection.execute("SELECT COUNT(DISTINCT content_hash) FROM passages").fetchone()[0],
                             connection.execute("SELECT COUNT(*) FROM passages").fetchone()[0])

        result = self.retrieve("ALWNEVER", "en")
        passage = next(item for item in result["passages"] if item["passage_id"] == "verilator_5052:warnings:alwnever")
        included = [span for span in passage["source_spans"] if "/gen/" in span["source_url"]]
        self.assertTrue(any("ex_ALWNEVER_faulty.rst" in span["source_url"] for span in included))
        self.assertTrue(any("ex_ALWNEVER_msg.rst" in span["source_url"] for span in included))
        for span in included:
            source_path = span["source_url"].split("/blob/", 1)[1].split("/", 1)[1].split("#", 1)[0]
            local = RAW_ROOT / "verilator" / "v5.052" / Path(*Path(source_path).parts)
            lines = local.read_text(encoding="utf-8").splitlines()
            self.assertEqual(span["text"], "\n".join(lines[span["line_start"] - 1:span["line_end"]]).strip("\n"))
        span_rows = [json.loads(line) for line in (RESOURCES / "external_official_spans.jsonl").read_text(encoding="utf-8").splitlines()]
        self.assertFalse(any("_fixed.rst" in span.get("chapter", "") for row in span_rows for span in row["spans"]))

    def test_yosys_help_normalization_keeps_original_cpp_locator(self):
        result = self.retrieve("Yosys check force detailed loop check", "en")
        passage = next(item for item in result["passages"] if item["passage_id"] == "yosys_047:command:check")
        span = passage["source_spans"][0]
        self.assertEqual(span["normalization"], "decoded_cpp_help_log_strings")
        self.assertTrue(span["text"].startswith("check [options] [selection]"))
        self.assertIn("-force-detailed-loop-check", span["text"])
        self.assertIn("log(\"", (RAW_ROOT / "yosys" / "0.47" / "passes" / "cmds" / "check.cc").read_text(encoding="utf-8"))
        self.assertTrue(span["anchor"].startswith("#L31-L"))

    def test_chinese_english_and_warning_code_queries_include_english_sources(self):
        chinese = self.retrieve("为什么这里会有位宽告警？")
        self.assertEqual(chinese["status"], "ok")
        self.assertTrue(any(item["language"] == "en" for item in chinese["passages"]))
        self.assertTrue(any(item["passage_id"].endswith(":widthtrunc") for item in chinese["passages"]))
        colloquial = self.retrieve("Yosys 为什么解析不了这段 Verilog？")
        self.assertTrue(any(item["passage_id"] == "yosys_047:command:read-verilog" for item in colloquial["passages"]))
        english = self.retrieve("What does WIDTHTRUNC mean?", "en")
        self.assertTrue(any(item["passage_id"] == "verilator_5052:warnings:widthtrunc" for item in english["passages"]))
        code = self.retrieve("PINNOCONNECT", "en")
        self.assertTrue(any(item["passage_id"] == "verilator_5052:warnings:pinnoconnect" for item in code["passages"]))

        mappings = [json.loads(line) for line in (RESOURCES / "external_official_mappings.jsonl").read_text(encoding="utf-8").splitlines()]
        self.assertEqual(len(mappings), 288)
        self.assertTrue(all(re.search(r"[\u3400-\u9fff]", item["title_zh"]) for item in mappings))
        read_verilog = next(item for item in mappings if item["passage_id"] == "yosys_047:command:read-verilog")
        self.assertTrue(any("read_verilog" in term for term in read_verilog["terms"]))
        self.assertTrue(read_verilog["questions_zh"])
        self.assertEqual(read_verilog["concept_ids"], ["tool_command"])
        translations = [json.loads(line) for line in (RESOURCES / "external_official_translations.jsonl").read_text(encoding="utf-8").splitlines()]
        translated_help = next(item for item in translations if item["passage_id"] == "yosys_047:command:read-verilog")
        self.assertIn("-formal", translated_help["translated_text"])
        self.assertIn("-nosynthesis", translated_help["translated_text"])
        self.assertIn("错误报告能力较弱", translated_help["translated_text"])

    def test_official_topic_mappings_headings_and_warning_labels(self):
        passages = [json.loads(line) for line in (RESOURCES / "external_official_passages.jsonl").read_text(encoding="utf-8").splitlines()]
        by_id = {item["passage_id"]: item for item in passages}
        self.assertTrue({
            "lowrisc_style:verilog:sequential-logic-latches",
            "lowrisc_style:verilog:combinational-logic",
            "lowrisc_style:verilog:blocking-and-non-blocking-assignments",
            "yosys_047:primer:behavioural-modelling",
        } <= set(by_id))
        self.assertEqual(by_id["lowrisc_style:verilog:sequential-logic-latches"]["concept_ids"],
                         ["latch", "blocking_nonblocking", "sequential_logic"])
        self.assertIn("combinational_logic", by_id["lowrisc_style:verilog:combinational-logic"]["concept_ids"])
        self.assertIn("blocking_nonblocking", by_id["lowrisc_style:verilog:blocking-and-non-blocking-assignments"]["concept_ids"])
        self.assertIn("combinational_logic", by_id["yosys_047:primer:behavioural-modelling"]["concept_ids"])
        self.assertIn("blocking_nonblocking", by_id["yosys_047:primer:behavioural-modelling"]["concept_ids"])

        spans = [json.loads(line) for line in (RESOURCES / "external_official_spans.jsonl").read_text(encoding="utf-8").splitlines()]
        heading_by_id = {item["passage_id"]: item["spans"][0]["heading_path"] for item in spans}
        self.assertEqual(heading_by_id["lowrisc_style:verilog:sequential-logic-latches"],
                         ["lowRISC Verilog Coding Style Guide", "Language Features", "Sequential Logic (Latches)"])
        self.assertEqual(heading_by_id["lowrisc_style:verilog:combinational-logic"],
                         ["lowRISC Verilog Coding Style Guide", "Language Features", "Combinational Logic"])
        self.assertEqual(heading_by_id["yosys_047:primer:behavioural-modelling"],
                         ["A primer on digital circuit synthesis", "Features of synthesizable Verilog", "Behavioural modelling"])

        with closing(open_database(self.db)) as connection:
            warning_rows = connection.execute(
                "SELECT title, concept_ids_json FROM passages "
                "WHERE source_id='verilator_5052' AND passage_id LIKE 'verilator_5052:warnings:%'"
            ).fetchall()
            width_titles = {row["title"] for row in warning_rows if "bit_width" in json.loads(row["concept_ids_json"])}
            self.assertEqual(width_titles, {"WIDTH", "WIDTHCONCAT", "WIDTHEXPAND", "WIDTHTRUNC", "WIDTHXZEXPAND", "ENUMITEMWIDTH"})
            concepts_by_title = {row["title"]: set(json.loads(row["concept_ids_json"])) for row in warning_rows}
            self.assertNotIn("bit_width", concepts_by_title["LATCH"])
            self.assertNotIn("syntax_error", concepts_by_title["UNUSEDSIGNAL"])
            self.assertNotIn("multiple_driver", concepts_by_title["UNDRIVEN"])
            self.assertNotIn("reset", concepts_by_title["MULTIDRIVEN"])
            self.assertNotIn("latch", concepts_by_title["DIDNOTCONVERGE"])
            self.assertIn("combinational_logic", concepts_by_title["UNOPTFLAT"])

    def test_retrieval_quality_regressions_and_named_source_gaps(self):
        dataset = ROOT / "tests" / "evaluation" / "course_retrieval_quality_2026-10-14.jsonl"
        report = run_evaluation(dataset, self.db)
        expected_by_case = {item["case"]: item["acceptable_passage_ids"] for item in load_questions(dataset)}
        self.assertEqual(report["hit_at_3"], 1.0)
        self.assertEqual(report["recall_at_10"], 1.0)
        self.assertGreaterEqual(report["mrr_at_3"], 0.75)
        self.assertEqual(report["required_source_coverage"], 1.0)
        self.assertEqual(report["false_positive_negative_queries"], 0)
        for row in report["results"]:
            if row["expected_status"] == "match":
                self.assertTrue(set(expected_by_case[row["case"]]) <= set(row["ranked_passage_ids_top10"]), row["case"])
        negative = next(row for row in report["results"] if row["case"] == "negative_named_sources")
        self.assertEqual(negative["retrieved_passage_ids"], [])
        self.assertEqual(set(negative["missing_sources"]), {"lowrisc_style", "yosys_047"})

        result = self.retrieve("Yosys primer 的组合逻辑示例为何使用 <=，lowRISC 要求 =？", "zh-CN", top_k=3)
        self.assertLessEqual(len(result["passages"]), 3)
        self.assertGreaterEqual(len(result["candidate_rankings"]), len(result["passages"]))
        self.assertEqual({item["status"] for item in result["source_coverage"]}, {"covered"})

    def test_official_source_precedes_authored_note_and_versions_stay_separate(self):
        result = self.retrieve("Yosys check -force-detailed-loop-check", "en")
        self.assertEqual(result["passages"][0]["passage_id"], "yosys_047:command:check")
        self.assertNotIn("yosys_047:checks:driver-loop-undriven", result["retrieved_passage_ids"])
        with closing(open_database(self.db)) as connection:
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM passages WHERE content_type='authored_note'").fetchone()[0], 9)
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM passages WHERE passage_id='yosys_047:checks:driver-loop-undriven' AND content_type='authored_note'").fetchone()[0], 1)
        prepared = prepare_course_context("Yosys check [options] [selection]", db_path=self.db)
        ref = next(item.reference for item in prepared.passages if item.reference.passage_id == "yosys_047:command:check")
        self.assertTrue(ref.course_version.startswith("Yosys 0.47@"))
        self.assertNotIn("Yosys 0.49", ref.course_version)
        self.assertIn("Yosys 0.47", next(item.text for item in prepared.passages if item.reference.passage_id == ref.passage_id))

    def test_unsupported_concept_no_match_and_forged_citation_rejection(self):
        unsupported = self.retrieve("two's complement", "en")
        self.assertEqual(unsupported["status"], "no_match")
        self.assertIn("twos_complement", unsupported["unsupported_concept_ids"])
        none = self.retrieve("violet orchestra questionnaire", "en")
        self.assertEqual(none["status"], "no_match")
        prepared = prepare_course_context("Why WIDTHTRUNC?", db_path=self.db)
        admitted = [item.reference.passage_id for item in prepared.passages]
        with self.assertRaises(ValueError):
            resolve_course_refs(prepared, ["forged:warning:invented"], admitted_passage_ids=admitted)
        resolved = resolve_course_refs(prepared, admitted[:1], admitted_passage_ids=admitted)
        self.assertEqual(resolved[0].passage_id, admitted[0])

    def test_translation_context_is_labeled_linked_and_not_duplicated_with_english(self):
        prepared = prepare_course_context("为什么这里会有位宽告警？", db_path=self.db)
        translated = next(item for item in prepared.passages if item.reference.passage_id == "verilator_5052:warnings:widthtrunc")
        self.assertEqual(translated.reference.material_kind, "translated_source")
        self.assertEqual(translated.reference.translation_id, "zh-CN:verilator_5052:warnings:widthtrunc:v1")
        self.assertTrue(translated.reference.source_content_hash.startswith("sha256:"))
        self.assertIn("未教师审阅", translated.reference.material_note)
        self.assertIn("不是官方译文", translated.reference.material_note)
        self.assertIn("中文检索标题（仓库映射，非官方标题）: Verilator v5.052 告警 WIDTHTRUNC｜位宽截断", translated.text)
        self.assertIn("原文行范围: 2802-2806", translated.text)
        self.assertIn(".. option:: WIDTHTRUNC", translated.text)
        self.assertNotIn("A more granular", translated.text)

    def test_complete_context_chain_mocked_model_cites_only_admitted_passage(self):
        source = Source()
        candidate = {"level": 1, "hint": "请先确认诊断指出的是截断还是扩展。", "question": "被赋值一侧和目标信号各是多少位？",
                     "evidence_ids": ["ev1"], "course_passage_ids": ["verilator_5052:warnings:widthtrunc"]}
        with closing(sqlite3.connect(self.db)) as connection:
            candidate["course_passage_ids"]=[connection.execute(
                "SELECT chunk_id FROM evidence_chunks WHERE parent_id=? ORDER BY ordinal",
                ("verilator_5052:warnings:widthtrunc",)).fetchone()[0]]
        with patch("sigflow_edu_agent.deepseek.http.client.HTTPSConnection") as connection_factory:
            reply = connection_factory.return_value.getresponse.return_value
            reply.status = 200
            reply.read.return_value = json.dumps({"choices": [{"finish_reason": "stop", "message": {"content": json.dumps(candidate, ensure_ascii=False)}}]}).encode()
            provider = DeepSeekProvider(DeepSeekConfig("test-key", "test-model"))
            loop = TeachingLoop(RunRequest("p", "r", "issue", "为什么这里会有位宽告警？"), source, provider,
                                course_provider=SQLiteCourseProvider(self.db))
            state = loop.run_until_pause()
            request_body = connection_factory.return_value.request.call_args.kwargs["body"]
        payload = json.loads(request_body)
        context = json.loads(payload["messages"][1]["content"])
        self.assertEqual(state.course_status, "partial")
        self.assertEqual(state.course_diagnostics["retrieval_status"], "ok")
        self.assertTrue(state.course_diagnostics["omission_counts"] or
                        state.course_diagnostics["candidate_not_selected_count"])
        self.assertEqual(state.course_diagnostics["answerability"], "not_assessed")
        self.assertEqual([ref.passage_id for ref in state.cards[0].course_refs], [candidate["course_passage_ids"][0]])
        self.assertIn("translated_source", request_body.decode("utf-8"))
        self.assertIn("未教师审阅", request_body.decode("utf-8"))
        locator=state.cards[0].course_refs[0].source_spans[0]
        self.assertEqual(locator.line_start,2802)
        self.assertLessEqual(locator.line_end,2806)
        self.assertIn(f"#L{locator.line_start}-L{locator.line_end}", request_body.decode("utf-8"))
        self.assertNotIn("A more granular", request_body.decode("utf-8"))
        self.assertNotIn("local_path", request_body.decode("utf-8"))
        course_items = [item for item in context["evidence"]["context_items"] if item["kind"] == "course"]
        self.assertLessEqual(len(course_items), 8)
        self.assertGreaterEqual(len(course_items), 1)
        self.assertLessEqual(sum(len(item["text"].encode("utf-8")) for item in context["evidence"]["context_items"]), 8192)
        self.assertLessEqual(len(request_body), DeepSeekProvider.MAX_REQUEST_BYTES)

    def test_overlong_course_is_omitted_whole_with_reason_and_no_reference(self):
        bundle = SQLiteCourseProvider(self.db).prepare("为什么这里会有位宽告警？", ("bit_width",))
        evidence = Source().evidence
        engineer_item = ContextItem("report", "report", "p", "r", "工" * 2700)
        evidence = replace(evidence, context_items=(engineer_item,))
        packed, admitted, status, omitted = pack_courses(evidence, bundle)
        self.assertEqual(status, "omitted")
        self.assertEqual(admitted, ())
        self.assertEqual(packed.context_items, (engineer_item,))
        self.assertEqual(len(omitted), len(bundle.materials))
        self.assertTrue(all(item.reason == "engineering_over_budget" for item in omitted))


if __name__ == "__main__":
    unittest.main()
