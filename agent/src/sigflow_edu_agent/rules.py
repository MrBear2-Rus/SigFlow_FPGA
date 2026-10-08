"""Deterministic, model-free RTL verification rule engine.

Every rule in this module is a pure function from a :class:`Selection` to an
optional :class:`RuleFinding`.  No network, no model, no file access: the same
selection always produces the same finding.  This is what makes the no-model
degradation path trustworthy and testable.

The engine intentionally covers a small, explainable rule set:

``RTL_LATCH_INCOMPLETE_BRANCH``
    A combinational ``always`` block assigns a signal on some control paths but
    not all of them, which makes synthesis infer a latch.
``RTL_SENSITIVITY_INCOMPLETE``
    An ``always @(...)`` block reads a signal that its sensitivity list omits.
``RTL_WIDTH_MISMATCH``
    A block assignment whose constant literal is wider than the target signal.
``RTL_BLOCK_UNBALANCED``
    ``begin``/``end`` counters do not match, i.e. an incomplete edit.

Findings never contain corrected source code.  They name the symptom, the
signal and the inference rule so that the student can fix it themselves.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Any, Optional

INCOMPLETE_LATCH: str = "RTL_LATCH_INCOMPLETE_BRANCH"
INCOMPLETE_SENSITIVITY: str = "RTL_SENSITIVITY_INCOMPLETE"
WIDTH_MISMATCH: str = "RTL_WIDTH_MISMATCH"
UNBALANCED_BLOCK: str = "RTL_BLOCK_UNBALANCED"

BUILTIN_KEYWORDS: frozenset[str] = frozenset(
    {
        "always", "assign", "begin", "end", "if", "else", "case", "casex",
        "casez", "default", "endcase", "module", "endmodule", "input",
        "output", "inout", "wire", "reg", "logic", "integer", "parameter",
        "localparam", "posedge", "negedge", "or", "and", "not", "for",
        "while", "initial", "generate", "endgenerate", "function",
        "endfunction", "task", "endtask", "signed", "unsigned", "bit",
        "byte", "int", "shortint", "longint", "time", "real", "genvar",
    }
)

_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_$]*")
_ACTIVE_LOW = re.compile(r"^\s*n[_]?[a-z0-9_]*\s*$", re.IGNORECASE)
_ASSIGN_LHS = re.compile(r"^\s*(?:[A-Za-z_][\w$]*\s*)*([A-Za-z_][\w$]*)\s*(?:\[[^\]]*\])?\s*<=")
_ASSIGN_LHS_BLOCKING = re.compile(
    r"^\s*(?:[A-Za-z_][\w$]*\s*)*([A-Za-z_][\w$]*)\s*(?:\[[^\]]*\])?\s*=(?!=)"
)
_ALWAYS = re.compile(r"\balways\b\s*(@\s*\((?P<sens>[^)]*)\))?\s*(?P<rest>.*)$")
_IF = re.compile(r"\bif\s*\(")
_ELSE = re.compile(r"\belse\b")
_CASE = re.compile(r"\bcase[xz]?\s*\(")
_ENDCASE = re.compile(r"\bendcase\b")
_DECL = re.compile(
    r"\b(?:input|output|inout|wire|reg|logic|bit|integer|parameter|localparam)\b"
    r"(?:\s+(?:signed|unsigned|wire|reg|logic|bit))?"
    r"\s*(?:\[[^\]]*\]\s*)?"
    r"(?P<names>[A-Za-z_][\w$]*(?:\s*,\s*[A-Za-z_][\w$]*)*)"
)
_BLOCK_OPEN = re.compile(r"\bbegin\b")
_BLOCK_CLOSE = re.compile(r"\bend\b(?!case|function|task|module|generate)")
_LITERAL_WIDTH = re.compile(r"(?P<width>\d+)\s*'\s*[sS]?[bBoOdDhH]")
_READ_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_$]*")


@dataclass(frozen=True)
class Selection:
    """One bounded piece of student evidence handed to the rule engine."""

    text: str
    node_id: Optional[str] = None
    source_id: Optional[str] = None
    start_line: Optional[int] = None
    end_line: Optional[int] = None


@dataclass(frozen=True)
class RuleFinding:
    """A deterministic finding produced without any model involvement."""

    issue_id: str
    rule: str
    title: str
    body: str
    signal: Optional[str] = None
    line_hint: Optional[int] = None
    detail: dict[str, Any] = field(default_factory=dict)

    def to_rule_match_ref(self, selection: Selection) -> dict[str, Any]:
        """Build the ``evidence[0].ref`` payload for a ``rule_match`` entry."""
        ref: dict[str, Any] = {
            "rule": self.rule,
            "issue_id": self.issue_id,
            "signal": self.signal,
            "line_hint": self.line_hint,
            "detail": self.detail,
        }
        if selection.node_id:
            ref["node_id"] = selection.node_id
        if selection.source_id:
            ref["source_id"] = selection.source_id
        return ref


@dataclass(frozen=True)
class _Block:
    """One ``always`` block recovered from the selection text."""

    start: int
    body: str
    sensitivity: Optional[str]
    combinational: bool


@dataclass(frozen=True)
class _Branch:
    """One top-level control path inside an ``always`` body."""

    condition: str
    assigns: frozenset[str]
    line: int


@dataclass(frozen=True)
class _Assigned:
    """Assignment coverage of one statement or block."""

    definite: frozenset[str]
    possible: frozenset[str]

    @staticmethod
    def empty() -> "_Assigned":
        """Return coverage for a statement that assigns nothing."""
        return _Assigned(frozenset(), frozenset())

    def merge(self, other: "_Assigned") -> "_Assigned":
        """Combine two sequential statements."""
        return _Assigned(self.definite | other.definite, self.possible | other.possible)


def analyse(selection: Selection) -> list[RuleFinding]:
    """Run every rule over ``selection`` and return the findings in rule order."""
    findings: list[RuleFinding] = []
    for rule in _RULES:
        finding = rule(selection)
        if finding is not None:
            findings.append(finding)
    return findings


def _detect_latch(selection: Selection) -> Optional[RuleFinding]:
    """Detect a combinational block that does not assign on every path."""
    for block in _iter_blocks(selection.text):
        if not block.combinational:
            continue
        coverage = _walk(block.body)
        if not coverage.possible:
            continue
        for signal in sorted(coverage.possible):
            if signal in BUILTIN_KEYWORDS or signal in coverage.definite:
                continue
            if _ACTIVE_LOW.match(signal):
                continue
            covered_path = _first_covering_path(block.body, signal)
            body = (
                "在同一个组合 always 块里，信号 `%s` 只在“%s”这一条路径上被赋值，"
                "其它路径（包括条件不成立时）没有给它赋任何值。\n\n"
                "综合工具会认为“没有赋值”意味着“保持上一次的值”，于是为 `%s` "
                "推断出一个锁存器（latch），而不是你想要的纯组合逻辑。"
                "这就是“推断值”和“锁存器”的区别：组合块的输出必须只由当前输入决定，"
                "所以每一条控制路径都要显式给出结果。\n\n"
                "请先自己检查两点：\n"
                "1. 条件不成立时，`%s` 本来应该是什么值？\n"
                "2. 这个块里是否还有别的信号也只覆盖了部分路径？\n\n"
                "（本卡片只定位问题，不提供可直接替换的完整代码。）"
                % (signal, covered_path, signal, signal)
            )
            return RuleFinding(
                issue_id="issue-latch-%s-%d" % (signal, block.start + 1),
                rule=INCOMPLETE_LATCH,
                title="组合逻辑推断出锁存器：`%s` 没有被所有分支赋值" % signal,
                body=body,
                signal=signal,
                line_hint=block.start + 1,
                detail={
                    "signal": signal,
                    "covered_path": covered_path,
                    "missing_path": "条件不成立（无 else 或缺少默认赋值）",
                },
            )
    return None


def _possible_of(arm: _Assigned) -> set[str]:
    """Return the signals one branch may assign."""
    return set(arm.possible)


def _first_covering_path(body: str, signal: str) -> str:
    """Describe the first modelled path that assigns ``signal``."""
    for branch in _top_level_branches(body):
        if signal in branch.assigns:
            return branch.condition
    for line in body.splitlines():
        statement = _strip_comment(line).strip()
        match = _DIRECT_ASSIGN.match(statement)
        if match is not None and match.group("target").split("[", 1)[0].strip() == signal:
            return "无条件执行"
    return "某一条件分支"


def _detect_sensitivity(selection: Selection) -> Optional[RuleFinding]:
    """Detect a read signal missing from an explicit sensitivity list.

    Single-character names are deliberately skipped: in Verilog ``x``/``z``/
    ``b``/``d``/``h`` are also number-base characters, so a one-letter read is
    not enough evidence to accuse the sensitivity list.
    """
    for block in _iter_blocks(selection.text):
        if block.sensitivity is None:
            continue
        listed = set(_READ_IDENTIFIER.findall(block.sensitivity))
        if listed & {"*", "always_comb"} or "*" in block.sensitivity:
            continue
        lhs = _assigned_signals(block.body)
        reads = _read_signals(block.body)
        for read in sorted(reads):
            if read in listed or read in lhs or read in BUILTIN_KEYWORDS:
                continue
            if len(read) <= 1:
                continue
            body = (
                "这个 `always` 块的敏感列表里没有 `%s`，但块内读取了它。\n\n"
                "仿真器只会在敏感列表中的信号变化时重新执行这个块，"
                "因此 `%s` 变化时块不会重算，仿真波形会和综合结果不一致。"
                "这类问题在综合报告里通常看不到，只能在仿真或代码审查时发现。\n\n"
                "请对照敏感列表和块内读取的信号，找出所有遗漏项；"
                "或者确认这里是否本应使用组合块通配写法。"
                "（本卡片只定位问题，不提供完整修正代码。）" % (read, read)
            )
            return RuleFinding(
                issue_id="issue-sensitivity-%s-%d" % (read, block.start + 1),
                rule=INCOMPLETE_SENSITIVITY,
                title="敏感列表不完整：缺少 `%s`" % read,
                body=body,
                signal=read,
                line_hint=block.start + 1,
                detail={"missing_signal": read, "sensitivity": block.sensitivity},
            )
    return None


def _detect_width(selection: Selection) -> Optional[RuleFinding]:
    """Detect a constant literal wider than the signal it is assigned to."""
    ranges = _declared_ranges(selection.text)
    if not ranges:
        return None
    for line_number, line in enumerate(selection.text.splitlines(), start=1):
        match = _ASSIGN_LHS.match(line) or _ASSIGN_LHS_BLOCKING.match(line)
        if not match:
            continue
        target = match.group(1)
        width = ranges.get(target)
        if width is None:
            continue
        literal = _LITERAL_WIDTH.search(line)
        if literal is None:
            continue
        source_width = int(literal.group("width"))
        if source_width <= width:
            continue
        body = (
            "`%s` 声明为 %d 位，但这一行赋给它的常量字面量是 %d 位。\n\n"
            "Verilog 会按右侧宽度截断（或零扩展），超出声明宽度的位被静默丢掉，"
            "综合不报错、仿真也可能“看起来正常”，但功能已经和你的意图不一致。\n\n"
            "请确认：这个常量的高位是你有意省略的，还是宽度写错了？"
            "（本卡片只定位问题，不提供完整修正代码。）"
            % (target, width, source_width)
        )
        return RuleFinding(
            issue_id="issue-width-%s-%d" % (target, line_number),
            rule=WIDTH_MISMATCH,
            title="位宽不匹配：`%s` 为 %d 位，赋值为 %d 位" % (target, width, source_width),
            body=body,
            signal=target,
            line_hint=line_number,
            detail={
                "signal": target,
                "declared_width": width,
                "literal_width": source_width,
            },
        )
    return None


def _detect_unbalanced(selection: Selection) -> Optional[RuleFinding]:
    """Detect an unbalanced ``begin``/``end`` pair (an incomplete edit)."""
    opens = len(_BLOCK_OPEN.findall(selection.text))
    closes = len(_BLOCK_CLOSE.findall(selection.text))
    if opens == closes:
        return None
    body = (
        "这段代码里 `begin` 出现 %d 次、`end` 出现 %d 次，数量对不上。\n\n"
        "片段很可能被截断，或者某次编辑删掉了结尾。语法结构不完整时，"
        "仿真器的报错位置常常指向文件末尾而不是真正的缺口。\n\n"
        "请先补全结构，再重新解读工具报错；在结构补齐之前，"
        "其它诊断的结论都不可靠。（本卡片只定位问题，不提供完整修正代码。）"
        % (opens, closes)
    )
    return RuleFinding(
        issue_id="issue-unbalanced-%d" % opens,
        rule=UNBALANCED_BLOCK,
        title="结构不完整：`begin`/`end` 数量不匹配",
        body=body,
        line_hint=None,
        detail={"begin_count": opens, "end_count": closes},
    )


_RULES = (
    _detect_latch,
    _detect_sensitivity,
    _detect_width,
    _detect_unbalanced,
)
"""Rule evaluation order; the first finding becomes the primary card."""


def _iter_blocks(text: str) -> list[_Block]:
    """Recover every ``always`` block body from ``text``."""
    lines = text.splitlines()
    blocks: list[_Block] = []
    for line_number, line in enumerate(lines):
        head = _ALWAYS.search(line)
        if head is None:
            continue
        sensitivity = head.group("sens")
        remainder = _strip_comment(head.group("rest") or "")
        body_lines: list[str]
        if remainder:
            # ``always @(*) begin`` and ``always @(*) q = d;`` share a line.
            body_lines = [remainder]
            depth = _depth_delta(remainder)
        else:
            # A bare ``always`` consumes the following lines until the implicit
            # block (one statement) or the explicit ``begin``/``end`` closes.
            body_lines = []
            depth = 1
        index = line_number + 1
        while depth > 0 and index < len(lines):
            candidate = lines[index]
            body_lines.append(candidate)
            depth += _depth_delta(candidate)
            index += 1
        body = _unwrap_block("\n".join(body_lines))
        if sensitivity is None:
            combinational = True
        else:
            combinational = "*" in sensitivity or "always_comb" in sensitivity
        blocks.append(
            _Block(
                start=line_number,
                body=body,
                sensitivity=sensitivity,
                combinational=combinational,
            )
        )
    return blocks


def _strip_comment(line: str) -> str:
    """Remove a trailing ``//`` comment from one source line."""
    marker = line.find("//")
    return line[:marker] if marker >= 0 else line


