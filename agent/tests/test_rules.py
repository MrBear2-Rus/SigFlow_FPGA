"""Tests for the deterministic, model-free rule engine."""

from __future__ import annotations

import json
import os
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from sigflow_edu_agent.cards import (  # noqa: E402
    PlanStep,
    Selection,
    build_insufficient_evidence_card,
    build_plan_card,
    build_rule_card,
    cap_cards,
    validate_plan_card,
)
from sigflow_edu_agent.dispatch import (  # noqa: E402
    RunDispatcher,
    RunRequest,
    safe_teaching_answer,
    sufficient_report,
)
from sigflow_edu_agent.model_client import (  # noqa: E402
    API_KEY_ENV,
    ModelAnswer,
    ModelClient,
    ModelStatus,
)
from sigflow_edu_agent.rules import (  # noqa: E402
    INCOMPLETE_LATCH,
    INCOMPLETE_SENSITIVITY,
    UNBALANCED_BLOCK,
    WIDTH_MISMATCH,
    RuleFinding,
    analyse,
)
from sigflow_edu_agent.version import MAX_CARDS_PER_RUN  # noqa: E402

LATCH_SNIPPET = (
    "module blk(input clk, input en, input d, output reg q);\n"
    "  always @(*) begin\n"
    "    if (en)\n"
    "      q = d;\n"
    "  end\n"
    "endmodule"
)

LATCH_BEGIN_SNIPPET = (
    "module blk(input en, input d, output reg q);\n"
    "  always @(*) begin\n"
    "    if (en) begin\n"
    "      q = d;\n"
    "    end\n"
    "  end\n"
    "endmodule"
)

FULL_ELSE_SNIPPET = (
    "module blk(input en, input d, output reg q);\n"
    "  always @(*) begin\n"
    "    if (en)\n"
    "      q = d;\n"
    "    else\n"
    "      q = 1'b0;\n"
    "  end\n"
    "endmodule"
)

CHAIN_OK_SNIPPET = (
    "module blk(input a, input b, output reg y);\n"
    "  always @(*) begin\n"
    "    if (a)\n"
    "      y = 1'b1;\n"
    "    else if (b)\n"
    "      y = 1'b0;\n"
    "    else\n"
    "      y = 1'b1;\n"
    "  end\n"
    "endmodule"
)

SENSITIVITY_SNIPPET = (
    "module blk(input ready, input valid, output reg y);\n"
    "  always @(ready) begin\n"
    "    y = ready & valid;\n"
    "  end\n"
    "endmodule"
)

WIDTH_SNIPPET = (
    "module blk(input [3:0] a, output reg [3:0] y);\n"
    "  always @(*) begin\n"
    "    y = 8'hff;\n"
    "  end\n"
    "endmodule"
)

UNBALANCED_SNIPPET = (
    "module blk(input a, output reg y);\n"
    "  always @(*) begin\n"
    "    if (a) begin\n"
    "      y = 1'b1;\n"
    "  end\n"
    "endmodule"
)

CASE_WITHOUT_DEFAULT = (
    "module blk(input [1:0] s, output reg y);\n"
    "  always @(*) begin\n"
    "    case (s)\n"
    "      2'b00: y = 1'b0;\n"
    "      2'b01: y = 1'b1;\n"
    "    endcase\n"
    "  end\n"
    "endmodule"
)


