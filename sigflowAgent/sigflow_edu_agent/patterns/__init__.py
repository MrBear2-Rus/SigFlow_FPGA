"""Versioned teaching policy; this module does not detect hardware errors.

PatternFacts is a trusted host-port value, never model output. Synthetic facts
are deliberately restricted to fixture evidence. Registering a teaching pattern
is not registering a production detector.
"""
from dataclasses import dataclass
from types import MappingProxyType
from ..domain import Evidence
from ..context.models import EvidenceAnchor, Omission
from ..context.report import valid_anchor

POLICY_VERSION = 'pedagogy.v1'
PATTERN_VERSION = 'patterns.v1'

@dataclass(frozen=True)
class Pattern:
    concept: str
    requirements: tuple[str, ...]
    exclusions: tuple[str, ...]
    detector: str
    prompts: tuple[tuple[str, str], ...]
    retest: str
    version: str = PATTERN_VERSION

@dataclass(frozen=True)
class PatternFacts:
    concept: str
    anchor: EvidenceAnchor
    detector: str
    observations: tuple[str, ...]

@dataclass(frozen=True)
class PatternAssessment:
    status: str
    reason: str
    pattern: Pattern | None

_WAVE = ('exact', 'complete', 'all_pages', 'initial_values', 'timescale')

def _pattern(concept, requirements, exclusion, detector, questions, retest):
    hints = ('先比较证据中的现象与设计目标。', '缩小到相关声明、连接或行为条件。', '逐项对照目标与证据，选择一个可验证的差异。')
    return Pattern(concept, requirements, (exclusion,), detector, tuple(zip(hints, questions)), retest)

PATTERNS = MappingProxyType({p.concept: p for p in (
    _pattern('syntax', ('parser_diagnostic', 'source_location'), 'accepted_language_construct', 'unavailable:parser',
        ('解析器指出的位置对应哪种语法结构？', '该结构的定界与相邻声明是否完整？', '最小片段能否在同一语言模式下解析？'), '固定语言模式与源文件版本，重新解析并核对诊断位置。'),
    Pattern('latch', ('storage_observed', 'combinational_goal'), ('intentional_storage',), 'report:EDU_LATCH_INFERRED', (
        ('报告观察到了存储行为。', '你期望输出只依赖当前输入，还是需要记住过去？'),
        ('可以检查相关组合逻辑块的赋值覆盖范围。', '每一种输入条件下，输出是否都有定义？'),
        ('沿着条件不成立的路径检查输出赋值。', '该路径期望输出什么，代码是否描述了这个情况？')),
        '在新 revision 重新综合并比较存储诊断；保留有意存储的设计目标。'),
    _pattern('width_signedness', ('operand_types', 'expression_width', 'expected_range'), 'intentional_conversion', 'unavailable:type-analysis',
        ('期望的数值范围和符号是什么？', '操作数与表达式的位宽、符号属性分别是什么？', '边界值经过扩展或截断后是否仍符合预期？'), '固定输入边界值与类型上下文，复查扩展、截断和数值结果。'),
    _pattern('drivers', ('driver_inventory', 'net_kind'), 'legal_resolved_net', 'unavailable:driver-analysis',
        ('信号预期由哪些位置驱动？', '驱动清单与网络类型是否一致？', '在哪个条件下会出现驱动缺失或冲突？'), '对同一信号重新生成驱动清单，核对合法解析网络与使能条件。'),
    _pattern('combinational_feedback', ('dependency_cycle', 'combinational_path'), 'sequential_boundary', 'unavailable:dependency-analysis',
        ('输出到输入之间是否存在返回路径？', '路径中是否存在时序边界？', '逐段检查依赖，哪一段与预期结构不同？'), '重新提取依赖路径并确认环路中是否含时序边界。'),
    _pattern('ports', ('interface_declaration', 'connection_map'), 'intentional_open_port', 'unavailable:port-analysis',
        ('模块接口期望连接哪些信号？', '方向、位宽和连接映射是否一致？', '逐个核对有意悬空与遗漏连接的接口约定。'), '在同一顶层配置重新展开，比较接口声明和连接映射。'),
    _pattern('reset', ('reset_contract',) + _WAVE, 'contract_allows_behavior', 'unavailable:exact-waveform',
        ('复位约定中的极性和触发条件是什么？', '复位与时钟事件的先后关系是什么？', '精确事件与初值是否满足复位约定？'), '精确查询复位前后事件，包含所有分页、初值、timescale 和 artifact/revision 绑定。'),
    _pattern('simulation_mismatch', ('expected_result', 'observed_result') + _WAVE, 'comparison_matches', 'unavailable:exact-waveform',
        ('同一输入下期望观察到什么？', '最早出现差异的精确事件在哪里？', '比较窗口、初值和采样时刻是否一致？'), '固定测试输入、期望和采样窗口，用完整精确事件重新比较结果。'),
)})