def _unwrap_block(body: str) -> str:
    """Drop the outermost ``begin``/``end`` wrapper of a block body."""
    lines = body.splitlines()
    if not lines:
        return body
    first = _strip_comment(lines[0]).strip()
    if not first.startswith("begin"):
        return body
    rest = first[len("begin") :]
    if rest.strip():
        lines = [rest] + lines[1:]
    else:
        lines = lines[1:]
    while lines and not _strip_comment(lines[-1]).strip():
        lines.pop()
    if lines and _strip_comment(lines[-1]).strip() == "end":
        lines.pop()
    return "\n".join(lines)


def _depth_delta(line: str) -> int:
    """Return ``begin`` minus ``end`` for one source line."""
    return len(_BLOCK_OPEN.findall(line)) - len(_BLOCK_CLOSE.findall(line))


def _top_level_branches(body: str) -> list[_Branch]:
    """Model the body as a list of top-level control paths.

    The model is deliberately simple and explainable: an ``if`` always yields a
    taken path plus an ``else`` path (empty when the source has no ``else``),
    and a ``case`` yields one path per item plus an implicit path when the
    ``default`` item is missing.
    """
    lines = body.splitlines()
    branches: list[_Branch] = []
    index = 0
    while index < len(lines):
        line = lines[index]
        statement = _strip_comment(line).strip()
        if _IF.search(statement):
            condition = _condition_of(statement) or "if"
            arms, _has_else, next_index = _if_arms(lines, index)
            branches.append(
                _Branch(
                    condition=condition,
                    assigns=frozenset(_possible_of(arms[0])),
                    line=index + 1,
                )
            )
            index = max(next_index, index + 1)
            continue
        if _CASE.search(statement):
            case = _collect_case(lines, index)
            index = case.next_index
            for item, item_body in case.items:
                branches.append(
                    _Branch(
                        condition="case %s" % item,
                        assigns=frozenset(_assigned_direct(item_body)),
                        line=case.line,
                    )
                )
            if not case.has_default:
                branches.append(
                    _Branch(
                        condition="case default（缺失）",
                        assigns=frozenset(),
                        line=case.line,
                    )
                )
            continue
        index += 1
    if not branches and _assigned_direct(body):
        # Straight-line block: every assignment covers every path.
        branches.append(
            _Branch(
                condition="无条件执行",
                assigns=frozenset(_assigned_direct(body)),
                line=0,
            )
        )
    return branches


