"""Offline U3 -> official U2 citation plumbing probe, not teaching acceptance."""
import argparse
from dataclasses import asdict
import json
from pathlib import Path

from sigflow_edu_agent.courses import SQLiteCourseProvider
from sigflow_edu_agent.demo import FixtureSource
from sigflow_edu_agent.domain import RunRequest
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.pedagogy import RuleProvider


class OfflineCitationProbe:
    """Select a provided ID to test plumbing; never calls a model API."""
    def __init__(self):
        self.admitted = []

    def generate(self, context):
        self.admitted = [ref.passage_id for ref in context.course_refs]
        candidate = RuleProvider().generate(context)
        candidate['course_passage_ids'] = self.admitted[:1]
        return candidate


def main():
    parser = argparse.ArgumentParser(description='离线课程引用结构联调，不运行真实模型或 EDA')
    parser.add_argument('--course-db', type=Path)
    parser.add_argument('--question', default='组合逻辑中为什么出现锁存器？')
    args = parser.parse_args()
    provider = OfflineCitationProbe()
    loop = TeachingLoop(RunRequest('demo', 'r1', 'latch', args.question), FixtureSource(), provider,
                        course_provider=SQLiteCourseProvider(args.course_db))
    state = loop.run_until_pause()
    print(json.dumps({
        'mode': 'offline_citation_plumbing_probe',
        'provider': 'offline_stub',
        'evidence_origin': 'fixture',
        'real_model_called': False,
        'real_eda_executed': False,
        'course_status': state.course_status,
        'admitted_passage_ids': provider.admitted,
        'omitted': [asdict(item) for item in state.course_omitted],
        'card': asdict(state.cards[0]),
        'limitations': '固定候选响应仅检查接入和引用结构，不评测真实模型教学质量。',
    }, ensure_ascii=False, indent=2))
    return 0 if state.cards[0].course_refs else 1


if __name__ == '__main__':
    raise SystemExit(main())
