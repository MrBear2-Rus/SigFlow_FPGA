"""Run dispatcher: turns a bounded teaching request into cards.

This is the policy layer of the sidecar.  It owns the decisions that the
NG-07 acceptance criteria name explicitly:

* with no model key configured, ``explain`` still produces a ``source: "rule"``
  card and the run is reported as ``degraded`` with ``model_used: false``;
* L1–L3 never produce a corrected module, and L4 is refused unless the caller
  has already asked for more detail (two-step policy) — the refusal happens
  before any card is built, so the answer can never leak;
* ``report_review`` fetches the real report through the Gateway and never
  pretends to have read a report it could not fetch;
* an empty ``diagnostics`` array is only explained as "the tool reported
  nothing", and never as "the circuit is fine", unless the report is
  trustworthy enough (see :func:`_insufficient_evidence_reasons`).

All returned payloads are JSON-serializable dictionaries; the dispatcher does
not perform any I/O other than the read-only Gateway call.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Any, Mapping, Optional, Sequence

from .cards import (
    PlanStep,
    build_diagnostic_card,
    build_insufficient_evidence_card,
    build_plan_card,
    build_reference_solution_card,
    build_rule_card,
    build_teaching_card,
    cap_cards,
    rule_match_evidence,
    source_ref_evidence,
)
from .errors import AgentError
from .gateway_client import GatewayClient
from .model_client import ModelAnswer, ModelClient, ModelStatus
from .rules import RuleFinding, Selection, analyse
from .util import contains_absolute_path

TRUSTED_COMPLETENESS: frozenset[str] = frozenset({"complete", "exact"})
"""Report completeness values trustworthy enough to explain an empty diagnostic list."""

_CONTEXT_TRUSTED = frozenset({"core", "gateway"})
"""Report origins considered attributable to this instance's real run."""

_PLAN_TOP_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_$]*$")
"""A module name may only ever be a bare identifier, never a path."""

_MAX_MODEL_TEACHING_CHARS = 12 * 1024
"""A model paragraph must remain small enough to be a teaching aid, not a dump."""

_MODEL_CODE_MARKERS: tuple[str, ...] = (
    "```",
    "endmodule",
    "always @",
    "always_comb",
    "always_ff",
)
"""Markers that turn an L1--L3 response into an impermissible code answer."""


def safe_teaching_answer(answer: ModelAnswer, level: str) -> ModelAnswer:
    """Fail closed before untrusted model text becomes a TeachingCard.

    The model is optional.  A rejected answer is deliberately converted to a
    normal degraded rule response instead of being redacted in-place: partial
    redaction can leave a misleading or directly usable code answer behind.
    """
    if not answer.ok:
        return answer
    text = answer.text.strip()
    if not text:
        return ModelAnswer(False, reason="模型没有返回可用文本，已降级为规则讲解。")
    if len(text) > _MAX_MODEL_TEACHING_CHARS:
        return ModelAnswer(False, reason="模型讲解超出长度预算，已降级为规则讲解。")
    if contains_absolute_path(text):
        return ModelAnswer(False, reason="模型返回包含本机路径，已降级为规则讲解。")
    lowered = text.lower()
    if level in {"L1", "L2", "L3"} and any(marker in lowered for marker in _MODEL_CODE_MARKERS):
        return ModelAnswer(
            False,
            reason="模型返回了代码形式的内容，不符合该教学级别，已降级为规则讲解。",
        )
    return ModelAnswer(True, text=text)


@dataclass(frozen=True)
class RunRequest:
    """One validated run request handed to the dispatcher."""

    kind: str
    level: str
    selection: Selection
    job_id: Optional[str] = None
    more: bool = False
    top: str = ""
    """Top module name taken from the session; used only after identifier validation."""
    revision: str = ""
    """Session revision, carried into a PlanCard so approval binds to a version."""


@dataclass
class Outcome:
    """The result of dispatching one run."""

    state: str
    cards: list[dict[str, Any]] = field(default_factory=list)
    omitted: list[dict[str, str]] = field(default_factory=list)
    model_used: bool = False
    error_code: Optional[str] = None
    error_message: Optional[str] = None