def _if_paths(
    lines: list[str],
    start: int,
) -> tuple[tuple[set[str], set[str]], bool, int]:
    """Return ``((taken, other), has_else, next_index)`` for the ``if`` at ``start``.

    ``else if`` chains are folded into the branch they belong to, which is what
    makes a full ``if / else if / else`` chain count as path-complete.
    """
    statement = _strip_comment(lines[start]).strip()
    taken_body, cursor = _collect_statement(lines, _after_condition(statement), start)
    taken = _assigned_direct(taken_body)

    probe = cursor
    while probe < len(lines) and not _strip_comment(lines[probe]).strip():
        probe += 1
    if probe >= len(lines) or not _ELSE.search(_strip_comment(lines[probe])):
        return (taken, set()), False, cursor

    tail = _strip_comment(lines[probe]).split("else", 1)[1]
    if _IF.search(tail):
        nested, _nested_has_else, next_index = _if_paths(lines, probe)
        return (taken | nested[0], nested[0] | nested[1]), True, next_index
    else_body, next_index = _collect_statement(lines, tail, probe)
    return (taken, _assigned_direct(else_body)), True, next_index


def _collect_statement(lines: list[str], first: str, start: int) -> tuple[str, int]:
    """Collect one statement starting at ``first`` (which may open a ``begin``)."""
    collected = [first]
    depth = _depth_delta(first)
    explicit_block = depth > 0
    cursor = start + 1
    if explicit_block:
        while depth > 0 and cursor < len(lines):
            collected.append(lines[cursor])
            depth += _depth_delta(lines[cursor])
            cursor += 1
    elif not first.strip():
        # ``if (cond)`` followed by an implicit single statement on the next line.
        while cursor < len(lines) and not _strip_comment(lines[cursor]).strip():
            cursor += 1
        if cursor < len(lines):
            collected = [lines[cursor]]
            cursor += 1
    return "\n".join(collected), cursor


