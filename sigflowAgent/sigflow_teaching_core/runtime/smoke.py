"""Offline real-UCAgent integration smoke; no default VerifyAgent is started."""

import json
import os
from pathlib import Path
import sys
import tempfile

from ..demo import FixtureSource
from ..domain import RunRequest, Status
from ..loop import TeachingLoop
from .session import TeachingSession


def main():
    source = os.environ.get('SIGFLOW_UCAGENT_SOURCE', '')
    if not source or not (Path(source) / 'ucagent' / '__init__.py').is_file():
        raise SystemExit('Set SIGFLOW_UCAGENT_SOURCE to the trusted upstream checkout.')
    sys.path.insert(0, str(Path(source).resolve()))
    import ucagent
    from .ucagent_adapter import build_stage, build_tools

    session = TeachingSession(TeachingLoop(RunRequest('demo', 'r1', 'latch', 'Explain the evidence', 'explain'), FixtureSource()))
    tool, = build_tools(session)
    transitions = []
    with tempfile.TemporaryDirectory(prefix='sigflow-edu-') as workspace:
        for phase in ('E0', 'E1', 'E2', 'E3', 'E4'):
            stage = build_stage(session, phase, workspace)
            stage.on_init()
            if stage.do_check(is_complete=True)[0]:
                raise RuntimeError('gate_passed_before_transition')
            transitions.append(tool.invoke({'expected_version': session.state.version}))
            if not stage.do_check(is_complete=True)[0]:
                raise RuntimeError('gate_rejected_transition')
    if session.state.status != Status.COMPLETED:
        raise RuntimeError('smoke_incomplete')
    print(json.dumps({'upstream': ucagent.__file__, 'evidence_origin': 'fixture',
                      'status': session.state.status, 'transitions': transitions}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
