"""Optional adapters to the real UCAgent runtime; no default agent tools loaded."""

from functools import partial
from pydantic import BaseModel, ConfigDict, PrivateAttr, StrictInt, ValidationError
from ucagent.checkers.base import Checker
from ucagent.stage.vstage import VerifyStage
from ucagent.tools.uctool import UCTool
from ucagent.util.config import Config

from .session import TeachingSession


class AdvanceArguments(BaseModel):
    model_config = ConfigDict(extra='forbid')
    expected_version: StrictInt


class EduAdvance(UCTool):
    name: str = 'edu_advance'
    description: str = 'Advance one teaching phase using the current state version.'
    args_schema: type[BaseModel] = AdvanceArguments
    _session: TeachingSession = PrivateAttr()

    def __init__(self, session: TeachingSession):
        super().__init__()
        self._session = session

    def _run(self, expected_version: int):
        state = self._session.advance(expected_version)
        return {'stage': state.stage, 'status': state.status,
                'state_version': state.version, 'reason': state.reason}

    def _parse_input(self, tool_input, tool_call_id):
        # UCTool otherwise logs Pydantic's raw input values on validation failure.
        try:
            return super()._parse_input(tool_input, tool_call_id)
        except ValidationError:
            raise ValueError('invalid_tool_arguments') from None


class TeachingStageChecker(Checker):
    def __init__(self, session, phase, cfg=None):
        super().__init__()
        self.session = session
        self.phase = phase

    def do_check(self, is_complete=False, **kwargs):
        return self.session.check_phase(self.phase)

    def get_template_data(self):
        state = self.session.state
        return {'edu_stage': state.stage, 'edu_status': state.status,
                'edu_state_version': state.version}


def validate_tools(tools):
    if (not isinstance(tools, (list, tuple)) or len(tools) != 1
            or type(tools[0]) is not EduAdvance or tools[0].name != 'edu_advance'):
        raise ValueError('tool_registry_denied')
    return tuple(tools)


def build_tools(session):
    return validate_tools([EduAdvance(session)])


def build_stage(session, phase, workspace):
    if phase not in ('E0', 'E1', 'E2', 'E3', 'E4', 'E5', 'E6', 'E7'):
        raise ValueError('invalid_phase')
    cfg = Config({'_temp_cfg': {'OUT': 'output', 'DUT': 'edu'},
                  'hist_ignore_pattern': [], 'skill': {'use_skill': False}})
    checker = Config({'name': 'teaching_gate', 'clss': 'TeachingStageChecker',
                      'args': {'phase': phase}, 'extra_args': {}})
    return VerifyStage(
        cfg=cfg, workspace=workspace, name=phase, description='Teaching phase gate',
        task=['Advance the current phase; wait for trusted host events when paused.'],
        checker=[checker], reference_files=[], skill_list=[], output_files=[],
        force_use_skill=False, pre_cmds=[], post_cmds=[],
        checker_registry={'TeachingStageChecker': partial(TeachingStageChecker, session=session)},
    )
