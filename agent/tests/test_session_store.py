"""T8 acceptance tests for in-memory sessions, runs, events and concurrency."""

from __future__ import annotations

import os
import sys
import threading
import unittest
from typing import Any

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_DIR = os.path.dirname(TESTS_DIR)
SRC_DIR = os.path.join(AGENT_DIR, "src")
if AGENT_DIR not in sys.path:
    sys.path.insert(0, AGENT_DIR)
if SRC_DIR not in sys.path:
    sys.path.insert(0, SRC_DIR)

from sigflow_edu_agent.errors import AgentError  # noqa: E402
from sigflow_edu_agent.session import (  # noqa: E402
    MAX_EVENTS_PER_SESSION,
    MAX_RUNS_PER_SESSION,
    SessionStore,
)
from sigflow_edu_agent.version import MAX_SESSIONS  # noqa: E402
from tests.test_http_api import SidecarFixture  # noqa: E402


def _create(store: SessionStore, revision: str = "rev-1"):
    """Create one deterministic session in a store."""
    return store.create_session(
        project_id="prj-1",
        revision=revision,
        top="top",
        target={"profile": revision},
    )


def _commit(
    store: SessionStore,
    session,
    *,
    state: str = "succeeded",
    cards: list[dict[str, Any]] | None = None,
):
    """Commit one simple run through the production transaction."""
    return store.commit_run(
        session=session,
        kind="explain",
        level="L1",
        state=state,
        cards=cards if cards is not None else [{"card_id": "card-1", "state_version": "0"}],
        omitted=[],
        model_used=False,
    )


class SessionLimitTests(unittest.TestCase):
    """32-session LRU and in-flight pinning."""

    def test_default_session_cap_is_thirty_two(self) -> None:
        self.assertEqual(MAX_SESSIONS, 32)

    def test_lru_evicts_the_least_recently_used_session(self) -> None:
        store = SessionStore(max_sessions=3)
        first = _create(store, "rev-a")
        second = _create(store, "rev-b")
        third = _create(store, "rev-c")
        self.assertIsNotNone(store.get_session(first.session_id))

        fourth = _create(store, "rev-d")
        self.assertIs(store.get_session(first.session_id), first)
        self.assertIsNone(store.get_session(second.session_id))
        self.assertIs(store.get_session(third.session_id), third)
        self.assertIs(store.get_session(fourth.session_id), fourth)

    def test_in_flight_session_cannot_be_evicted_and_stale_writes_fail(self) -> None:
        store = SessionStore(max_sessions=1)
        active = _create(store, "rev-active")
        with store.lease(active.session_id):
            with self.assertRaises(AgentError) as caught:
                _create(store, "rev-new")
            self.assertEqual(caught.exception.code, "RESOURCE_EXHAUSTED")
            self.assertIs(store.get_session(active.session_id), active)

        replacement = _create(store, "rev-replacement")
        self.assertIsNone(store.get_session(active.session_id))
        self.assertIs(store.get_session(replacement.session_id), replacement)
        with self.assertRaises(AgentError) as caught:
            _commit(store, active)
        self.assertEqual(caught.exception.code, "NOT_FOUND")
        with self.assertRaises(AgentError) as caught:
            store.append_event(active, "stale/write", {})
        self.assertEqual(caught.exception.code, "NOT_FOUND")


class RunRetentionTests(unittest.TestCase):
    """64-run retention and commit-time state_version assignment."""

    def test_run_cap_keeps_the_newest_sixty_four(self) -> None:
        store = SessionStore()
        session = _create(store)
        for index in range(MAX_RUNS_PER_SESSION + 6):
            _commit(
                store,
                session,
                cards=[{"card_id": "card-1", "index": index, "state_version": "0"}],
            )

        self.assertEqual(len(session.runs), MAX_RUNS_PER_SESSION)
        self.assertEqual(next(iter(session.runs)), "run-7")
        self.assertEqual(next(reversed(session.runs)), "run-70")
        self.assertEqual(session.state_version, 71)

    def test_commit_assigns_one_version_to_run_cards_and_event(self) -> None:
        store = SessionStore()
        session = _create(store)
        cards = [
            {"card_id": "card-1", "state_version": "0"},
            {"card_id": "card-2", "state_version": "0"},
        ]
        snapshot = [dict(card) for card in cards]
        run = _commit(store, session, cards=cards)

        event = store.read_events(session, after=0)[0][-1]
        self.assertEqual(run.state_version, 2)
        self.assertEqual(event["data"]["state_version"], "2")
        self.assertEqual(
            [card["state_version"] for card in run.cards],
            ["2", "2"],
        )
        self.assertEqual(cards, snapshot, "commit must not mutate caller cards")

    def test_run_bytes_budget_is_enforced(self) -> None:
        store = SessionStore()
        session = _create(store)
        oversized = [{"card_id": "card-1", "body": "x" * (2 * 1024 * 1024)}]
        with self.assertRaises(AgentError) as caught:
            _commit(store, session, cards=oversized)
        self.assertEqual(caught.exception.code, "RESOURCE_EXHAUSTED")
        self.assertEqual(session.state_version, 1)
        self.assertEqual(len(session.runs), 0)


