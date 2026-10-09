"""Single-writer boundary between the host UI and UCAgent stage tools."""

from threading import RLock

from ..domain import Status
from ..loop import TeachingLoop


class TeachingSession:
    def __init__(self, loop: TeachingLoop):
        self._loop = loop
        self._lock = RLock()
        self._observed_phase = None
        self._observed_version = None

    @property
    def state(self):
        with self._lock:
            return self._loop.state

    def advance(self, expected_version: int):
        with self._lock:
            before = self._loop.state
            if type(expected_version) is not int or expected_version != before.version:
                raise ValueError('stale_state')
            after = self._loop.step()
            if after.version != before.version:
                self._observed_phase = before.stage
                self._observed_version = after.version
            return after

    def host_action(self, kind, action_id, expected_version, *, grant_ref=''):
        """Trusted host only; deliberately absent from the model tool registry."""
        with self._lock:
            before = self._loop.state
            after = self._loop.act(kind, action_id, expected_version, grant_ref=grant_ref)
            if after.version != before.version:
                self._observed_phase = 'E5' if before.stage == 'E5' else None
                self._observed_version = after.version
            return after

    def check_phase(self, phase):
        """Read-only gate on the latest transition, never on model assertions."""
        with self._lock:
            state = self._loop.state
            passed = (
                phase in ('E0', 'E1', 'E2', 'E3', 'E4', 'E5', 'E6', 'E7')
                and self._observed_phase == phase
                and self._observed_version == state.version
                and state.status in {Status.RUNNING, Status.COMPLETED,
                                     Status.WAITING_STUDENT, Status.WAITING_APPROVAL}
                and (phase != 'E5' or state.status in {Status.RUNNING, Status.COMPLETED})
                and (phase != 'E6' or state.stage == 'E7')
            )
            if passed:
                return True, {'phase': phase, 'state_version': state.version, 'status': state.status}
            return False, {
                'error_code': 'EDU_STAGE_PENDING',
                'error': 'The required trusted transition has not completed.',
                'next_action': ('wait_for_host_action' if state.status != Status.RUNNING
                                else 'advance_current_phase'),
                'observed': {'stage': state.stage, 'status': state.status, 'state_version': state.version},
            }