@dataclass
class _Case:
    """Recovered ``case`` construct."""

    items: list[tuple[str, str]]
    has_default: bool
    next_index: int
    line: int


def _collect_case(lines: list[str], start: int) -> _Case:
    """Collect ``case`` items and detect a missing ``default``."""
    items: list[tuple[str, str]] = []
    has_default = False
    current_label: Optional[str] = None
    buffer: list[str] = []
    cursor = start + 1
    while cursor < len(lines):
        line = lines[cursor]
        if _ENDCASE.search(line):
            if current_label is not None:
                items.append((current_label, "\n".join(buffer)))
            cursor += 1
            break
        stripped = _strip_comment(line).strip()
        if _is_case_label(stripped):
            label, _, tail = stripped.partition(":")
            label = label.strip()
            if current_label is not None:
                items.append((current_label, "\n".join(buffer)))
            if label == "default":
                has_default = True
                current_label = None
                buffer = []
            else:
                current_label = label
                buffer = [tail] if tail.strip() else []
            cursor += 1
            continue
        if current_label is not None:
            buffer.append(line)
        cursor += 1
    return _Case(
        items=items,
        has_default=has_default,
        next_index=max(cursor, start + 1),
        line=start + 1,
    )


def _is_case_label(stripped: str) -> bool:
    """Return whether a line is a ``case`` item label line (``2'b01:``)."""
    if not stripped or ":" not in stripped:
        return False
    label, _, _tail = stripped.partition(":")
    label = label.strip()
    if not label or "=" in label or "?" in label or "(" in label:
        return False
    return all(char.isalnum() or char in "_'\"$, \t" for char in label)