class EventRetentionTests(unittest.TestCase):
    """1000-event retention, cursor expiry and at-least-once reads."""

    def test_event_cap_keeps_newest_one_thousand_and_expires_old_cursor(self) -> None:
        store = SessionStore()
        session = _create(store)
        for index in range(MAX_EVENTS_PER_SESSION + 5):
            store.append_event(session, "test/event", {"index": str(index)})

        self.assertEqual(len(session.events), MAX_EVENTS_PER_SESSION)
        events, high, oldest = store.read_events(session, after=None, limit=2000)
        assert events is not None
        self.assertEqual(high, 1005)
        self.assertEqual(oldest, 6)
        self.assertEqual([event["sequence"] for event in events], [str(i) for i in range(6, 1006)])

        valid, _, _ = store.read_events(session, after=oldest - 1, limit=2000)
        self.assertIsNotNone(valid)
        expired_old, _, _ = store.read_events(session, after=oldest - 2)
        expired_future, _, _ = store.read_events(session, after=high + 1)
        self.assertIsNone(expired_old)
        self.assertIsNone(expired_future)

    def test_reads_repeat_event_ids_and_return_copies(self) -> None:
        store = SessionStore()
        session = _create(store)
        payload = {"nested": {"value": "before"}}
        appended = store.append_event(session, "test/event", payload)
        payload["nested"]["value"] = "caller-mutated"
        appended["data"]["nested"]["value"] = "return-mutated"

        first, _, _ = store.read_events(session, after=0)
        second, _, _ = store.read_events(session, after=0)
        assert first is not None and second is not None
        self.assertEqual(first, second)
        self.assertEqual([event["event_id"] for event in first], ["ev-1"])
        self.assertEqual(first[0]["data"]["nested"]["value"], "before")
        first[0]["data"]["nested"]["value"] = "read-mutated"

        third, _, _ = store.read_events(session, after=0)
        assert third is not None
        self.assertEqual(third[0]["data"]["nested"]["value"], "before")


class ConcurrentCommitTests(unittest.TestCase):
    """Concurrent writes must be serialized into one version/event sequence."""

    def test_concurrent_commits_have_unique_versions_and_event_ids(self) -> None:
        store = SessionStore()
        session = _create(store)
        count = 24
        barrier = threading.Barrier(count)
        errors: list[BaseException] = []
        runs = []
        lock = threading.Lock()

        def worker(index: int) -> None:
            try:
                barrier.wait(timeout=5)
                run = _commit(
                    store,
                    session,
                    cards=[{"card_id": "card-1", "worker": index, "state_version": "0"}],
                )
                with lock:
                    runs.append(run)
            except BaseException as exc:  # pragma: no cover - reported below
                with lock:
                    errors.append(exc)

        threads = [threading.Thread(target=worker, args=(index,)) for index in range(count)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=10)

        self.assertEqual(errors, [])
        self.assertEqual(len(runs), count)
        self.assertEqual(session.state_version, count + 1)
        self.assertEqual(len({run.run_id for run in runs}), count)
        self.assertEqual({run.state_version for run in runs}, set(range(2, count + 2)))
        for run in runs:
            self.assertEqual(run.cards[0]["state_version"], str(run.state_version))

        events, high, oldest = store.read_events(session, after=0, limit=count)
        assert events is not None
        self.assertEqual((oldest, high), (1, count))
        self.assertEqual(len({event["event_id"] for event in events}), count)
        self.assertEqual([event["sequence"] for event in events], [str(i) for i in range(1, count + 1)])
        self.assertEqual(
            {int(event["data"]["state_version"]) for event in events},
            set(range(2, count + 2)),
        )


class NoLongPollingTests(unittest.TestCase):
    """Interface A stays immediate and has no Agent state route."""

    def test_wait_ms_is_not_supported(self) -> None:
        fixture = SidecarFixture()
        try:
            session_id = fixture.create_session()
            response = fixture.request(
                "GET",
                "/api/v1/sessions/%s/events?after=0&wait_ms=25000" % session_id,
            )
            self.assertEqual(response.status, 400)
            assert response.payload is not None
            self.assertEqual(response.payload["error"]["code"], "INVALID_ARGUMENT")
        finally:
            fixture.close()

    def test_agent_state_route_does_not_exist(self) -> None:
        fixture = SidecarFixture()
        try:
            response = fixture.request("GET", "/api/v1/state")
            self.assertEqual(response.status, 404)
            assert response.payload is not None
            self.assertEqual(response.payload["error"]["code"], "NOT_FOUND")
        finally:
            fixture.close()


if __name__ == "__main__":  # pragma: no cover - manual execution
    unittest.main()