def sufficient_report(report: Mapping[str, Any]) -> tuple[bool, list[str]]:
    """Decide whether an empty diagnostic list may be explained at all.

    Returns ``(trustworthy, reasons)``.  When ``trustworthy`` is false the
    caller must produce an ``issue_id: null`` rule card that refuses to draw a
    conclusion; it must never claim the circuit is fine.
    """
    reasons: list[str] = []
    completeness = str(report.get("completeness") or "").strip()
    origin = str(report.get("origin") or "").strip()
    if completeness not in TRUSTED_COMPLETENESS:
        reasons.append(
            "报告的 completeness 是 %r，不在 %s 之内，覆盖范围无法确认为完整。"
            % (completeness or "（缺失）", "/".join(sorted(TRUSTED_COMPLETENESS)))
        )
    if origin == "legacy":
        reasons.append("报告的 origin 是 legacy：它来自旧数据，没有可信的 revision/输入指纹绑定。")
    elif origin and origin not in _CONTEXT_TRUSTED:
        reasons.append("报告的 origin 是 %r，无法确认它来自本次实例的真实运行。" % origin)
    if not report.get("revision"):
        reasons.append("报告没有 revision，无法确认它对应的是学生当前看到的代码版本。")
    return (not reasons, reasons)


class RunDispatcher:
    """Build cards for one run request using rules, the Gateway and (maybe) a model."""

    def __init__(
        self,
        gateway: GatewayClient,
        model: ModelClient,
    ) -> None:
        self._gateway = gateway
        self._model = model

    @property
    def model_status(self) -> ModelStatus:
        """Model availability to report on ``/health`` and ``/capabilities``."""
        return self._model.status

    @property
    def gateway_base_url(self) -> str:
        """Configured Gateway base URL (never a local path)."""
        return self._gateway.base_url

    # -- public entry points ---------------------------------------------

    def dispatch(self, request: RunRequest, state_version: str) -> Outcome:
        """Dispatch a run request.  Raises :class:`AgentError` for policy refusals."""
        if request.kind == "plan":
            return self._dispatch_plan(request, state_version)
        if request.kind == "report_review":
            return self._dispatch_report_review(request, state_version)
        if request.kind == "hint":
            return self._dispatch_hint(request, state_version)
        return self._dispatch_explain(request, state_version)

    # -- explain / hint ---------------------------------------------------

    def _dispatch_explain(self, request: RunRequest, state_version: str) -> Outcome:
        """Serve ``kind=explain``: deterministic rule cards plus optional model prose.

        ``level=L4`` is the reference-solution request.  ``more=true`` is the
        second half of the two-step policy and is enforced by the HTTP layer
        before this method is reached; here we only build the solution card.
        """
        if request.level == "L4":
            return self._dispatch_reference_solution(request, state_version)

        findings = analyse(request.selection)
        cards: list[dict[str, Any]] = []
        omitted: list[dict[str, str]] = []
        for finding in findings:
            cards.append(
                build_rule_card(
                    card_id="card-%d" % (len(cards) + 1),
                    level=request.level,
                    finding=finding,
                    selection=request.selection,
                    state_version=state_version,
                )
            )
        if not findings:
            cards.append(self._no_finding_card(request, state_version))

        answer = safe_teaching_answer(self._model.complete(
            instruction=self._explain_instruction(request),
            evidence=self._evidence_digest(findings, request.selection),
        ), request.level)
        model_used = False
        if answer.ok:
            cards.append(
                build_teaching_card(
                    card_id="card-%d" % (len(cards) + 1),
                    level=request.level,
                    title="模型补充讲解",
                    body=answer.text,
                    evidence=[
                        rule_match_evidence(findings[0], request.selection)
                        if findings
                        else source_ref_evidence(request.selection, "本次选中的片段")
                    ],
                    state_version=state_version,
                    issue_id=findings[0].issue_id if findings else None,
                    source="model",
                    limitations=[
                        "模型讲解基于上面列出的证据生成，可能与工具结论不一致，请以证据为准。",
                    ],
                )
            )
            model_used = True
        else:
            omitted.append({"kind": "model_explanation", "reason": answer.reason})

        rendered = cap_cards(cards, omitted)
        state = "succeeded" if model_used else "degraded"
        return Outcome(
            state=state,
            cards=rendered.cards,
            omitted=rendered.omitted,
            model_used=model_used,
        )

    def _dispatch_reference_solution(
        self,
        request: RunRequest,
        state_version: str,
    ) -> Outcome:
        """Serve the second L4 step: a review checklist, never a rewritten module.

        v1 deliberately does not emit a replacement module body.  The card
        states the reference approach (which inference rules apply and what to
        check) and records, as a limitation, that the service does not author
        the student's project files.  This keeps the safety property
        "the Agent never writes the student's code" true even at L4.
        """
        finding = next(iter(analyse(request.selection)), None)
        evidence: list[dict[str, Any]] = []
        if finding is not None:
            evidence.append(rule_match_evidence(finding, request.selection))
        evidence.append(source_ref_evidence(request.selection, "L4 参考解所依据的片段"))
        title = "参考解要点（L4）"
        body_lines = [
            "已确认你完成了两次明确操作，下面是参考解的**要点**，不是可以直接替换的模块。",
            "",
        ]
        if finding is not None:
            body_lines.extend(
                [
                    "本片段命中的规则：`%s`（信号 `%s`）。" % (finding.rule, finding.signal or "-"),
                    "",
                    "参考做法的判断顺序：",
                    "1. 先列出这个块在每条控制路径上应当产出的值；",
                    "2. 找出没有被覆盖的路径，明确它应有的默认结果；",
                    "3. 用显式赋值覆盖所有路径，使块内输出只依赖当前输入；",
                    "4. 重新综合，确认综合报告里不再出现该信号的锁存器推断；",
                    "5. 用仿真波形验证被补齐的路径确实产生了你预期的值。",
                ]
            )
        else:
            body_lines.extend(
                [
                    "本次片段没有命中已实现的规则，因此没有可参考的具体规则结论。",
                    "",
                    "通用判断顺序：先用最小输入组合穷举这个块的所有控制路径，"
                    "确认每条路径产出的值都是你预期的，再回到综合报告核对。",
                ]
            )
        body_lines.extend(
            [
                "",
                "本服务不会代写工程文件，也不会给出可直接提交的完整模块；"
                "请你自己动手修改，然后重新运行验证。",
            ]
        )
        card = build_reference_solution_card(
            card_id="card-1",
            level="L4",
            title=title,
            body="\n".join(body_lines),
            evidence=evidence,
            state_version=state_version,
            issue_id=finding.issue_id if finding is not None else None,
        )
        rendered = cap_cards([card])
        return Outcome(
            state="succeeded",
            cards=rendered.cards,
            omitted=rendered.omitted,
            model_used=False,
        )

    def _dispatch_hint(self, request: RunRequest, state_version: str) -> Outcome:
        """Serve ``kind=hint``: one nudge, never a solution."""
        findings = analyse(request.selection)
        omitted: list[dict[str, str]] = []
        if findings:
            primary = findings[0]
            body = "\n".join(
                [
                    "先不要急着改代码。请回答一个问题：",
                    "",
                    "**%s**" % _hint_question(primary),
                    "",
                    "把你从这段片段里读到的信号、条件和赋值路径列出来，"
                    "再对照你在波形或综合报告里看到的现象。",
                    "",
                    "（这不是答案，只是让你自己找到答案的下一步。）",
                ]
            )
            cards = [
                build_teaching_card(
                    card_id="card-1",
                    level=request.level,
                    title="提示：%s" % primary.title,
                    body=body,
                    evidence=[rule_match_evidence(primary, request.selection)],
                    state_version=state_version,
                    issue_id=primary.issue_id,
                    source="rule",
                    limitations=["提示只指出观察方向，不给出修正后的代码。"],
                )
            ]
        else:
            cards = [self._no_finding_card(request, state_version)]

        answer = safe_teaching_answer(self._model.complete(
            instruction="只给一句启发式提问，不要给出修正代码。",
            evidence=self._evidence_digest(findings, request.selection),
        ), request.level)
        model_used = False
        if answer.ok:
            cards.append(
                build_teaching_card(
                    card_id="card-%d" % (len(cards) + 1),
                    level=request.level,
                    title="模型提问",
                    body=answer.text,
                    evidence=[
                        rule_match_evidence(findings[0], request.selection)
                        if findings
                        else source_ref_evidence(request.selection, "本次选中的片段")
                    ],
                    state_version=state_version,
                    issue_id=findings[0].issue_id if findings else None,
                    source="model",
                    limitations=["模型提问仅供参考，请以规则证据和工具报告为准。"],
                )
            )
            model_used = True
        else:
            omitted.append({"kind": "model_hint", "reason": answer.reason})

        rendered = cap_cards(cards, omitted)
        return Outcome(
            state="succeeded" if model_used else "degraded",
            cards=rendered.cards,
            omitted=rendered.omitted,
            model_used=model_used,
        )

    def _no_finding_card(
        self,
        request: RunRequest,
        state_version: str,
    ) -> dict[str, Any]:
        """Explain that no rule matched — without claiming the code is correct."""
        if request.selection.text.strip():
            body = (
                "在当前选中的片段里，规则引擎没有命中任何已知的锁存器、敏感列表、"
                "位宽或结构模式。\n\n"
                "这只说明**已实现的那几条规则**没有报警，不等于这段代码正确。\n\n"
                "可以继续做的检查：\n"
                "1. 这段信号的驱动是否覆盖了你期望的所有条件组合？\n"
                "2. 综合和仿真报告里有没有相关诊断？\n"
                "3. 选中范围是否需要扩大（当前片段可能被截断）？"
            )
            title = "规则未命中：请扩大证据范围"
        else:
            body = (
                "本次请求没有提供可分析的代码片段。\n\n"
                "规则引擎只能解释它真正看到的证据。请在编辑器里选中具体代码，"
                "或提供 source_id / 行号范围后重试；不要在没有证据的情况下推测结论。"
            )
            title = "缺少可分析的证据"
        evidence: list[dict[str, Any]] = [
            source_ref_evidence(request.selection, "本次请求的选中范围")
        ]
        return build_teaching_card(
            card_id="card-1",
            level=request.level,
            title=title,
            body=body,
            evidence=evidence,
            state_version=state_version,
            issue_id=None,
            source="rule",
            limitations=[
                "规则未命中不等于代码正确；本卡片拒绝给出任何正确性结论。",
            ],
        )

    # -- plan -------------------------------------------------------------

    def _dispatch_plan(self, request: RunRequest, state_version: str) -> Outcome:
        """Serve ``kind=plan`` with a capability-only, path-free PlanCard."""
        goal = "在不改动工程文件的前提下，用受限能力复现并定位当前问题"
        steps: list[PlanStep] = []
        params: dict[str, Any] = {}
        if _PLAN_TOP_PATTERN.match(request.top or ""):
            params["top_module"] = request.top
        steps.append(
            PlanStep(
                step_id="s1",
                capability="eda.synth",
                description="对当前 revision 的不可变快照做综合，拿到综合报告与资源占用。",
                params=dict(params),
            )
        )
        steps.append(
            PlanStep(
                step_id="s2",
                capability="eda.sim.build",
                description="在同一个快照上编译仿真可执行文件，确认 testbench 可以构建。",
                params=dict(params),
                depends_on=("s1",),
            )
        )
        steps.append(
            PlanStep(
                step_id="s3",
                capability="eda.sim.run",
                description="运行上一步构建出的仿真，产出波形后回到代码对照观察。",
                params=dict(params),
                depends_on=("s2",),
            )
        )
        card = build_plan_card(
            plan_id="plan-1",
            goal=goal,
            steps=steps,
            revision=request.revision or "",
            state_version=state_version,
        )
        rendered = cap_cards([card])
        return Outcome(
            state="succeeded",
            cards=rendered.cards,
            omitted=rendered.omitted,
            model_used=False,
        )

    # -- report_review ----------------------------------------------------

    def _dispatch_report_review(self, request: RunRequest, state_version: str) -> Outcome:
        """Serve ``kind=report_review`` from the Gateway's normalized report."""
        job_id = request.job_id or ""
        if not job_id:
            raise AgentError(
                "INVALID_ARGUMENT",
                "kind=report_review 需要 job_id",
                http_status=400,
            )
        outcome = self._gateway.fetch_job_report(job_id)
        if not outcome.ok or not isinstance(outcome.report, Mapping):
            return Outcome(
                state="failed",
                cards=[],
                omitted=[],
                model_used=False,
                error_code=outcome.code or "GATEWAY_UNAVAILABLE",
                error_message=outcome.message
                or "无法从 EDA Gateway 读取该 Job 的报告，本次不提供任何报告解读。",
            )
        report = outcome.report
        report_ref = _report_reference(job_id, report)
        diagnostics = report.get("diagnostics")
        cards: list[dict[str, Any]] = []
        omitted: list[dict[str, str]] = []

        if not isinstance(diagnostics, list) or not diagnostics:
            trustworthy, reasons = sufficient_report(report)
            if trustworthy:
                cards.append(
                    build_teaching_card(
                        card_id="card-1",
                        level=request.level,
                        title="报告没有报出任何诊断",
                        body=_empty_diagnostics_body(report),
                        evidence=[
                            {
                                "kind": "job_report",
                                "ref": dict(report_ref),
                                "note": "完整报告：diagnostics 为空数组",
                            }
                        ],
                        state_version=state_version,
                        issue_id=None,
                        source="rule",
                        limitations=[
                            "空诊断只表示本次工具运行没有报告问题，不构成设计正确的证明。",
                        ],
                    )
                )
            else:
                cards.append(
                    build_insufficient_evidence_card(
                        card_id="card-1",
                        selection=request.selection,
                        report_ref=report_ref,
                        reasons=reasons,
                        state_version=state_version,
                        level=request.level,
                    )
                )
        else:
            for diagnostic in diagnostics:
                if not isinstance(diagnostic, Mapping):
                    omitted.append(
                        {"kind": "diagnostic", "reason": "诊断条目不是 JSON 对象，已忽略"}
                    )
                    continue
                if len(cards) >= 16:
                    omitted.append(
                        {
                            "kind": "diagnostic",
                            "reason": "超过单次运行卡片上限，剩余诊断未展开",
                        }
                    )
                    continue
                cards.append(
                    build_diagnostic_card(
                        card_id="card-%d" % (len(cards) + 1),
                        level=request.level,
                        diagnostic=diagnostic,
                        state_version=state_version,
                        report_ref=report_ref,
                    )
                )

        rendered = cap_cards(cards, omitted)
        return Outcome(
            state="degraded" if rendered.cards else "succeeded",
            cards=rendered.cards,
            omitted=rendered.omitted,
            model_used=False,
        )

    # -- prompt helpers ---------------------------------------------------

    def _explain_instruction(self, request: RunRequest) -> str:
        """Instruction text for the optional model call."""
        return (
            "以 %s 难度解释学生选中的这段 Verilog。只做引导式讲解："
            "指出问题的推理过程与需要学生自己确认的点，"
            "不要给出可以直接替换的完整模块。" % request.level
        )

    def _evidence_digest(
        self,
        findings: Sequence[RuleFinding],
        selection: Selection,
    ) -> str:
        """Render a bounded, path-free evidence digest for the model prompt."""
        lines: list[str] = []
        if selection.source_id:
            lines.append("source_id=%s" % selection.source_id)
        if selection.start_line is not None:
            lines.append(
                "lines=%s-%s"
                % (selection.start_line, selection.end_line or selection.start_line)
            )
        for finding in findings:
            lines.append("rule=%s signal=%s" % (finding.rule, finding.signal or "-"))
        excerpt = selection.text.strip()
        if excerpt:
            lines.append("--- selection ---")
            lines.append(excerpt)
        return "\n".join(lines)


