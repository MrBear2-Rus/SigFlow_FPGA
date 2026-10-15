"""Read-only U2 integration example; engineering evidence is an explicit fixture."""
import argparse
import json
from pathlib import Path

from sigflow_edu_agent.courses import SQLiteCourseProvider
from sigflow_edu_agent.domain import Evidence, RunRequest
from sigflow_edu_agent.loop import TeachingLoop


class FixtureSource:
    def collect(self, project_id, revision):
        return Evidence('example-fixture',project_id,revision,'offline','unknown',True,
                        'Offline integration fixture; no engineering conclusion.',origin='fixture')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--db',type=Path,default=Path('data/course.sqlite'))
    parser.add_argument('--question',default='Verilator UNDRIVEN 是什么意思？')
    args=parser.parse_args()
    state=TeachingLoop(RunRequest('example','frozen','example',args.question),FixtureSource(),
                       course_provider=SQLiteCourseProvider(args.db)).run_until_pause()
    print(json.dumps({'stage':state.stage,'status':state.status,'course_status':state.course_status,
                      'course_diagnostics':state.course_diagnostics,
                      'note':'debug.packed_evidence lists the admitted chunk allowlist. Rule fallback makes no course citations.'},
                     ensure_ascii=False,indent=2))


if __name__=='__main__':main()