def _after_condition(line: str) -> str:
    """Return the text of a line after its ``if (...)`` condition."""
    match = _IF.search(line)
    if match is None:
        return line
    depth = 0
    for position in range(match.end() - 1, len(line)):
        char = line[position]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return line[position + 1 :]
    return ""


def _condition_of(line: str) -> Optional[str]:
    """Return the raw condition text of an ``if`` line."""
    match = _IF.search(line)
    if match is None:
        return None
    depth = 0
    start = match.end()
    for position in range(match.end() - 1, len(line)):
        char = line[position]
        if char == "(":
            depth += 1
            if depth == 1:
                start = position + 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return line[start:position].strip()
    return None


def _assigned_signals(body: str) -> set[str]:
    """Return every signal written by a non-blocking or blocking assignment."""
    signals: set[str] = set()
    for line in body.splitlines():
        for pattern in (_ASSIGN_LHS, _ASSIGN_LHS_BLOCKING):
            match = pattern.match(line)
            if match is not None:
                signals.add(match.group(1))
                break
    return signals


_DIRECT_ASSIGN = re.compile(
    r"^(?P<target>[A-Za-z_][\w$]*(?:\s*\[[^\]]*\])?)\s*(?:<=|=(?!=))"
)
"""A block assignment at the start of a statement, ignoring indentation."""


