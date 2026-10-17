"""T6 acceptance tests for TeachingCard and PlanCard construction."""

from __future__ import annotations

import json
import os
import sys
import unittest
from copy import deepcopy

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_DIR = os.path.dirname(TESTS_DIR)
SRC_DIR = os.path.join(AGENT_DIR, "src")
if SRC_DIR not in sys.path:
    sys.path.insert(0, SRC_DIR)

from sigflow_edu_agent.cards import (  # noqa: E402
    PlanStep,
    build_diagnostic_card,
    build_plan_card,
    build_reference_solution_card,
    build_rule_card,
    build_teaching_card,
    cap_cards,
    is_known_card_kind,
    validate_plan_card,
)
from sigflow_edu_agent.rules import Selection, analyse  # noqa: E402
from sigflow_edu_agent.version import MAX_CARDS_PER_RUN  # noqa: E402

LATCH_SNIPPET = (
    "module blk(input clk, input en, input d, output reg q);\n"
    "  always @(*) begin\n"
    "    if (en)\n"
    "      q = d;\n"
    "  end\n"
    "endmodule\n"
)


def _selection() -> Selection:
    """Return a bounded selection with opaque evidence IDs."""
    return Selection(
        text=LATCH_SNIPPET,
        source_id="source-000000000000000000000001",
        node_id="node-1",
        start_line=1,
        end_line=6,
    )


def _rule_card() -> dict[str, object]:
    """Build the deterministic rule-card sample."""
    selection = _selection()
    finding = analyse(selection)[0]
    return build_rule_card("card-1", "L1", finding, selection, "7")


def _teaching_card() -> dict[str, object]:
    """Build a teaching-card sample with nested evidence."""
    return build_teaching_card(
        card_id="card-1",
        level="L2",
        title="条件覆盖说明",
        body="先检查每条控制路径是否都为输出赋了值。",
        evidence=[
            {
                "kind": "source_ref",
                "ref": {
                    "source_id": "source-1",
                    "start_line": 1,
                    "end_line": 6,
                    "metadata": {"language": "verilog"},
                },
                "note": "所选片段",
            }
        ],
        state_version="8",
        limitations=["只解释当前证据。"],
    )


def _diagnostic_card() -> dict[str, object]:
    """Build a diagnostic-explanation sample."""
    return build_diagnostic_card(
        card_id="card-1",
        level="L1",
        diagnostic={
            "id": "diag-1",
            "code": "EDU_LATCH_INFERRED",
            "raw_code": "LATCH",
            "severity": "warning",
            "stage": "synth",
            "summary": "A latch was inferred.",
            "origin": "core",
            "confidence_kind": "tool",
            "location": {"file": "rtl/top.v", "line": 7},
        },
        report_ref={
            "job_id": "job-1",
            "completeness": "complete",
            "metadata": {"origin": "core"},
        },
        state_version="9",
    )


def _plan_card() -> dict[str, object]:
    """Build a valid three-capability PlanCard."""
    return build_plan_card(
        plan_id="plan-1",
        goal="在不可变快照上复现并定位问题",
        steps=[
            PlanStep(
                "s1",
                "eda.synth",
                "综合当前快照",
                params={"top_module": "top", "strategy": "area"},
            ),
            PlanStep("s2", "eda.sim.build", "构建仿真", depends_on=("s1",)),
            PlanStep(
                "s3",
                "eda.sim.run",
                "运行仿真",
                params={"build_step_id": "s2", "duration_ticks": "1000"},
                depends_on=("s2",),
            ),
        ],
        revision="rev-1",
        state_version="10",
    )


