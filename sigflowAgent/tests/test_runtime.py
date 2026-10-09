import unittest
from concurrent.futures import ThreadPoolExecutor
from threading import Barrier

from sigflow_edu_agent.demo import FixtureSource
from sigflow_edu_agent.domain import RunRequest, Status
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.runtime.session import TeachingSession


class SessionTests(unittest.TestCase):
    def make_session(self):
        return TeachingSession(TeachingLoop(RunRequest('p', 'r', 'i', '锁存器问题'), FixtureSource()))

    def test_one_step_advances_one_phase_and_replay_is_denied(self):
        session = self.make_session()
        before = session.state
        state = session.advance(before.version)
        self.assertEqual(state.stage, 'E1')
        self.assertTrue(session.check_phase('E0')[0])
        with self.assertRaisesRegex(ValueError, 'stale_state'):
            session.advance(before.version)
        self.assertEqual(session.state, state)

    def test_check_is_read_only_and_cannot_accept_model_pass_claim(self):
        session = self.make_session()
        before = session.state
        for _ in range(3):
            ok, diagnostic = session.check_phase('E0')
            self.assertFalse(ok)
            self.assertIn('next_action', diagnostic)
        self.assertEqual(session.state, before)

    def test_pause_cannot_be_advanced_by_model(self):
        session = self.make_session()
        for _ in range(5):
            session.advance(session.state.version)
        state = session.state
        self.assertEqual(state.status, Status.WAITING_STUDENT)
        self.assertTrue(session.check_phase('E4')[0])
        self.assertFalse(session.check_phase('E5')[0])
        self.assertEqual(session.advance(state.version), state)
        session.host_action('hint_next', 'ui-action', state.version)
        self.assertEqual(session.state.level, 2)
        self.assertTrue(session.check_phase('E5')[0])
        self.assertFalse(session.check_phase('E4')[0])

    def test_failed_or_stale_phase_never_passes_gate(self):
        session = self.make_session()
        session.advance(0)
        session.advance(1)
        self.assertFalse(session.check_phase('E0')[0])
        session.host_action('cancel', 'stop', session.state.version)
        self.assertFalse(session.check_phase('E1')[0])

    def test_instance_state_is_isolated(self):
        left, right = self.make_session(), self.make_session()
        left.advance(0)
        self.assertEqual(right.state.version, 0)
        self.assertFalse(right.check_phase('E0')[0])

    def test_student_finish_completes_e5_but_cancel_does_not(self):
        for action, expected in (('finish', True), ('cancel', False)):
            with self.subTest(action=action):
                session = self.make_session()
                for _ in range(5):
                    session.advance(session.state.version)
                session.host_action(action, 'host-stop', session.state.version)
                self.assertEqual(session.check_phase('E5')[0], expected)
                self.assertFalse(session.check_phase('E4')[0])

    def test_concurrent_calls_with_same_version_advance_only_once(self):
        session = self.make_session()
        barrier = Barrier(2)
        def advance():
            barrier.wait(timeout=3)
            try:
                return session.advance(0).version
            except ValueError as error:
                return str(error)
        with ThreadPoolExecutor(max_workers=2) as executor:
            results = list(executor.map(lambda _: advance(), range(2)))
        self.assertCountEqual(results, [1, 'stale_state'])
        self.assertEqual(session.state.version, 1)


if __name__ == '__main__':
    unittest.main()
