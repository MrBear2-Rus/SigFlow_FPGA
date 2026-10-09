"""Deterministic fallback and structural output checks; not semantic proof."""

import re
from .domain import Card, CardContext, Evidence, RunRequest
from .context.builder import build_context
from .context.models import Omission
from .context.report import valid_anchor


def valid_evidence(evidence: Evidence | None, request: RunRequest) -> bool:
    # Dataclass annotations do not validate adapter results at runtime.
    # This checker must be total: malformed data is a denial, never a crash
    # that leaves a side-effecting E6 step eligible for another submission.
    if not isinstance(evidence, Evidence):
        return False
    strings = (evidence.evidence_id, evidence.project_id, evidence.revision,
               evidence.job_id, evidence.concept, evidence.summary, evidence.origin)
    if not all(type(value) is str and value.strip() for value in strings):
        return False
    try:
        if any(len(value.encode('utf-8')) > 8192 for value in strings):
            return False
    except UnicodeError:
        return False
    if evidence.confidence_kind not in ('tool', 'rule'):
        return False
    if evidence.anchor is not None:
        if not valid_anchor(evidence.anchor, request.project_id, request.revision, evidence.job_id):
            return False
        if evidence.anchor.evidence_id != evidence.evidence_id:
            return False
    try:
        packed = build_context(evidence.context_items, request.project_id, request.revision,
                               max_bytes=8192, max_tokens=8192)
        if packed.omitted:
            return False
    except ValueError:
        return False
    if not isinstance(evidence.omitted, tuple) or not all(
            isinstance(item, Omission) and type(item.item_id) is str and item.item_id
            and item.reason in ('byte_budget', 'token_budget') for item in evidence.omitted):
        return False
    return (evidence.project_id == request.project_id
            and evidence.revision == request.revision
            and evidence.complete is True
            and type(evidence.combinational_goal) is bool
            and evidence.origin in {'fixture', 'tool', 'rule'})


class RuleProvider:
    def generate(self, context: CardContext) -> dict:
        if context.evidence.concept != 'latch' or not context.evidence.combinational_goal:
            hint = '先核对已观察到的现象与预期，当前信息不足以将它判定为设计错误。'
            question = '你的设计目标是什么？当前行为与它有什么差异？'
        else:
            hint, question = {
                1: ('报告观察到了存储行为。', '你期望输出只依赖当前输入，还是需要记住过去？'),
                2: ('可以检查相关组合逻辑块的赋值覆盖范围。', '每一种输入条件下，输出是否都有定义？'),
                3: ('沿着条件不成立的路径检查输出赋值。', '该路径期望输出什么，代码是否描述了这个情况？'),
            }[context.level]
        return {'level': context.level, 'hint': hint, 'question': question,
                'evidence_ids': [context.evidence.evidence_id]}


def checked_card(value: object, context: CardContext, producer: str) -> Card | None:
    if not isinstance(value, dict) or set(value) != {'level', 'hint', 'question', 'evidence_ids'}:
        return None
    if type(value['level']) is not int or value['level'] != context.level or context.level not in (1, 2, 3):
        return None
    if value['evidence_ids'] != [context.evidence.evidence_id]:
        return None
    for key in ('hint', 'question'):
        text = value[key]
        if not isinstance(text, str) or not text.strip() or len(text) > 1200:
            return None
        # Conservative code filter; natural-language answer leaks require review.
        if re.search(r'```|\b(?:module|endmodule|always_comb|always_ff|assign)\b', text, re.I):
            return None
    limitations = ['仅依据所列证据；提示内容仍需教学质量评审。']
    if context.evidence.origin == 'fixture':
        limitations.append('离线示例证据，不是真实工程的 EDA 验证。')
    return Card(context.level, value['hint'], value['question'],
                tuple(value['evidence_ids']), producer, tuple(limitations))