class CardKindTests(unittest.TestCase):
    """Acceptance: every frozen card kind has a working builder."""

    def test_each_card_kind_is_built_with_a_decimal_state_version(self) -> None:
        cards = {
            "rule": _rule_card(),
            "teaching": _teaching_card(),
            "diagnostic_explanation": _diagnostic_card(),
            "reference_solution": build_reference_solution_card(
                "card-1",
                "L4",
                "参考解要点",
                "先补齐所有赋值路径，再重新综合验证。",
                [
                    {
                        "kind": "source_ref",
                        "ref": {"source_id": "source-1"},
                        "note": "L4 证据",
                    }
                ],
                "11",
            ),
        }
        for expected_kind, card in cards.items():
            self.assertEqual(card["kind"], expected_kind)
            self.assertTrue(is_known_card_kind(expected_kind))
            self.assertIsInstance(card["state_version"], str)
            self.assertRegex(card["state_version"], r"^[0-9]+$")

    def test_reference_solution_is_rejected_below_l4(self) -> None:
        with self.assertRaises(ValueError):
            build_reference_solution_card(
                "card-1",
                "L3",
                "错误级别",
                "不应生成",
                [
                    {
                        "kind": "source_ref",
                        "ref": {"source_id": "source-1"},
                        "note": "证据",
                    }
                ],
                "1",
            )

    def test_invalid_state_version_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            build_teaching_card(
                "card-1",
                "L1",
                "标题",
                "正文",
                [
                    {
                        "kind": "source_ref",
                        "ref": {"source_id": "source-1"},
                        "note": "证据",
                    }
                ],
                1,  # type: ignore[arg-type]
            )


class CardCapTests(unittest.TestCase):
    """Acceptance: cap at 16 and report every dropped batch."""

    def test_cap_keeps_sixteen_and_records_omission(self) -> None:
        source = [{"card_id": "card-%d" % index} for index in range(20)]
        snapshot = deepcopy(source)
        rendered = cap_cards(source, [{"kind": "model", "reason": "disabled"}])
        self.assertEqual(len(rendered.cards), MAX_CARDS_PER_RUN)
        self.assertEqual(source, snapshot)
        self.assertEqual(rendered.omitted[0], {"kind": "model", "reason": "disabled"})
        self.assertTrue(any(item["kind"] == "cards" for item in rendered.omitted))

    def test_returned_cards_do_not_alias_the_input(self) -> None:
        source = [{"card_id": "card-1", "nested": {"value": "before"}}]
        rendered = cap_cards(source)
        rendered.cards[0]["nested"]["value"] = "after"
        self.assertEqual(source[0]["nested"]["value"], "before")


class BuilderPurityTests(unittest.TestCase):
    """Acceptance: builders never mutate or alias their inputs."""

    def test_teaching_builder_does_not_alias_nested_evidence(self) -> None:
        evidence = [
            {
                "kind": "source_ref",
                "ref": {"source_id": "source-1", "metadata": {"value": "before"}},
                "note": "证据",
            }
        ]
        snapshot = deepcopy(evidence)
        card = build_teaching_card(
            "card-1",
            "L1",
            "标题",
            "正文",
            evidence,
            "1",
        )
        card["evidence"][0]["ref"]["metadata"]["value"] = "after"
        self.assertEqual(evidence, snapshot)

    def test_diagnostic_builder_does_not_alias_inputs(self) -> None:
        diagnostic = {
            "id": "diag-1",
            "code": "CODE",
            "severity": "warning",
            "stage": "synth",
            "summary": "summary",
            "origin": "core",
            "confidence_kind": "tool",
            "location": {"file": "rtl/top.v", "meta": {"value": "before"}},
        }
        report = {"job_id": "job-1", "meta": {"value": "before"}}
        diagnostic_snapshot = deepcopy(diagnostic)
        report_snapshot = deepcopy(report)
        card = build_diagnostic_card("card-1", "L1", diagnostic, "1", report)
        card["evidence"][0]["ref"]["location"]["meta"]["value"] = "after"
        card["evidence"][1]["ref"]["meta"]["value"] = "after"
        self.assertEqual(diagnostic, diagnostic_snapshot)
        self.assertEqual(report, report_snapshot)

    def test_plan_builder_does_not_alias_step_params(self) -> None:
        params = {"strategy": "area", "opts": ["-flatten"]}
        step = PlanStep("s1", "eda.synth", "综合", params=params)
        snapshot = deepcopy(params)
        card = build_plan_card("plan-1", "目标", [step], "rev-1", "1")
        card["steps"][0]["params"]["opts"].append("-retime")
        self.assertEqual(params, snapshot)