class LatchRuleTests(unittest.TestCase):
    """Acceptance (a): the no-model rule card is about latch inference."""

    def test_incomplete_if_is_detected(self) -> None:
        findings = analyse(Selection(text=LATCH_SNIPPET))
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].rule, INCOMPLETE_LATCH)
        self.assertEqual(findings[0].signal, "q")

    def test_incomplete_begin_block_is_detected(self) -> None:
        findings = analyse(Selection(text=LATCH_BEGIN_SNIPPET))
        self.assertEqual([finding.rule for finding in findings], [INCOMPLETE_LATCH])

    def test_case_without_default_is_detected(self) -> None:
        findings = analyse(Selection(text=CASE_WITHOUT_DEFAULT))
        self.assertEqual([finding.rule for finding in findings], [INCOMPLETE_LATCH])
        self.assertEqual(findings[0].signal, "y")

    def test_complete_if_else_is_not_flagged(self) -> None:
        self.assertEqual(analyse(Selection(text=FULL_ELSE_SNIPPET)), [])

    def test_complete_else_if_chain_is_not_flagged(self) -> None:
        self.assertEqual(analyse(Selection(text=CHAIN_OK_SNIPPET)), [])

    def test_sequential_block_is_not_flagged(self) -> None:
        sequential = (
            "module blk(input clk, input d, output reg q);\n"
            "  always @(posedge clk) begin\n"
            "    q <= d;\n"
            "  end\n"
            "endmodule"
        )
        self.assertEqual(analyse(Selection(text=sequential)), [])

    def test_ternary_assignment_is_not_flagged(self) -> None:
        ternary = (
            "module blk(input en, input a, input b, output reg q);\n"
            "  always @(*) begin\n"
            "    q = en ? a : b;\n"
            "  end\n"
            "endmodule"
        )
        self.assertEqual(analyse(Selection(text=ternary)), [])

    def test_straight_line_combinational_block_is_not_flagged(self) -> None:
        straight = (
            "module blk(input a, input b, output reg y);\n"
            "  always @(*) begin\n"
            "    y = a & b;\n"
            "  end\n"
            "endmodule"
        )
        self.assertEqual(analyse(Selection(text=straight)), [])

    def test_card_body_does_not_contain_a_fixed_module(self) -> None:
        findings = analyse(Selection(text=LATCH_SNIPPET))
        card = build_rule_card("card-1", "L1", findings[0], Selection(text=LATCH_SNIPPET), "1")
        self.assertIn("锁存器", card["body"])
        self.assertNotIn("always @(*)", card["body"])
        self.assertNotIn("else", card["body"].replace("无 else", ""))

    def test_rule_match_evidence_is_first(self) -> None:
        selection = Selection(text=LATCH_SNIPPET, source_id="source-x")
        findings = analyse(selection)
        card = build_rule_card("card-1", "L1", findings[0], selection, "1")
        self.assertEqual(card["evidence"][0]["kind"], "rule_match")
        self.assertEqual(card["source"], "rule")
        self.assertEqual(card["level"], "L1")
        self.assertEqual(card["issue_id"], findings[0].issue_id)


class OtherRuleTests(unittest.TestCase):
    """The remaining rules must be deterministic and avoid false positives."""

    def test_width_mismatch_is_detected(self) -> None:
        findings = analyse(Selection(text=WIDTH_SNIPPET))
        self.assertEqual([finding.rule for finding in findings], [WIDTH_MISMATCH])
        self.assertEqual(findings[0].detail["declared_width"], 4)
        self.assertEqual(findings[0].detail["literal_width"], 8)

    def test_matching_width_is_not_flagged(self) -> None:
        ok = (
            "module blk(input [3:0] a, output reg [3:0] y);\n"
            "  always @(*) begin\n"
            "    y = 4'hf;\n"
            "  end\n"
            "endmodule"
        )
        self.assertEqual(analyse(Selection(text=ok)), [])

    def test_incomplete_sensitivity_is_detected(self) -> None:
        findings = analyse(Selection(text=SENSITIVITY_SNIPPET))
        self.assertEqual([finding.rule for finding in findings], [INCOMPLETE_SENSITIVITY])
        self.assertEqual(findings[0].signal, "valid")

    def test_complete_sensitivity_is_not_flagged(self) -> None:
        ok = (
            "module blk(input ready, input valid, output reg y);\n"
            "  always @(ready or valid) begin\n"
            "    y = ready & valid;\n"
            "  end\n"
            "endmodule"
        )
        self.assertEqual(analyse(Selection(text=ok)), [])

    def test_unbalanced_begin_end_is_detected(self) -> None:
        findings = analyse(Selection(text=UNBALANCED_SNIPPET))
        self.assertIn(UNBALANCED_BLOCK, [finding.rule for finding in findings])

    def test_balanced_code_is_not_flagged(self) -> None:
        ok = (
            "module blk(input a, output reg y);\n"
            "  always @(*) begin\n"
            "    y = a;\n"
            "  end\n"
            "endmodule"
        )
        self.assertEqual(analyse(Selection(text=ok)), [])

    def test_empty_selection_produces_nothing(self) -> None:
        self.assertEqual(analyse(Selection(text="")), [])

    def test_analysis_is_deterministic(self) -> None:
        first = analyse(Selection(text=LATCH_SNIPPET))
        second = analyse(Selection(text=LATCH_SNIPPET))
        self.assertEqual(
            [(finding.rule, finding.signal) for finding in first],
            [(finding.rule, finding.signal) for finding in second],
        )


