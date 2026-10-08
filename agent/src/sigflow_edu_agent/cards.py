"""TeachingCard and PlanCard builders.

Cards are the only model-facing output of the service.  Every builder here
enforces three invariants that the contract freezes:

1. L1–L3 cards never contain a complete corrected module; the reference
   solution is only reachable through the explicit two-step L4 policy.
2. No card, evidence reference or limitation may contain a local absolute
   path, an executable name or a working directory.
3. ``state_version`` is always a decimal string.

The builders are pure: they return new dictionaries and never mutate their
inputs, so a run can be inspected before it is stored.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Mapping, Optional, Sequence

from .rules import RuleFinding, Selection
from .util import decimal
from .version import CARD_KINDS, LEVELS, MAX_CARDS_PER_RUN, PLAN_CAPABILITIES


@dataclass
class RenderedCards:
    """Cards plus the explicit list of anything that was dropped."""

    cards: list[dict[str, Any]] = field(default_factory=list)
    omitted: list[dict[str, str]] = field(default_factory=list)
    model_used: bool = False


def cap_cards(
    cards: Sequence[dict[str, Any]],
    omitted: Sequence[Mapping[str, str]] = (),
) -> RenderedCards:
    """Apply the per-run card cap and record what was dropped."""
    kept = list(cards[:MAX_CARDS_PER_RUN])
    dropped = list(omitted)
    if len(cards) > MAX_CARDS_PER_RUN:
        dropped.append(
            {
                "kind": "cards",
                "reason": "超过单次运行 %d 张卡片的限额，多余卡片已省略"
                % MAX_CARDS_PER_RUN,
            }
        )
    return RenderedCards(cards=kept, omitted=dropped)


def source_ref_evidence(selection: Selection, note: str) -> dict[str, Any]:
    """Build a bounded ``source_ref`` evidence entry for a selection."""
    ref: dict[str, Any] = {}
    if selection.source_id:
        ref["source_id"] = selection.source_id
    if selection.node_id:
        ref["node_id"] = selection.node_id
    if selection.start_line is not None:
        ref["start_line"] = int(selection.start_line)
    if selection.end_line is not None:
        ref["end_line"] = int(selection.end_line)
    return {"kind": "source_ref", "ref": ref, "note": note}


def rule_match_evidence(finding: RuleFinding, selection: Selection) -> dict[str, Any]:
    """Build the mandatory ``rule_match`` evidence entry of a rule card."""
    return {
        "kind": "rule_match",
        "ref": finding.to_rule_match_ref(selection),
        "note": "规则引擎在所选片段内命中 %s（未调用模型）" % finding.rule,
    }


def build_rule_card(
    card_id: str,
    level: str,
    finding: RuleFinding,
    selection: Selection,
    state_version: str,
) -> dict[str, Any]:
    """Build a ``source: "rule"`` TeachingCard from a deterministic finding."""
    if level not in LEVELS:
        raise ValueError("unsupported level: %s" % level)
    return {
        "card_id": card_id,
        "kind": "rule",
        "level": level,
        "title": finding.title,
        "body": finding.body,
        "issue_id": finding.issue_id,
        "source": "rule",
        "evidence": [
            rule_match_evidence(finding, selection),
            source_ref_evidence(selection, "规则命中的选中片段"),
        ],
        "course_refs": [],
        "limitations": [
            "本卡片由确定性规则引擎生成，未经过模型推理，也不会给出可直接替换的完整代码。",
            "规则结论只对上面给出的证据范围有效；请结合仿真波形与综合报告自行确认。",
        ],
        "expired": False,
        "state_version": state_version,
    }


def build_teaching_card(
    card_id: str,
    level: str,
    title: str,
    body: str,
    evidence: Sequence[Mapping[str, Any]],
    state_version: str,
    issue_id: Optional[str] = None,
    source: str = "rule",
    limitations: Sequence[str] = (),
    course_refs: Sequence[Mapping[str, Any]] = (),
) -> dict[str, Any]:
    """Build a TeachingCard with an explicit, caller-supplied evidence list."""
    if level not in LEVELS:
        raise ValueError("unsupported level: %s" % level)
    if source not in ("rule", "model"):
        raise ValueError("unsupported card source: %s" % source)
    notes = list(limitations)
    if not notes:
        notes = ["本卡片只对列出的证据负责；未在证据中出现的原因不作推断。"]
    return {
        "card_id": card_id,
        "kind": "teaching",
        "level": level,
        "title": title,
        "body": body,
        "issue_id": issue_id,
        "source": source,
        "evidence": [dict(entry) for entry in evidence],
        "course_refs": [dict(entry) for entry in course_refs],
        "limitations": notes,
        "expired": False,
        "state_version": state_version,
    }


def build_diagnostic_card(
    card_id: str,
    level: str,
    diagnostic: Mapping[str, Any],
    state_version: str,
    report_ref: Mapping[str, Any],
    source: str = "rule",
) -> dict[str, Any]:
    """Build a ``diagnostic_explanation`` card for one tool diagnostic.

    The diagnostic's ``code``/``raw_code``/``severity``/``location`` are copied
    verbatim into the evidence.  The body explains what the tool said; it never
    invents a root cause that the report did not provide.
    """
    if level not in LEVELS:
        raise ValueError("unsupported level: %s" % level)
    code = str(diagnostic.get("code") or diagnostic.get("raw_code") or "UNKNOWN")
    raw_code = diagnostic.get("raw_code")
    severity = str(diagnostic.get("severity") or "unknown")
    summary = str(diagnostic.get("summary") or "").strip()
    location = diagnostic.get("location")
    evidence: list[dict[str, Any]] = [
        {
            "kind": "diagnostic",
            "ref": {
                "id": diagnostic.get("id"),
                "code": code,
                "raw_code": raw_code,
                "severity": severity,
                "stage": diagnostic.get("stage"),
                "location": location,
                "origin": diagnostic.get("origin"),
                "confidence_kind": diagnostic.get("confidence_kind"),
                "summary": summary,
            },
            "note": "工具报告的原始诊断字段，未经改写",
        },
        {
            "kind": "job_report",
            "ref": dict(report_ref),
            "note": "该诊断所属的归一化 Job 报告",
        },
    ]
    location_text = _format_location(location)
    body_lines = [
        "工具在 %s 阶段报告了一条 %s 级诊断：" % (diagnostic.get("stage") or "未知", severity),
        "",
        "%s：%s" % (code, summary or "（工具未给出摘要文本）"),
    ]
    if location_text:
        body_lines.extend(["", "定位：%s" % location_text])
    body_lines.extend(
        [
            "",
            "报告只提供了以上内容。请先回到定位到的位置自行阅读代码，"
            "确认这条诊断描述的现象是否真的存在；本卡片不会替你推断"
            "报告之外的根因，也不会给出完整修正代码。",
        ]
    )
    return {
        "card_id": card_id,
        "kind": "diagnostic_explanation",
        "level": level,
        "title": "诊断解读：%s" % code,
        "body": "\n".join(body_lines),
        "issue_id": str(diagnostic.get("id") or code),
        "source": source,
        "evidence": evidence,
        "course_refs": [],
        "limitations": [
            "本卡片只复述工具报告的字段；报告没有给出的根因不作补充推断。",
            "报告的可信度受其 completeness/origin 限制，请对照 /health 与报告字段自行判断。",
        ],
        "expired": False,
        "state_version": state_version,
    }


def build_insufficient_evidence_card(
    card_id: str,
    selection: Selection,
    report_ref: Mapping[str, Any],
    reasons: Sequence[str],
    state_version: str,
    level: str = "L1",
) -> dict[str, Any]:
    """Build a ``source: "rule"`` card that refuses to draw a conclusion.

    Used when a report has an empty ``diagnostics`` array but is not trustworthy
    enough (``completeness`` outside ``complete``/``exact``, or
    ``origin == "legacy"``) to claim the design is clean.
    """
    body = "\n".join(
        [
            "本次工具报告里 `diagnostics` 是空数组，但这份报告**不足以**支持"
            "“电路没有问题”的结论。",
            "",
            "原因：",
        ]
        + ["- %s" % reason for reason in reasons]
        + [
            "",
            "“没有诊断”只说明这次工具运行在它自己的覆盖范围内没有报出问题，"
            "它不等于设计正确。空诊断的可信度和报告本身一样高，"
            "所以在证据补齐之前不能下任何结论。",
            "",
            "建议的下一步：确认该 Job 是否与当前 revision/快照绑定、"
            "报告是否来自本次真实的综合或仿真运行，再决定是否需要重跑。",
        ]
    )
    evidence: list[dict[str, Any]] = [
        {
            "kind": "job_report",
            "ref": dict(report_ref),
            "note": "工具报告本身：诊断为空，但完整性/来源不足以支撑结论",
        }
    ]
    if selection.source_id or selection.node_id:
        evidence.append(source_ref_evidence(selection, "报告关联的选中片段"))
    return {
        "card_id": card_id,
        "kind": "rule",
        "level": level,
        "title": "证据不足：不能由空诊断得出“电路正常”",
        "body": body,
        "issue_id": None,
        "source": "rule",
        "evidence": evidence,
        "course_refs": [],
        "limitations": [
            "本卡片拒绝给出结论，因为报告的可信度不足；这不是“无问题”的证明。",
        ],
        "expired": False,
        "state_version": state_version,
    }


def build_reference_solution_card(
    card_id: str,
    level: str,
    title: str,
    body: str,
    evidence: Sequence[Mapping[str, Any]],
    state_version: str,
    issue_id: Optional[str] = None,
) -> dict[str, Any]:
    """Build the L4 ``reference_solution`` card.

    Only the HTTP layer may call this, and only after the two-step policy has
    been satisfied (level L4 **and** ``more == true``).
    """
    if level != "L4":
        raise ValueError("reference solutions are only issued at level L4")
    return {
        "card_id": card_id,
        "kind": "reference_solution",
        "level": "L4",
        "title": title,
        "body": body,
        "issue_id": issue_id,
        "source": "rule",
        "evidence": [dict(entry) for entry in evidence],
        "course_refs": [],
        "limitations": [
            "这是参考解，不是唯一写法；请先自己动手，再对照差异。",
            "参考解只对列出的证据范围有效。",
        ],
        "expired": False,
        "state_version": state_version,
    }


def _format_location(location: Any) -> str:
    """Render a diagnostic location without exposing a local path."""
    if not isinstance(location, Mapping):
        return ""
    parts: list[str] = []
    for key in ("file", "path", "source_id", "node_id"):
        value = location.get(key)
        if isinstance(value, str) and value.strip():
            parts.append("%s=%s" % (key, _basename(value)))
    for key in ("start_line", "line", "end_line"):
        value = location.get(key)
        if isinstance(value, int):
            parts.append("%s=%d" % (key, value))
    return ", ".join(parts)


def _basename(value: str) -> str:
    """Keep only the last path segment of a reported location."""
    return value.replace("\\", "/").rstrip("/").split("/")[-1]


@dataclass(frozen=True)
class PlanStep:
    """One execution step of a teaching plan."""

    step_id: str
    capability: str
    description: str
    params: dict[str, Any] = field(default_factory=dict)
    depends_on: tuple[str, ...] = ()


def build_plan_card(
    plan_id: str,
    goal: str,
    steps: Sequence[PlanStep],
    revision: str,
    state_version: str,
    stop_conditions: Sequence[str] = (),
    max_jobs: Optional[int] = None,
) -> dict[str, Any]:
    """Build a PlanCard restricted to the three educational capabilities.

    The card carries capability names and bounded scalar parameters only.  It
    never contains a filesystem path, an executable name, a script or a working
    directory; :func:`validate_plan_card` re-checks that on the produced object.
    """
    for step in steps:
        if step.capability not in PLAN_CAPABILITIES:
            raise ValueError("plan step uses a non-educational capability: %s" % step.capability)
    card = {
        "plan_id": plan_id,
        "goal": goal,
        "steps": [
            {
                "step_id": step.step_id,
                "capability": step.capability,
                "description": step.description,
                "params": dict(step.params),
                "depends_on": list(step.depends_on),
                "max_jobs": 1,
            }
            for step in steps
        ],
        "max_jobs": int(max_jobs if max_jobs is not None else max(1, len(steps))),
        "stop_conditions": list(stop_conditions)
        or [
            "任何一步失败即停止，不自动重跑或换参数重试",
            "revision 或快照发生变化即停止，需重新确认计划",
        ],
        "snapshot_required": True,
        "revision": revision,
        "requires_approval": True,
        "state_version": state_version,
    }
    validate_plan_card(card)
    return card


def validate_plan_card(card: Mapping[str, Any]) -> None:
    """Raise :class:`ValueError` if a plan card violates the frozen policy."""
    steps = card.get("steps")
    if not isinstance(steps, list) or not steps:
        raise ValueError("plan card must contain at least one step")
    for step in steps:
        capability = step.get("capability")
        if capability not in PLAN_CAPABILITIES:
            raise ValueError("plan card step capability is not educational: %r" % capability)
        for key in step.get("params", {}):
            if key in _FORBIDDEN_PARAM_KEYS:
                raise ValueError("plan card step carries a forbidden parameter: %r" % key)
    if card.get("snapshot_required") is not True:
        raise ValueError("plan card must require a snapshot")
    if card.get("requires_approval") is not True:
        raise ValueError("plan card must require explicit approval")


_FORBIDDEN_PARAM_KEYS: frozenset[str] = frozenset(
    {
        "path", "paths", "file", "files", "dir", "directory", "work_dir", "workdir",
        "cwd", "executable", "exe", "command", "cmd", "args", "argv", "script",
        "env", "environment", "shell", "output", "output_dir", "log", "log_path",
    }
)
"""Parameter names that would smuggle an execution primitive into a plan."""


def decimal_state(version: int) -> str:
    """Render a session/run state version for the wire."""
    return decimal(version)


def is_known_card_kind(kind: str) -> bool:
    """Return whether ``kind`` is a frozen TeachingCard kind."""
    return kind in CARD_KINDS