def _assigned_direct(body: str) -> set[str]:
    """Return signals assigned directly by this statement, not nested blocks.

    Nested ``if``/``case`` bodies carry their own paths and are modelled
    separately, so descending into them here would double-count a conditional
    assignment as unconditional.
    """
    signals: set[str] = set()
    for line in body.splitlines():
        statement = _strip_comment(line).strip()
        if not statement:
            continue
        if _IF.search(statement) or _CASE.search(statement) or statement.startswith("begin"):
            continue
        match = _DIRECT_ASSIGN.match(statement)
        if match is not None:
            target = match.group("target")
            signals.add(target.split("[", 1)[0].strip())
    return signals


def _read_signals(body: str) -> set[str]:
    """Return every identifier referenced on the right-hand side of a line."""
    reads: set[str] = set()
    for line in body.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("//"):
            continue
        if "=" in stripped:
            _, _, rhs = stripped.partition("=")
            reads.update(_READ_IDENTIFIER.findall(rhs))
        elif _IF.search(stripped):
            condition = _condition_of(stripped)
            if condition:
                reads.update(_READ_IDENTIFIER.findall(condition))
    return reads


def _universally_assigned(branches: list[_Branch]) -> set[str]:
    """Return signals assigned on every modelled path (safe in all paths)."""
    if not branches:
        return set()
    universal: Optional[set[str]] = None
    for branch in branches:
        signals = set(branch.assigns)
        universal = signals if universal is None else (universal & signals)
    if universal is None:
        return set()
    return universal