def _hint_question(finding: RuleFinding) -> str:
    """Return the single question a hint card asks for a finding."""
    if finding.rule == "RTL_LATCH_INCOMPLETE_BRANCH":
        return "当条件不成立时，`%s` 应该保持上一次的值，还是应该有一个明确的结果？" % (
            finding.signal or "这个信号"
        )
    if finding.rule == "RTL_SENSITIVITY_INCOMPLETE":
        return "如果 `%s` 变化但这个块没有重新执行，仿真结果会和综合结果有什么差别？" % (
            finding.signal or "这个信号"
        )
    if finding.rule == "RTL_WIDTH_MISMATCH":
        return "赋值右边多出来的那些位，被截掉之后功能还符合你的预期吗？"
    return "这段片段的结构完整吗？缺口在哪里？"


def _empty_diagnostics_body(report: Mapping[str, Any]) -> str:
    """Body text for a trustworthy report that reported no diagnostics."""
    return "\n".join(
        [
            "工具本次运行在它的覆盖范围内**没有报告任何诊断**。",
            "",
            "- 报告来源 origin：%s" % (report.get("origin") or "（未标注）"),
            "- 覆盖完整性 completeness：%s" % (report.get("completeness") or "（未标注）"),
            "- 绑定 revision：%s" % (report.get("revision") or "（未标注）"),
            "",
            "请注意：这只说明工具没有报出问题，不能据此判断电路正确。"
            "空诊断的可信度等于这份报告本身的可信度；"
            "要确认功能，仍然需要看仿真波形和你的 testbench 是否真的覆盖了目标场景。",
        ]
    )


def _report_reference(job_id: str, report: Mapping[str, Any]) -> dict[str, Any]:
    """Build the job-report evidence reference, keeping report fields verbatim."""
    return {
        "job_id": job_id,
        "origin": report.get("origin"),
        "completeness": report.get("completeness"),
        "revision": report.get("revision"),
        "snapshot_id": report.get("snapshot_id"),
        "input_fingerprint": report.get("input_fingerprint"),
        "capability": report.get("capability"),
        "plugin_id": report.get("plugin_id"),
        "state": report.get("state"),
        "raw_report_schema": report.get("raw_report_schema"),
    }


def trusted_completeness() -> frozenset[str]:
    """Expose the trusted completeness set for documentation and tests."""
    return TRUSTED_COMPLETENESS