class ReportTrustTests(unittest.TestCase):
    """Acceptance (b): an empty diagnostic list is only explained when trustworthy."""

    def test_complete_core_report_is_trustworthy(self) -> None:
        trustworthy, reasons = sufficient_report(
            {"completeness": "complete", "origin": "core", "revision": "rev-1", "state": "Succeeded",
             "snapshot_id": "snap-1", "input_fingerprint": "fingerprint"}
        )
        self.assertTrue(trustworthy)
        self.assertEqual(reasons, [])

    def test_unknown_completeness_and_origin_are_not_trustworthy(self) -> None:
        trustworthy, _ = sufficient_report(
            {"completeness": "exact", "origin": "gateway", "revision": "rev-1"}
        )
        self.assertFalse(trustworthy)

    def test_legacy_origin_is_not_trustworthy(self) -> None:
        trustworthy, reasons = sufficient_report(
            {"completeness": "complete", "origin": "legacy", "revision": "rev-1"}
        )
        self.assertFalse(trustworthy)
        self.assertTrue(any("legacy" in reason for reason in reasons))

    def test_partial_completeness_is_not_trustworthy(self) -> None:
        trustworthy, reasons = sufficient_report(
            {"completeness": "partial", "origin": "core", "revision": "rev-1"}
        )
        self.assertFalse(trustworthy)
        self.assertTrue(any("completeness" in reason for reason in reasons))

    def test_unavailable_completeness_is_not_trustworthy(self) -> None:
        trustworthy, _ = sufficient_report(
            {"completeness": "unavailable", "origin": "core", "revision": "rev-1"}
        )
        self.assertFalse(trustworthy)

    def test_missing_revision_is_not_trustworthy(self) -> None:
        trustworthy, reasons = sufficient_report(
            {"completeness": "complete", "origin": "core", "revision": None}
        )
        self.assertFalse(trustworthy)
        self.assertTrue(any("revision" in reason for reason in reasons))

    def test_missing_origin_is_not_trustworthy(self) -> None:
        """AD-05: fail-closed — an absent origin is not evidence of a trusted run."""
        trustworthy, reasons = sufficient_report(
            {"completeness": "complete", "revision": "rev-1"}
        )
        self.assertFalse(trustworthy)
        self.assertTrue(any("origin" in reason for reason in reasons))

    def test_empty_origin_is_not_trustworthy(self) -> None:
        trustworthy, reasons = sufficient_report(
            {"completeness": "complete", "origin": "   ", "revision": "rev-1"}
        )
        self.assertFalse(trustworthy)
        self.assertTrue(any("origin" in reason for reason in reasons))

    def test_unknown_origin_is_not_trustworthy(self) -> None:
        trustworthy, reasons = sufficient_report(
            {"completeness": "complete", "origin": "made-up", "revision": "rev-1"}
        )
        self.assertFalse(trustworthy)
        self.assertTrue(any("origin" in reason for reason in reasons))

    def test_insufficient_evidence_card_never_claims_success(self) -> None:
        card = build_insufficient_evidence_card(
            card_id="card-1",
            selection=Selection(text=LATCH_SNIPPET),
            report_ref={"job_id": "job-1", "completeness": "legacy_unverified"},
            reasons=["报告的 origin 是 legacy"],
            state_version="2",
        )
        self.assertEqual(card["kind"], "rule")
        self.assertEqual(card["source"], "rule")
        self.assertIsNone(card["issue_id"])
        self.assertIn("证据不足", card["title"])
        self.assertNotIn("所以电路没有问题", card["body"])


class CardCapTests(unittest.TestCase):
    """Acceptance (d): anything dropped is reported in ``omitted``."""

    def test_card_cap_keeps_sixteen(self) -> None:
        cards = [{"card_id": "card-%d" % i} for i in range(20)]
        rendered = cap_cards(cards)
        self.assertEqual(len(rendered.cards), MAX_CARDS_PER_RUN)
        self.assertTrue(any(item["kind"] == "cards" for item in rendered.omitted))

    def test_card_cap_preserves_caller_omissions(self) -> None:
        rendered = cap_cards([{"card_id": "card-1"}], [{"kind": "x", "reason": "y"}])
        self.assertEqual(rendered.omitted, [{"kind": "x", "reason": "y"}])


