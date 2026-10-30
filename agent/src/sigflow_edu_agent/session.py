"""Persistent session, run and event store.

When a data directory is supplied, the minimum learner/session state is saved
atomically so a restarted sidecar can expose historical results and resume
observation.  The store is guarded by a single lock because the HTTP server is
threaded.  Runtime instance identity and bearer credentials never enter this
file.

Caps are enforced rather than reported:

* at most :data:`MAX_SESSIONS` sessions, least recently used evicted first;
* at most :data:`MAX_RUNS_PER_SESSION` runs per session, trimmed oldest first;
* at most :data:`MAX_CARDS_PER_RUN` cards per run (enforced by ``cards.py``);
* at most :data:`MAX_EVENTS_PER_SESSION` events per session, trimmed from the
  oldest end, which is what makes ``after`` cursors expire with 410.
"""

from __future__ import annotations

import threading
import time
import json
import hashlib
import os
from pathlib import Path
from contextlib import contextmanager
from copy import deepcopy
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Any, Optional

from .util import decimal, iso_utc
from .version import MAX_EVENTS_PER_SESSION, MAX_SESSIONS
from .errors import AgentError

MAX_RUNS_PER_SESSION: int = 64
"""Per-session run retention cap; oldest runs are removed first."""

MAX_SESSION_RUN_BYTES: int = 2 * 1024 * 1024
"""Per-session serialized-card budget for retained runs."""


def _numeric_suffix(value: Any, prefix: str) -> int:
    """Read a persisted monotone id without allowing malformed data to lower it."""
    if not isinstance(value, str) or not value.startswith(prefix):
        return 0
    suffix = value[len(prefix):]
    return int(suffix) if suffix.isdigit() else 0


@dataclass
class RunRecord:
    """One completed (or failed) agent run."""

    run_id: str
    session_id: str
    kind: str
    level: str
    state: str
    state_version: int
    cards: list[dict[str, Any]] = field(default_factory=list)
    omitted: list[dict[str, str]] = field(default_factory=list)
    model_used: bool = False
    error_code: Optional[str] = None
    error_message: Optional[str] = None
    created_at: str = ""

    def to_wire(self) -> dict[str, Any]:
        """Render the run view returned by the run routes."""
        payload: dict[str, Any] = {
            "run_id": self.run_id,
            "session_id": self.session_id,
            "kind": self.kind,
            "level": self.level,
            "state": self.state,
            "state_version": decimal(self.state_version),
            "cards": deepcopy(self.cards),
            "omitted": deepcopy(self.omitted),
            "model_used": self.model_used,
        }
        if self.error_code:
            payload["error"] = {
                "code": self.error_code,
                "message": self.error_message or "",
            }
        return payload


@dataclass
class SessionRecord:
    """One teaching session bound to a project revision."""

    session_id: str
    project_id: str
    revision: str
    top: str
    target: Any
    locale: str
    state_version: int = 1
    created_at: str = ""
    last_used: float = 0.0
    runs: "OrderedDict[str, RunRecord]" = field(default_factory=OrderedDict)
    events: list[dict[str, Any]] = field(default_factory=list)
    sequence: int = 0
    active_requests: int = 0
    run_lock: Any = field(default_factory=threading.RLock, repr=False)
    run_bytes: int = 0
    workspace_id: str = ""
    learner_id: str = ""
    status: str = "active"
    read_only: bool = False
    recovery_required: bool = False
    invalidated_reason: str = ""

    def to_wire(self) -> dict[str, Any]:
        """Render the session summary returned by the session routes."""
        return {
            "session_id": self.session_id,
            "project_id": self.project_id,
            "revision": self.revision,
            "top": self.top,
            "target": deepcopy(self.target),
            "locale": self.locale,
            "state_version": decimal(self.state_version),
            "created_at": self.created_at,
            "run_count": len(self.runs),
            "event_count": len(self.events),
            "workspace_id": self.workspace_id,
            "learner_id": self.learner_id,
            "status": self.status,
            "read_only": self.read_only,
            "recovery_required": self.recovery_required,
            "high_watermark": decimal(self.high_watermark),
            "oldest_sequence": decimal(self.oldest_sequence),
        }

    @property
    def oldest_sequence(self) -> int:
        """Sequence of the oldest retained event, or the current watermark."""
        if not self.events:
            return self.sequence
        return int(self.events[0]["sequence"])

    @property
    def high_watermark(self) -> int:
        """Highest sequence allocated for this session so far."""
        return self.sequence


