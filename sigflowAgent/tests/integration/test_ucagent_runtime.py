import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SOURCE = os.environ.get('SIGFLOW_UCAGENT_SOURCE')
if SOURCE:
    source_path = Path(SOURCE).resolve()
    if not (source_path / 'ucagent' / '__init__.py').is_file():
        raise RuntimeError('invalid_SIGFLOW_UCAGENT_SOURCE')
    sys.path.insert(0, str(source_path))
    from ucagent.checkers.base import Checker
    from ucagent.stage.vstage import VerifyStage
    from ucagent.tools.uctool import UCTool
    from sigflow_edu_agent.runtime.ucagent_adapter import build_stage, build_tools, validate_tools

from sigflow_edu_agent.demo import FixtureSource, FixtureVerification
from sigflow_edu_agent.domain import RunRequest, Status, Plan
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.runtime.session import TeachingSession


@unittest.skipUnless(SOURCE, 'Set SIGFLOW_UCAGENT_SOURCE and install the optional runtime dependencies.')
class UCAgentIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory()
        self.addCleanup(self.workspace.cleanup)
        self.session = TeachingSession(TeachingLoop(RunRequest('p', 'r', 'i', '解释', 'explain'), FixtureSource()))

    def test_real_stage_tool_and_checker_complete_explanation(self):
        tool, = build_tools(self.session)
        self.assertIsInstance(tool, UCTool)
        for phase in ('E0', 'E1', 'E2', 'E3', 'E4'):
            stage = build_stage(self.session, phase, self.workspace.name)
            self.assertIsInstance(stage, VerifyStage)
            self.assertIsInstance(stage.checker[0], Checker)
            stage.on_init()
            before = self.session.state
            self.assertFalse(stage.do_check(is_complete=True, stage_args={'passed': True})[0])
            self.assertEqual(self.session.state, before)
            tool.invoke({'expected_version': before.version})
            self.assertTrue(stage.do_check(is_complete=True)[0])
        self.assertEqual(self.session.state.status, Status.COMPLETED)

    def test_tool_has_no_host_action_or_grant_parameters(self):
        tool, = build_tools(self.session)
        for extras in ({'kind': 'hint_next'}, {'grant_ref': 'fake'}, {'expected_version': True}):
            with patch('ucagent.tools.uctool.fc.warning') as warning:
                with self.assertRaisesRegex(ValueError, '^invalid_tool_arguments$'):
                    tool.invoke({'expected_version': 0, **extras})
                warning.assert_not_called()
        self.assertEqual(self.session.state.version, 0)

    def test_old_model_tool_call_cannot_advance_twice(self):
        tool, = build_tools(self.session)
        tool.invoke({'expected_version': 0})
        with self.assertRaisesRegex(ValueError, 'stale_state'):
            tool.invoke({'expected_version': 0})
        self.assertEqual(self.session.state.version, 1)

    def test_registry_rejects_unknown_or_duplicate_tools(self):
        tool, = build_tools(self.session)
        for tools in ([tool, object()], [tool, tool], []):
            with self.assertRaisesRegex(ValueError, 'tool_registry_denied'):
                validate_tools(tools)

    def test_template_projection_does_not_advance_loop(self):
        stage = build_stage(self.session, 'E0', self.workspace.name)
        before = self.session.state
        for _ in range(3):
            stage.checker[0].get_template_data()
        self.assertEqual(self.session.state, before)

    def test_real_gates_wait_for_host_approval_then_verify_and_finish(self):
        source = FixtureSource()
        self.session = TeachingSession(TeachingLoop(
            RunRequest('p', 'r', 'i', 'verify', 'verify'), source,
            plan=Plan('p', 'r', 'check', ('eda.synth',)), verification=FixtureVerification(source)))
        tool, = build_tools(self.session)
        for _ in range(5):
            tool.invoke({'expected_version': self.session.state.version})
        gate = build_stage(self.session, 'E5', self.workspace.name)
        gate.on_init()
        paused = self.session.state
        for _ in range(2):
            tool.invoke({'expected_version': paused.version})
            self.assertFalse(gate.do_check(is_complete=True)[0])
        self.assertEqual(self.session.state, paused)
        self.session.host_action('execute_plan', 'fixture-host', paused.version, grant_ref='fixture-only-grant')
        self.assertTrue(gate.do_check(is_complete=True)[0])
        for phase in ('E6', 'E7'):
            gate = build_stage(self.session, phase, self.workspace.name)
            gate.on_init()
            self.assertFalse(gate.do_check(is_complete=True)[0])
            tool.invoke({'expected_version': self.session.state.version})
            self.assertTrue(gate.do_check(is_complete=True)[0])
        self.assertEqual(self.session.state.status, Status.COMPLETED)


if __name__ == '__main__':
    unittest.main()
