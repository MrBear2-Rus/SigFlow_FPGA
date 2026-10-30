from __future__ import annotations

import os
import hashlib
import json
import sqlite3
from contextlib import contextmanager
from pathlib import Path
from typing import Callable, Iterable, Optional


class MigrationError(RuntimeError):
    pass


def default_database_path() -> Path:
    base = Path(os.environ.get("LOCALAPPDATA", Path.home() / ".local" / "share"))
    return base / "SigFlowEduAgent" / "learning.sqlite"


CORE_SCHEMA = (
    """CREATE TABLE learner_profile (
        profile_id TEXT PRIMARY KEY, display_name TEXT NOT NULL,
        preferences TEXT NOT NULL, preferences_explicit INTEGER NOT NULL DEFAULT 0,
        created_at TEXT NOT NULL, updated_at TEXT NOT NULL)""",
    """CREATE TABLE sessions (
        session_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL REFERENCES learner_profile ON DELETE CASCADE,
        project_id TEXT NOT NULL, instance_id TEXT NOT NULL, status TEXT NOT NULL,
        created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
        UNIQUE(profile_id, session_id))""",
    """CREATE TABLE runs (
        run_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL, session_id TEXT NOT NULL,
        revision TEXT NOT NULL, snapshot_id TEXT NOT NULL, policy_version TEXT NOT NULL,
        workflow_version TEXT NOT NULL, model_version TEXT NOT NULL, execution_mode TEXT NOT NULL,
        status TEXT NOT NULL, state_version INTEGER NOT NULL DEFAULT 0, hint_level INTEGER NOT NULL DEFAULT 1,
        created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
        UNIQUE(profile_id, session_id, run_id),
        FOREIGN KEY(profile_id, session_id) REFERENCES sessions(profile_id, session_id) ON DELETE CASCADE)""",
    """CREATE TABLE issues (
        issue_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL REFERENCES learner_profile ON DELETE CASCADE,
        project_id TEXT NOT NULL, instance_id TEXT NOT NULL, diagnostic_type TEXT NOT NULL,
        logical_object TEXT, goal_id TEXT NOT NULL, identity_key TEXT NOT NULL,
        concept_ids TEXT NOT NULL, status TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
        UNIQUE(profile_id, project_id, instance_id, identity_key), UNIQUE(profile_id, issue_id))""",
    """CREATE TABLE occurrences (
        occurrence_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL, session_id TEXT NOT NULL, run_id TEXT NOT NULL,
        issue_id TEXT NOT NULL, revision TEXT NOT NULL, job_id TEXT NOT NULL, status TEXT NOT NULL,
        created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
        UNIQUE(profile_id, occurrence_id), UNIQUE(issue_id, revision, job_id),
        FOREIGN KEY(profile_id, issue_id) REFERENCES issues(profile_id, issue_id) ON DELETE CASCADE,
        FOREIGN KEY(profile_id, session_id, run_id) REFERENCES runs(profile_id, session_id, run_id) ON DELETE CASCADE)""",
    """CREATE TABLE learning_events (
        event_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL, session_id TEXT NOT NULL, run_id TEXT NOT NULL,
        issue_id TEXT, occurrence_id TEXT, event_type TEXT NOT NULL, concept_ids TEXT NOT NULL,
        payload TEXT NOT NULL, created_at TEXT NOT NULL,
        FOREIGN KEY(profile_id, session_id, run_id) REFERENCES runs(profile_id, session_id, run_id) ON DELETE CASCADE,
        FOREIGN KEY(profile_id, issue_id) REFERENCES issues(profile_id, issue_id) ON DELETE CASCADE,
        FOREIGN KEY(profile_id, occurrence_id) REFERENCES occurrences(profile_id, occurrence_id) ON DELETE CASCADE)""",
    """CREATE TABLE concept_summary (
        profile_id TEXT NOT NULL REFERENCES learner_profile ON DELETE CASCADE,
        project_id TEXT NOT NULL, instance_id TEXT NOT NULL, concept_id TEXT NOT NULL,
        payload TEXT NOT NULL, PRIMARY KEY(profile_id, project_id, instance_id, concept_id))""",
    """CREATE TABLE cards (
        card_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL, session_id TEXT NOT NULL, run_id TEXT NOT NULL,
        state_version INTEGER NOT NULL, card_type TEXT NOT NULL, created_at TEXT NOT NULL,
        FOREIGN KEY(profile_id, session_id, run_id) REFERENCES runs(profile_id, session_id, run_id) ON DELETE CASCADE)""",
    """CREATE TABLE action_receipts (
        action_id TEXT PRIMARY KEY, card_id TEXT NOT NULL REFERENCES cards ON DELETE CASCADE,
        profile_id TEXT NOT NULL, session_id TEXT NOT NULL, run_id TEXT NOT NULL,
        payload_hash TEXT NOT NULL, receipt_ref TEXT NOT NULL, result TEXT NOT NULL, created_at TEXT NOT NULL,
        FOREIGN KEY(profile_id, session_id, run_id) REFERENCES runs(profile_id, session_id, run_id) ON DELETE CASCADE)""",
    """CREATE TABLE course_refs (
        event_id TEXT PRIMARY KEY REFERENCES learning_events ON DELETE CASCADE,
        passage_id TEXT NOT NULL, index_version TEXT NOT NULL, interaction TEXT NOT NULL)""",
    "CREATE INDEX event_history ON learning_events(profile_id, session_id, created_at, event_id)",
)