class SessionStore:
    """Thread-safe LRU store of sessions, runs and events.

    The v1 HTTP contract remains unchanged, but production instances now
    persist the minimum state needed to reconnect a learner after a sidecar
    restart.  The file contains no bootstrap token, instance id or source
    path.  Runtime instance identity and bearer tokens stay process-local.
    """

    _STORE_SCHEMA = "sigflow.edu-agent.session-store.v1"

    def __init__(
        self,
        max_sessions: int = MAX_SESSIONS,
        data_directory: Optional[str] = None,
        *,
        workspace_id: Optional[str] = None,
        learner_id: Optional[str] = None,
    ) -> None:
        self._lock = threading.RLock()
        self._max_sessions = max(1, int(max_sessions))
        self._sessions: "OrderedDict[str, SessionRecord]" = OrderedDict()
        self._next_session = 0
        self._next_run = 0
        self._next_event = 0
        self._active_project_id: Optional[str] = None
        self._active_revision: Optional[str] = None
        self._path: Optional[Path] = None
        identity_seed = str(data_directory or "workspace-local").strip().encode("utf-8")
        stable_digest = hashlib.sha256(identity_seed).hexdigest()[:24]
        self.workspace_id = workspace_id or "ws-" + stable_digest
        self.learner_id = learner_id or "learner-" + hashlib.sha256(
            ("learner:" + self.workspace_id).encode("utf-8")
        ).hexdigest()[:24]
        if data_directory:
            self._path = Path(data_directory) / "agent-session-store.json"
            self._load()

    # -- sessions ---------------------------------------------------------

    def create_session(
        self,
        project_id: str,
        revision: str,
        top: str,
        target: Any,
        locale: str = "zh-CN",
    ) -> SessionRecord:
        """Create a session, evicting the least recently used one if needed."""
        with self._lock:
            self._invalidate_previous_bindings_locked(project_id, revision)
            if len(self._sessions) >= self._max_sessions:
                victim = next((key for key, value in self._sessions.items()
                               if value.active_requests == 0), None)
                if victim is None:
                    raise AgentError("RESOURCE_EXHAUSTED", "all sessions are busy", http_status=429)
                del self._sessions[victim]
            self._next_session += 1
            session_id = "sess-%d" % self._next_session
            record = SessionRecord(
                session_id=session_id,
                project_id=project_id,
                revision=revision,
                top=top,
                target=deepcopy(target),
                locale=locale,
                created_at=iso_utc(),
                last_used=time.monotonic(),
                workspace_id=self.workspace_id,
                learner_id=self.learner_id,
            )
            self._sessions[session_id] = record
            self._active_project_id = project_id
            self._active_revision = revision
            self._persist_locked()
            self._evict_locked()
            return record

    def _invalidate_previous_bindings_locked(self, project_id: str, revision: str) -> None:
        """Make old project/revision contexts historical before a new one is used."""
        # Fixture-only in-memory stores intentionally retain the historical
        # multi-project behaviour of the frozen v1 tests.  Production stores
        # have a stable workspace binding and enforce one active context.
        if self._path is None:
            return
        for record in self._sessions.values():
            if record.project_id == project_id and record.revision == revision:
                continue
            if record.read_only and record.status == "historical":
                continue
            record.read_only = True
            record.status = "historical"
            record.invalidated_reason = (
                "project switched" if record.project_id != project_id
                else "project revision changed"
            )
            self._append_event_locked(
                record,
                "session/invalidated",
                {
                    "reason": record.invalidated_reason,
                    "project_id": record.project_id,
                    "revision": record.revision,
                },
            )

    def get_session(self, session_id: str) -> Optional[SessionRecord]:
        """Return a session and mark it as most recently used."""
        with self._lock:
            record = self._sessions.get(session_id)
            if record is None:
                return None
            record.last_used = time.monotonic()
            self._sessions.move_to_end(session_id)
            return record

    def get_session_view(self, session_id: str) -> Optional[dict[str, Any]]:
        """Return a consistent session summary while holding the store lock."""
        with self._lock:
            record = self._sessions.get(session_id)
            if record is None:
                return None
            record.last_used = time.monotonic()
            self._sessions.move_to_end(session_id)
            return record.to_wire()

    def get_run_view(self, run_id: str) -> Optional[dict[str, Any]]:
        """Return a consistent run view while holding the store lock."""
        with self._lock:
            for session in self._sessions.values():
                record = session.runs.get(run_id)
                if record is not None:
                    return record.to_wire()
            return None

    def _evict_locked(self) -> None:
        """Drop the least recently used sessions beyond the cap."""
        while len(self._sessions) > self._max_sessions:
            self._sessions.popitem(last=False)

    @contextmanager
    def lease(self, session_id: str):
        """Pin before waiting; serialize one session without holding the global lock."""
        with self._lock:
            record = self.get_session(session_id)
            if record is None:
                raise AgentError("NOT_FOUND", "unknown session", http_status=404)
            if record.recovery_required:
                raise AgentError(
                    "STALE_REVISION",
                    "session execution state is unknown; reconcile before running again",
                    http_status=409,
                )
            if record.read_only:
                raise AgentError(
                    "STALE_REVISION",
                    "historical session is read-only after a project or revision change",
                    http_status=409,
                )
            record.active_requests += 1
            self._persist_locked()
        try:
            with record.run_lock:
                yield record
        finally:
            with self._lock:
                record.active_requests -= 1
                self._persist_locked()

    def commit_run(self, session: SessionRecord, **fields: Any) -> RunRecord:
        """Publish run, its cards and its completion event in one transaction."""
        with self._lock:
            if self._sessions.get(session.session_id) is not session:
                raise AgentError("NOT_FOUND", "session expired", http_status=404)
            record = self.create_run(session=session, **fields)
            for card in record.cards:
                card["state_version"] = decimal(record.state_version)
            self.append_event(session, "run/failed" if record.error_code else "run/finished",
                              {"run_id": record.run_id, "kind": record.kind,
                               "level": record.level, "state": record.state,
                               "state_version": decimal(record.state_version),
                               "card_count": len(record.cards)})
            return record

    # -- runs -------------------------------------------------------------

    def create_run(
        self,
        session: SessionRecord,
        kind: str,
        level: str,
        state: str,
        cards: list[dict[str, Any]],
        omitted: list[dict[str, str]],
        model_used: bool,
        error_code: Optional[str] = None,
        error_message: Optional[str] = None,
    ) -> RunRecord:
        """Append a run to a session and assign its committed state version."""
        with self._lock:
            if self._sessions.get(session.session_id) is not session:
                raise AgentError("NOT_FOUND", "session expired", http_status=404)
            if session.recovery_required:
                raise AgentError("STALE_REVISION", "session requires reconciliation", http_status=409)
            if session.read_only:
                raise AgentError("STALE_REVISION", "historical session is read-only", http_status=409)
            cards = deepcopy(cards)
            omitted = deepcopy(omitted)
            size = len(json.dumps(cards, ensure_ascii=False).encode("utf-8"))
            if size > MAX_SESSION_RUN_BYTES or session.state_version >= 2**64 - 1:
                raise AgentError("RESOURCE_EXHAUSTED", "session/run budget exceeded", http_status=429)
            self._next_run += 1
            run_id = "run-%d" % self._next_run
            session.state_version += 1
            record = RunRecord(
                run_id=run_id,
                session_id=session.session_id,
                kind=kind,
                level=level,
                state=state,
                state_version=session.state_version,
                cards=cards,
                omitted=omitted,
                model_used=model_used,
                error_code=error_code,
                error_message=error_message,
                created_at=iso_utc(),
            )
            session.runs[run_id] = record
            session.run_bytes += size
            while (len(session.runs) > MAX_RUNS_PER_SESSION
                   or session.run_bytes > MAX_SESSION_RUN_BYTES):
                _, removed = session.runs.popitem(last=False)
                session.run_bytes -= len(json.dumps(removed.cards, ensure_ascii=False).encode("utf-8"))
            self._persist_locked()
            return record

    def get_run(self, run_id: str) -> Optional[RunRecord]:
        """Return a run by id, or ``None`` when the id is unknown."""
        with self._lock:
            for session in self._sessions.values():
                record = session.runs.get(run_id)
                if record is not None:
                    return record
            return None

    # -- events -----------------------------------------------------------

    def append_event(
        self,
        session: SessionRecord,
        event_type: str,
        data: dict[str, Any],
    ) -> dict[str, Any]:
        """Append one session event and return it."""
        with self._lock:
            if self._sessions.get(session.session_id) is not session:
                raise AgentError("NOT_FOUND", "session expired", http_status=404)
            event = self._append_event_locked(session, event_type, data)
            self._persist_locked()
            return deepcopy(event)

    def _append_event_locked(
        self, session: SessionRecord, event_type: str, data: dict[str, Any]
    ) -> dict[str, Any]:
        """Append an event while the store lock is held."""
        self._next_event += 1
        session.sequence += 1
        event = {
            "sequence": decimal(session.sequence),
            "event_id": "ev-%d" % self._next_event,
            "type": event_type,
            "at": iso_utc(),
            "data": deepcopy(data),
        }
        session.events.append(event)
        if len(session.events) > MAX_EVENTS_PER_SESSION:
            del session.events[: len(session.events) - MAX_EVENTS_PER_SESSION]
        return event

    def close(self) -> None:
        """Flush durable state without renewing any execution authority."""
        with self._lock:
            self._persist_locked()

    def _load(self) -> None:
        """Load a previous snapshot and mark interrupted work for recovery."""
        assert self._path is not None
        if not self._path.exists():
            return
        try:
            payload = json.loads(self._path.read_text(encoding="utf-8"))
            if payload.get("schema_version") != self._STORE_SCHEMA:
                raise ValueError("unsupported session store schema")
            stored_workspace = payload.get("workspace_id")
            stored_learner = payload.get("learner_id")
            if isinstance(stored_workspace, str) and stored_workspace:
                self.workspace_id = stored_workspace
            if isinstance(stored_learner, str) and stored_learner:
                self.learner_id = stored_learner
            self._active_project_id = payload.get("active_project_id")
            self._active_revision = payload.get("active_revision")
            self._next_session = int(payload.get("next_session", 0))
            self._next_run = int(payload.get("next_run", 0))
            self._next_event = int(payload.get("next_event", 0))
            for item in payload.get("sessions", []):
                record = self._record_from_storage(item)
                if record is not None:
                    self._sessions[record.session_id] = record
                    self._next_session = max(self._next_session, _numeric_suffix(record.session_id, "sess-"))
                    for run in record.runs.values():
                        self._next_run = max(self._next_run, _numeric_suffix(run.run_id, "run-"))
                    for event in record.events:
                        self._next_event = max(self._next_event, _numeric_suffix(event.get("event_id"), "ev-"))
            self._evict_locked()
        except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError) as exc:
            raise AgentError(
                "SERVICE_UNAVAILABLE",
                "persistent session state is unavailable",
                http_status=503,
            ) from exc

    def _record_from_storage(self, item: Any) -> Optional[SessionRecord]:
        if not isinstance(item, dict):
            return None
        session_id = item.get("session_id")
        project_id = item.get("project_id")
        revision = item.get("revision")
        if not all(isinstance(value, str) and value for value in (session_id, project_id, revision)):
            return None
        runs: "OrderedDict[str, RunRecord]" = OrderedDict()
        run_bytes = 0
        for raw in item.get("runs", []):
            if not isinstance(raw, dict) or not isinstance(raw.get("run_id"), str):
                continue
            run = RunRecord(
                run_id=raw["run_id"],
                session_id=session_id,
                kind=str(raw.get("kind", "explain")),
                level=str(raw.get("level", "L1")),
                state=str(raw.get("state", "succeeded")),
                state_version=int(raw.get("state_version", 1)),
                cards=deepcopy(raw.get("cards", [])),
                omitted=deepcopy(raw.get("omitted", [])),
                model_used=bool(raw.get("model_used", False)),
                error_code=raw.get("error_code"),
                error_message=raw.get("error_message"),
                created_at=str(raw.get("created_at", "")),
            )
            runs[run.run_id] = run
            run_bytes += len(json.dumps(run.cards, ensure_ascii=False).encode("utf-8"))
        persisted_active = int(item.get("active_requests", 0) or 0)
        recovery_required = bool(item.get("recovery_required", False)) or persisted_active > 0
        status = str(item.get("status", "active"))
        if recovery_required:
            status = "recovery_required"
        return SessionRecord(
            session_id=session_id,
            project_id=project_id,
            revision=revision,
            top=str(item.get("top", "")),
            target=deepcopy(item.get("target", {})),
            locale=str(item.get("locale", "zh-CN")),
            state_version=int(item.get("state_version", 1)),
            created_at=str(item.get("created_at", "")),
            last_used=time.monotonic(),
            runs=runs,
            events=deepcopy(item.get("events", [])),
            sequence=int(item.get("sequence", 0)),
            active_requests=0,
            run_bytes=run_bytes,
            workspace_id=self.workspace_id,
            learner_id=self.learner_id,
            status=status,
            read_only=bool(item.get("read_only", False)),
            recovery_required=recovery_required,
            invalidated_reason=str(item.get("invalidated_reason", "")),
        )

    def _storage_payload_locked(self) -> dict[str, Any]:
        sessions = []
        for session in self._sessions.values():
            sessions.append({
                "session_id": session.session_id,
                "project_id": session.project_id,
                "revision": session.revision,
                "top": session.top,
                "target": deepcopy(session.target),
                "locale": session.locale,
                "state_version": session.state_version,
                "created_at": session.created_at,
                "active_requests": session.active_requests,
                "sequence": session.sequence,
                "events": deepcopy(session.events),
                "status": session.status,
                "read_only": session.read_only,
                "recovery_required": session.recovery_required,
                "invalidated_reason": session.invalidated_reason,
                "runs": [
                    {
                        "run_id": run.run_id,
                        "kind": run.kind,
                        "level": run.level,
                        "state": run.state,
                        "state_version": run.state_version,
                        "cards": deepcopy(run.cards),
                        "omitted": deepcopy(run.omitted),
                        "model_used": run.model_used,
                        "error_code": run.error_code,
                        "error_message": run.error_message,
                        "created_at": run.created_at,
                    }
                    for run in session.runs.values()
                ],
            })
        return {
            "schema_version": self._STORE_SCHEMA,
            "workspace_id": self.workspace_id,
            "learner_id": self.learner_id,
            "active_project_id": self._active_project_id,
            "active_revision": self._active_revision,
            "next_session": self._next_session,
            "next_run": self._next_run,
            "next_event": self._next_event,
            "sessions": sessions,
        }

    def _persist_locked(self) -> None:
        """Atomically write the durable snapshot when persistence is enabled."""
        if self._path is None:
            return
        self._path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self._path.with_name(self._path.name + ".tmp")
        temporary.write_text(
            json.dumps(self._storage_payload_locked(), ensure_ascii=False, separators=(",", ":")),
            encoding="utf-8",
        )
        os.replace(str(temporary), str(self._path))

    def read_events(
        self,
        session: SessionRecord,
        after: Optional[int],
        limit: int = 100,
    ) -> tuple[Optional[list[dict[str, Any]]], int, int]:
        """Return ``(events, high_watermark, oldest_sequence)`` for a cursor.

        ``events`` is ``None`` when the request must be answered with
        ``410 CURSOR_EXPIRED`` because the cursor fell out of the retention
        window.
        """
        with self._lock:
            if self._sessions.get(session.session_id) is not session:
                raise AgentError("NOT_FOUND", "session expired", http_status=404)
            high = session.sequence
            oldest = session.oldest_sequence
            cursor = 0 if after is None else int(after)
            if cursor > high:
                return None, high, oldest
            if after is not None and cursor < oldest - 1:
                return None, high, oldest
            events = [
                event
                for event in session.events
                if int(event["sequence"]) > cursor
            ]
            return deepcopy(events[:limit]), high, oldest