class PlanCardTests(unittest.TestCase):
    """Acceptance (d): a plan never carries a path, executable or script."""

    def test_plan_card_uses_only_three_capabilities(self) -> None:
        card = build_plan_card(
            plan_id="plan-1",
            goal="goal",
            steps=[
                PlanStep("s1", "eda.synth", "综合"),
                PlanStep("s2", "eda.sim.build", "构建", depends_on=("s1",)),
                PlanStep("s3", "eda.sim.run", "运行", depends_on=("s2",)),
            ],
            revision="rev-1",
            state_version="1",
        )
        self.assertEqual(
            [step["capability"] for step in card["steps"]],
            ["eda.synth", "eda.sim.build", "eda.sim.run"],
        )
        self.assertTrue(card["snapshot_required"])
        self.assertTrue(card["requires_approval"])
        self.assertEqual(card["revision"], "rev-1")

    def test_non_educational_capability_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            build_plan_card(
                plan_id="plan-1",
                goal="goal",
                steps=[PlanStep("s1", "eda.pnr", "布局布线")],
                revision="rev-1",
                state_version="1",
            )

    def test_forbidden_parameter_is_rejected(self) -> None:
        card = build_plan_card(
            plan_id="plan-1",
            goal="goal",
            steps=[PlanStep("s1", "eda.synth", "综合")],
            revision="rev-1",
            state_version="1",
        )
        card["steps"][0]["params"] = {"executable": "yosys"}
        with self.assertRaises(ValueError):
            validate_plan_card(card)

    def test_missing_approval_is_rejected(self) -> None:
        card = build_plan_card(
            plan_id="plan-1",
            goal="goal",
            steps=[PlanStep("s1", "eda.synth", "综合")],
            revision="rev-1",
            state_version="1",
        )
        card["requires_approval"] = False
        with self.assertRaises(ValueError):
            validate_plan_card(card)


class FindingShapeTests(unittest.TestCase):
    """The rule-match reference feeds evidence, so its keys are contract."""

    def test_rule_match_ref_carries_rule_and_issue(self) -> None:
        selection = Selection(
            text=LATCH_SNIPPET,
            source_id="source-1",
            node_id="node-1",
            start_line=1,
            end_line=6,
        )
        finding: RuleFinding = analyse(selection)[0]
        ref = finding.to_rule_match_ref(selection)
        self.assertEqual(ref["rule"], INCOMPLETE_LATCH)
        self.assertEqual(ref["issue_id"], finding.issue_id)
        self.assertEqual(ref["source_id"], "source-1")
        self.assertEqual(ref["node_id"], "node-1")


class ModelOutputPolicyTests(unittest.TestCase):
    """Optional model prose must fail closed before it is rendered as a card."""

    def test_l1_to_l3_reject_code_formatted_model_answer(self) -> None:
        answer = safe_teaching_answer(
            ModelAnswer(ok=True, text="```verilog\nmodule replacement; endmodule\n```"),
            "L2",
        )
        self.assertFalse(answer.ok)
        self.assertIn("教学级别", answer.reason)

    def test_rejects_model_answer_with_absolute_path(self) -> None:
        answer = safe_teaching_answer(
            ModelAnswer(ok=True, text="请检查 C:\\Users\\student\\top.v 的条件分支。"),
            "L1",
        )
        self.assertFalse(answer.ok)
        self.assertIn("本机路径", answer.reason)

    def test_unsafe_model_answer_degrades_to_rule_card(self) -> None:
        class UnsafeModel:
            status = ModelStatus(True, "test", "configured", "test-model")

            def complete(self, instruction: str, evidence: str) -> ModelAnswer:
                del instruction, evidence
                return ModelAnswer(ok=True, text="```verilog\nendmodule\n```")

        dispatcher = RunDispatcher(gateway=object(), model=UnsafeModel())  # type: ignore[arg-type]
        outcome = dispatcher.dispatch(
            RunRequest(kind="explain", level="L1", selection=Selection(text=LATCH_SNIPPET)),
            state_version="1",
        )
        self.assertEqual(outcome.state, "degraded")
        self.assertFalse(outcome.model_used)
        self.assertTrue(any(item["kind"] == "model_explanation" for item in outcome.omitted))
        self.assertTrue(all(card["source"] == "rule" for card in outcome.cards))
        self.assertNotIn("```", "\n".join(str(card) for card in outcome.cards))

    def test_model_client_refuses_path_and_bootstrap_secret(self) -> None:
        class Response:
            def __init__(self, content: str) -> None:
                self._body = json.dumps(
                    {"choices": [{"message": {"content": content}}]}
                ).encode("utf-8")

            def __enter__(self) -> "Response":
                return self

            def __exit__(self, *args: object) -> None:
                return None

            def read(self, size: int = -1) -> bytes:
                del size
                return self._body

        status = ModelStatus(True, "test", "configured", "test-model")
        with patch.dict(os.environ, {API_KEY_ENV: "model-secret"}):
            with patch(
                "urllib.request.OpenerDirector.open",
                return_value=Response("see C:\\Users\\student\\top.v"),
            ):
                self.assertFalse(ModelClient(status, secrets=("ui-secret",)).complete("i", "e").ok)
            with patch(
                "urllib.request.OpenerDirector.open",
                return_value=Response("do not repeat ui-secret"),
            ):
                self.assertFalse(ModelClient(status, secrets=("ui-secret",)).complete("i", "e").ok)


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