def _walk(body: str) -> _Assigned:
    """Recursively compute assignment coverage of a block body.

    ``definite`` signals are assigned on every control path; ``possible``
    signals are assigned on at least one path.  A signal that is ``possible``
    but not ``definite`` is exactly what makes synthesis infer a latch.
    """
    lines = body.splitlines()
    result = _Assigned.empty()
    index = 0
    while index < len(lines):
        statement = _strip_comment(lines[index]).strip()
        if not statement:
            index += 1
            continue
        if _IF.search(statement):
            chain, has_else, next_index = _if_arms(lines, index)
            definite: set[str] = set()
            possible: set[str] = set()
            for arm in chain:
                definite |= arm.definite
                possible |= arm.possible
            if has_else:
                definite = set(definite)
            else:
                definite = set()
            result = result.merge(_Assigned(frozenset(definite), frozenset(possible)))
            index = max(next_index, index + 1)
            continue
        if _CASE.search(statement):
            case = _collect_case(lines, index)
            definite = set()
            possible: set[str] = set()
            for _label, item_body in case.items:
                coverage = _walk(item_body)
                definite |= coverage.definite
                possible |= coverage.possible
            if not case.has_default:
                definite = set()
            result = result.merge(_Assigned(frozenset(definite), frozenset(possible)))
            index = max(case.next_index, index + 1)
            continue
        coverage = _statement_coverage(lines, index)
        result = result.merge(coverage)
        index += 1
    return result


def _statement_coverage(lines: list[str], index: int) -> _Assigned:
    """Coverage of one non-control statement at ``index``."""
    statement = _strip_comment(lines[index]).strip()
    match = _DIRECT_ASSIGN.match(statement)
    if match is None:
        return _Assigned.empty()
    target = match.group("target").split("[", 1)[0].strip()
    return _Assigned(frozenset({target}), frozenset({target}))


def _if_arms(lines: list[str], start: int) -> tuple[list[_Assigned], bool, int]:
    """Return ``(arms, has_else, next_index)`` for the ``if`` at ``start``.

    An ``else if`` chain is folded into one decision: every arm is a branch of
    the same condition tree, and ``has_else`` says whether the tree is closed
    by a final ``else`` (which is what makes the arms exhaustive).
    """
    statement = _strip_comment(lines[start]).strip()
    taken_body, cursor = _collect_statement(lines, _after_condition(statement), start)
    arms: list[_Assigned] = [_walk(taken_body)]

    probe = cursor
    while probe < len(lines) and not _strip_comment(lines[probe]).strip():
        probe += 1
    if probe >= len(lines) or not _ELSE.search(_strip_comment(lines[probe])):
        return arms, False, cursor

    tail = _strip_comment(lines[probe]).split("else", 1)[1]
    if _IF.search(tail):
        nested_arms, nested_has_else, next_index = _if_arms(lines, probe)
        arms.extend(nested_arms)
        return arms, nested_has_else, next_index
    else_body, next_index = _collect_statement(lines, tail, probe)
    arms.append(_walk(else_body))
    return arms, True, next_index


def _declared_ranges(text: str) -> dict[str, int]:
    """Map every declared signal to its declared bit width (default 1)."""
    widths: dict[str, int] = {}
    for line in text.splitlines():
        for match in _DECL.finditer(line):
            width = _range_width(line)
            for name in match.group("names").split(","):
                widths.setdefault(name.strip(), width)
    return widths


def _range_width(line: str) -> int:
    """Return the declared width encoded in an ``[msb:lsb]`` range, else 1."""
    start = line.find("[")
    end = line.find("]", start + 1)
    if start < 0 or end < 0:
        return 1
    span = line[start + 1 : end]
    if ":" not in span:
        return 1
    high, _, low = span.partition(":")
    try:
        return abs(int(high.strip()) - int(low.strip())) + 1
    except ValueError:
        return 1