LIFECYCLE_SCHEMA = (
    """CREATE TABLE deletion_tombstones (
        batch_id TEXT PRIMARY KEY, scope_type TEXT NOT NULL, profile_id TEXT NOT NULL, session_id TEXT,
        status TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL, error_code TEXT)""",
    """CREATE TABLE event_receipts (
        event_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL, session_id TEXT NOT NULL, run_id TEXT NOT NULL,
        payload_hash TEXT NOT NULL, semantic_key TEXT UNIQUE, created_at TEXT NOT NULL,
        FOREIGN KEY(profile_id, session_id, run_id) REFERENCES runs(profile_id, session_id, run_id) ON DELETE CASCADE)""",
    """CREATE TABLE technical_logs (
        log_id TEXT PRIMARY KEY, profile_id TEXT NOT NULL, session_id TEXT NOT NULL,
        result_code TEXT NOT NULL, created_at TEXT NOT NULL,
        FOREIGN KEY(profile_id, session_id) REFERENCES sessions(profile_id, session_id) ON DELETE CASCADE)""",
)


def _execute_statements(connection: sqlite3.Connection, statements: Iterable[str]) -> None:
    # executescript commits implicitly: deliberately use execute for atomic DDL.
    for statement in statements:
        connection.execute(statement)


def _migration_lifecycle(connection):
    _execute_statements(connection, LIFECYCLE_SCHEMA)
    # v1 facts survive the upgrade, including deduplication on their next delivery.
    from dataclasses import fields
    from .models import LearningEvent
    allowed = {f.name for f in fields(LearningEvent)}
    def fingerprint(value):
        return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                        separators=(",", ":")).encode("utf-8")).hexdigest()
    seen_semantics = set()
    for row in connection.execute("""SELECT e.*,s.project_id,s.instance_id FROM learning_events e
        JOIN sessions s USING(session_id) ORDER BY e.created_at,e.event_id""").fetchall():
        payload = json.loads(row["payload"])
        raw = {k: v for k, v in payload.items() if k in allowed}
        raw["created_at"] = row["created_at"]
        scope = {k: row[k] for k in ("profile_id", "project_id", "instance_id", "session_id")}
        semantic = None
        if row["event_type"] in ("issue_observed", "retry_passed", "retry_failed") and payload.get("job"):
            semantic = fingerprint([row["profile_id"], row["project_id"], row["instance_id"], row["issue_id"],
                                    "observation" if row["event_type"] == "issue_observed" else "outcome",
                                    payload["job"]["job_id"]])
            if semantic in seen_semantics:
                semantic = None
            else:
                seen_semantics.add(semantic)
        connection.execute("INSERT INTO event_receipts VALUES (?,?,?,?,?,?,?)",
            (row["event_id"], row["profile_id"], row["session_id"], row["run_id"],
             "legacy:" + fingerprint({"scope": scope, "event": raw}), semantic, row["created_at"]))


MIGRATIONS = (
    lambda connection: _execute_statements(connection, CORE_SCHEMA),
    _migration_lifecycle,
)
SCHEMA_VERSION = len(MIGRATIONS)


class Database:
    """Single-process writer; each unit of work owns BEGIN IMMEDIATE."""

    def __init__(self, path: Optional[Path] = None, migrations: Iterable[Callable] = MIGRATIONS):
        self.path = Path(path) if path is not None else default_database_path()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.connection = sqlite3.connect(self.path, isolation_level=None, timeout=5)
        self.connection.row_factory = sqlite3.Row
        try:
            self.connection.execute("PRAGMA foreign_keys=ON")
            if self.connection.execute("PRAGMA journal_mode=WAL").fetchone()[0] != "wal":
                raise MigrationError("WAL unavailable; use a local file database")
            self.connection.execute("PRAGMA synchronous=FULL")
            self.migrate(tuple(migrations))
        except Exception:
            self.connection.close()
            raise

    @property
    def schema_version(self) -> int:
        return self.connection.execute("PRAGMA user_version").fetchone()[0]

    @contextmanager
    def transaction(self):
        if self.connection.in_transaction:
            raise RuntimeError("nested transaction not supported")
        self.connection.execute("BEGIN IMMEDIATE")
        try:
            yield self.connection
            self.connection.execute("COMMIT")
        except BaseException:
            self.connection.execute("ROLLBACK")
            raise

    def migrate(self, migrations=MIGRATIONS) -> None:
        try:
            with self.transaction():
                current = self.schema_version
                if current > len(migrations):
                    raise MigrationError("database schema newer than this application")
                for index in range(current, len(migrations)):
                    migrations[index](self.connection)
                    self.connection.execute("PRAGMA user_version=" + str(index + 1))
        except Exception as exc:
            raise MigrationError("migration rolled back; original schema retained") from exc

    def checkpoint(self) -> None:
        row = self.connection.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
        if row[0] != 0:
            raise RuntimeError("checkpoint busy")

    def close(self) -> None:
        self.connection.close()