def assess(evidence: Evidence, facts: PatternFacts | None = None) -> PatternAssessment:
    """Fail closed on malformed, stale or unsupported host facts.

    Caller validates evidence against the current request (valid_evidence).
    Anchor equality additionally binds facts to evidence, not to free booleans.
    """
    if not isinstance(evidence, Evidence) or type(evidence.concept) is not str:
        return PatternAssessment('insufficient', 'malformed_evidence', None)
    pattern = PATTERNS.get(evidence.concept)
    if pattern is None:
        return PatternAssessment('unsupported', 'unknown_pattern', None)
    # Optional course omissions describe background retrieval, not missing
    # engineering observations. Keep every engineering/malformed omission closed.
    if (evidence.complete is not True or type(evidence.omitted) is not tuple or any(
            not isinstance(item, Omission) or type(item.item_id) is not str
            or not item.item_id.startswith('course:') for item in evidence.omitted)):
        return PatternAssessment('insufficient', 'incomplete_evidence', pattern)
    if evidence.anchor is not None and (
            not valid_anchor(evidence.anchor, evidence.project_id, evidence.revision, evidence.job_id)
            or evidence.anchor.evidence_id != evidence.evidence_id):
        return PatternAssessment('insufficient', 'invalid_evidence_anchor', pattern)
    if facts is None:
        # Existing Evidence is emitted by trusted source ports. The report adapter
        # maps only EDU_LATCH_INFERRED; arbitrary log keywords never reach here.
        if evidence.concept == 'latch':
            if type(evidence.combinational_goal) is not bool:
                return PatternAssessment('insufficient', 'missing_goal', pattern)
            if evidence.origin not in ('fixture', 'tool', 'rule') or evidence.confidence_kind not in ('tool', 'rule'):
                return PatternAssessment('unsupported', 'untrusted_source', pattern)
            return PatternAssessment('supported' if evidence.combinational_goal else 'excluded', 'legacy_latch_goal', pattern)
        return PatternAssessment('unsupported', 'detector_unavailable', pattern)
    if (not isinstance(facts, PatternFacts) or type(facts.concept) is not str
            or type(facts.detector) is not str or type(facts.observations) is not tuple
            or not all(type(v) is str for v in facts.observations)
            or not valid_anchor(facts.anchor, evidence.project_id, evidence.revision, evidence.job_id)
            or facts.anchor != evidence.anchor or facts.anchor.evidence_id != evidence.evidence_id
            or facts.concept != evidence.concept):
        return PatternAssessment('insufficient', 'invalid_or_unbound_facts', pattern)
    if evidence.origin != 'fixture' or facts.detector != 'synthetic.v1':
        return PatternAssessment('unsupported', 'detector_unavailable', pattern)
    observed = frozenset(facts.observations)
    if observed - set(pattern.requirements + pattern.exclusions):
        return PatternAssessment('insufficient', 'unknown_fact', pattern)
    if observed.intersection(pattern.exclusions):
        return PatternAssessment('excluded', 'legal_counterexample', pattern)
    if not set(pattern.requirements).issubset(observed):
        return PatternAssessment('insufficient', 'missing_requirements', pattern)
    return PatternAssessment('supported', 'synthetic_policy_fixture', pattern)