class CardSafetyTests(unittest.TestCase):
    """Acceptance: cards reject paths, executable names and execution fields."""

    def test_absolute_path_is_rejected_by_each_teaching_builder(self) -> None:
        with self.assertRaises(ValueError):
            build_teaching_card(
                "card-1",
                "L1",
                "标题",
                "请检查 C:\\Users\\student\\top.v",
                [
                    {
                        "kind": "source_ref",
                        "ref": {"source_id": "source-1"},
                        "note": "证据",
                    }
                ],
                "1",
            )

        diagnostic = {
            "id": "diag-1",
            "code": "CODE",
            "severity": "warning",
            "stage": "synth",
            "summary": "summary",
            "origin": "core",
            "confidence_kind": "tool",
            "location": {"file": "/home/student/top.v"},
        }
        with self.assertRaises(ValueError):
            build_diagnostic_card("card-1", "L1", diagnostic, "1", {"job_id": "job-1"})

    def test_executable_filename_and_work_directory_fields_are_rejected(self) -> None:
        with self.assertRaises(ValueError):
            build_teaching_card(
                "card-1",
                "L1",
                "标题",
                "请运行 tool.exe 后再检查。",
                [
                    {
                        "kind": "source_ref",
                        "ref": {"source_id": "source-1"},
                        "note": "证据",
                    }
                ],
                "1",
            )
        with self.assertRaises(ValueError):
            build_teaching_card(
                "card-1",
                "L1",
                "标题",
                "正文",
                [
                    {
                        "kind": "source_ref",
                        "ref": {"source_id": "source-1", "work_dir": "build"},
                        "note": "证据",
                    }
                ],
                "1",
            )

    def test_uncarded_payload_never_contains_forbidden_fragments(self) -> None:
        cards = [_rule_card(), _teaching_card(), _diagnostic_card(), _plan_card()]
        text = json.dumps(cards, ensure_ascii=False)
        for fragment in ("C:\\", "/home/", "/tmp/", ".exe", "work_dir", "cwd"):
            self.assertNotIn(fragment, text)


class PlanCardValidationTests(unittest.TestCase):
    """Acceptance: approval, snapshot, capabilities and safe params are mandatory."""

    def test_valid_plan_has_three_capabilities_and_required_flags(self) -> None:
        card = _plan_card()
        validate_plan_card(card)
        self.assertTrue(card["snapshot_required"])
        self.assertTrue(card["requires_approval"])
        self.assertEqual(
            [step["capability"] for step in card["steps"]],
            ["eda.synth", "eda.sim.build", "eda.sim.run"],
        )

    def test_snapshot_and_approval_are_mandatory(self) -> None:
        for field in ("snapshot_required", "requires_approval"):
            card = _plan_card()
            card[field] = False
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_plan_card(card)

    def test_non_educational_capability_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            build_plan_card(
                "plan-1",
                "目标",
                [PlanStep("s1", "eda.pnr", "布局布线")],
                "rev-1",
                "1",
            )

    def test_dangerous_parameter_keys_are_rejected_at_every_depth_and_case(self) -> None:
        for params in (
            {"executable": "tool"},
            {"Path": "rtl/top.v"},
            {"nested": {"work_dir": "build"}},
            {"Command": "run"},
            {"output": "build"},
            {"args": ["-x"]},
        ):
            with self.subTest(params=params), self.assertRaises(ValueError):
                build_plan_card(
                    "plan-1",
                    "目标",
                    [PlanStep("s1", "eda.synth", "综合", params=params)],
                    "rev-1",
                    "1",
                )

    def test_path_like_or_executable_parameter_values_are_rejected(self) -> None:
        for params in (
            {"strategy": "C:\\work\\run"},
            {"strategy": "yosys"},
            {"opts": ["../build"]},
            {"top_module": "top.v"},
            {"duration_ticks": 100},
        ):
            with self.subTest(params=params), self.assertRaises(ValueError):
                build_plan_card(
                    "plan-1",
                    "目标",
                    [PlanStep("s1", "eda.synth", "综合", params=params)],
                    "rev-1",
                    "1",
                )

    def test_malformed_plan_structure_is_rejected(self) -> None:
        mutations = (
            lambda card: card.__setitem__("plan_id", "wrong"),
            lambda card: card.__setitem__("state_version", 1),
            lambda card: card["steps"][0].__setitem__("max_jobs", 2),
            lambda card: card["steps"][0].__setitem__("params", []),
            lambda card: card["steps"][0].__setitem__("depends_on", ["bad"]),
            lambda card: card.__setitem__("stop_conditions", []),
        )
        for mutate in mutations:
            card = _plan_card()
            mutate(card)
            with self.assertRaises(ValueError):
                validate_plan_card(card)


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
