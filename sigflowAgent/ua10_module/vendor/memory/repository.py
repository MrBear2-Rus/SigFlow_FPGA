"""All learning SQL lives here; services only operate validated DTOs/dicts."""
from __future__ import annotations

import hashlib
import json
import uuid
from pathlib import Path

from .database import Database, LIFECYCLE_SCHEMA
from .models import ConflictError, DeletedScopeError, Scope, ScopeError, utc_now


def encode(value) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def digest(value) -> str:
    return hashlib.sha256(encode(value).encode("utf-8")).hexdigest()


def decode_row(row):
    if row is None:
        return None
    result = dict(row)
    for field in ("preferences", "concept_ids", "payload", "result"):
        if field in result:
            result[field] = json.loads(result[field])
    return result


class TombstoneLedger:
    """Keep outside learning DB backups. Write intent first, fail closed after crashes."""

    def __init__(self, path: Path):
        self.db = Database(path, migrations=(lambda c: c.execute(LIFECYCLE_SCHEMA[0]),))

    def rows(self):
        return [dict(r) for r in self.db.connection.execute(
            "SELECT * FROM deletion_tombstones ORDER BY created_at, batch_id")]

    def blocks(self, profile_id, session_id=None):
        return self.db.connection.execute(
            """SELECT 1 FROM deletion_tombstones WHERE profile_id=?
               AND (scope_type='profile' OR session_id=?) LIMIT 1""",
            (profile_id, session_id)).fetchone() is not None

    def begin(self, profile_id, session_id=None):
        scope_type = "session" if session_id else "profile"
        existing = self.db.connection.execute(
            "SELECT * FROM deletion_tombstones WHERE profile_id=? AND scope_type=? AND session_id IS ?",
            (profile_id, scope_type, session_id)).fetchone()
        if existing:
            return dict(existing)
        now = utc_now()
        row = {"batch_id": str(uuid.uuid4()), "scope_type": scope_type,
               "profile_id": profile_id, "session_id": session_id, "status": "pending",
               "created_at": now, "updated_at": now, "error_code": None}
        with self.db.transaction() as c:
            c.execute("INSERT INTO deletion_tombstones VALUES (?,?,?,?,?,?,?,?)", tuple(row.values()))
        return row

    def update(self, row, status, error_code=None):
        updated = {**row, "status": status, "updated_at": utc_now(), "error_code": error_code}
        with self.db.transaction() as c:
            c.execute("UPDATE deletion_tombstones SET status=?,updated_at=?,error_code=? WHERE batch_id=?",
                      (status, updated["updated_at"], error_code, row["batch_id"]))
        return updated


class LearningRepository:
    def __init__(self, db: Database, ledger: TombstoneLedger):
        self.db, self.ledger = db, ledger
        self.c = db.connection

    def check_profile(self, profile_id):
        if self.ledger.blocks(profile_id):
            raise DeletedScopeError("profile has a deletion tombstone")
        row = decode_row(self.c.execute("SELECT * FROM learner_profile WHERE profile_id=?", (profile_id,)).fetchone())
        if row is None:
            raise ScopeError("unknown profile")
        return row

    def check_scope(self, scope: Scope):
        self.check_profile(scope.profile_id)
        if self.ledger.blocks(scope.profile_id, scope.session_id):
            raise DeletedScopeError("session has a deletion tombstone")
        row = self.c.execute("SELECT * FROM sessions WHERE session_id=?", (scope.session_id,)).fetchone()
        if row is None or any(row[key] != getattr(scope, key) for key in
                              ("profile_id", "project_id", "instance_id", "session_id")):
            raise ScopeError("session does not belong to this learner/project/instance")
        return dict(row)

    def check_run(self, scope, run_id):
        self.check_scope(scope)
        row = decode_row(self.c.execute(
            "SELECT * FROM runs WHERE profile_id=? AND session_id=? AND run_id=?",
            (scope.profile_id, scope.session_id, run_id)).fetchone())
        if row is None:
            raise ScopeError("run outside selected scope")
        return row

    def check_issue(self, scope, issue_id):
        self.check_scope(scope)
        row = decode_row(self.c.execute(
            "SELECT * FROM issues WHERE issue_id=? AND profile_id=? AND project_id=? AND instance_id=?",
            (issue_id, scope.profile_id, scope.project_id, scope.instance_id)).fetchone())
        if row is None:
            raise ScopeError("issue outside selected scope")
        return row

    def check_occurrence(self, scope, occurrence_id, issue_id):
        self.check_issue(scope, issue_id)
        row = self.c.execute(
            "SELECT * FROM occurrences WHERE occurrence_id=? AND profile_id=? AND session_id=? AND issue_id=?",
            (occurrence_id, scope.profile_id, scope.session_id, issue_id)).fetchone()
        if row is None:
            raise ScopeError("occurrence outside selected session/issue")
        return dict(row)

    def create_profile(self, profile_id, display_name, preferences):
        if self.ledger.blocks(profile_id):
            raise DeletedScopeError("deleted profile IDs cannot be reused")
        now = utc_now()
        self.c.execute("INSERT INTO learner_profile VALUES (?,?,?,0,?,?)",
                       (profile_id, display_name, encode(preferences), now, now))
        return self.check_profile(profile_id)

    def set_preferences(self, profile_id, preferences):
        self.check_profile(profile_id)
        self.c.execute("UPDATE learner_profile SET preferences=?,preferences_explicit=1,updated_at=? WHERE profile_id=?",
                       (encode(preferences), utc_now(), profile_id))

    def profile_exists(self, profile_id):
        return self.c.execute("SELECT 1 FROM learner_profile WHERE profile_id=?", (profile_id,)).fetchone() is not None

    def restore_issue(self, row):
        self.c.execute("INSERT INTO issues VALUES (?,?,?,?,?,?,?,?,?,?,?,?)", tuple(
            encode(row[k]) if k == "concept_ids" else row[k] for k in
            ("issue_id", "profile_id", "project_id", "instance_id", "diagnostic_type", "logical_object",
             "goal_id", "identity_key", "concept_ids", "status", "created_at", "updated_at")))

    def restore_occurrence(self, row):
        self.c.execute("INSERT INTO occurrences VALUES (?,?,?,?,?,?,?,?,?,?)", tuple(row[k] for k in
            ("occurrence_id", "profile_id", "session_id", "run_id", "issue_id", "revision", "job_id", "status", "created_at", "updated_at")))

    def create_session(self, scope, status):
        self.check_profile(scope.profile_id)
        if self.ledger.blocks(scope.profile_id, scope.session_id):
            raise DeletedScopeError("deleted session IDs cannot be reused")
        now = utc_now()
        self.c.execute("INSERT INTO sessions VALUES (?,?,?,?,?,?,?)",
                       (scope.session_id, scope.profile_id, scope.project_id, scope.instance_id, status, now, now))

    def create_run(self, scope, run_id, revision, snapshot_id, policy_version, workflow_version,
                   model_version, execution_mode, status, created_at):
        self.check_scope(scope)
        self.c.execute("""INSERT INTO runs (run_id,profile_id,session_id,revision,snapshot_id,
            policy_version,workflow_version,model_version,execution_mode,status,created_at,updated_at)
            VALUES (?,?,?,?,?,?,?,?,?,?,?,?)""",
            (run_id, scope.profile_id, scope.session_id, revision, snapshot_id, policy_version,
             workflow_version, model_version, execution_mode, status, created_at, created_at))

    def update_run(self, scope, run_id, status=None, hint_level=None):
        row = self.check_run(scope, run_id)
        self.c.execute("UPDATE runs SET status=?,hint_level=?,state_version=state_version+1,updated_at=? WHERE run_id=?",
                       (status or row["status"], row["hint_level"] if hint_level is None else hint_level, utc_now(), run_id))
        self.c.execute("UPDATE sessions SET updated_at=? WHERE session_id=?", (utc_now(), scope.session_id))
        return self.check_run(scope, run_id)

    def issue_and_occurrence(self, scope, run_id, diagnostic_type, logical_object, goal_id,
                             concepts, job_id, revision, observation_key):
        self.check_run(scope, run_id)
        # Missing logical identity is deliberately report-local, never merged by type alone.
        identity = [diagnostic_type, logical_object, goal_id]
        if logical_object is None:
            identity.extend([revision, job_id, observation_key])
        key = digest(identity)
        row = self.c.execute("SELECT * FROM issues WHERE profile_id=? AND project_id=? AND instance_id=? AND identity_key=?",
                             (scope.profile_id, scope.project_id, scope.instance_id, key)).fetchone()
        now = utc_now()
        if row is None:
            issue_id = str(uuid.uuid5(uuid.NAMESPACE_URL, encode([scope.profile_id, scope.project_id, scope.instance_id, key])))
            self.c.execute("INSERT INTO issues VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                           (issue_id, scope.profile_id, scope.project_id, scope.instance_id, diagnostic_type,
                            logical_object, goal_id, key, encode(concepts), "open", now, now))
        else:
            issue_id = row["issue_id"]
            if json.loads(row["concept_ids"]) != list(concepts):
                raise ConflictError("issue concept mapping changed; create a separately versioned identity")
        row = self.c.execute("SELECT * FROM occurrences WHERE issue_id=? AND revision=? AND job_id=?",
                             (issue_id, revision, job_id)).fetchone()
        if row is None:
            occurrence_id = str(uuid.uuid5(uuid.NAMESPACE_URL, encode([issue_id, revision, job_id])))
            self.c.execute("INSERT INTO occurrences VALUES (?,?,?,?,?,?,?,?,?,?)",
                           (occurrence_id, scope.profile_id, scope.session_id, run_id, issue_id,
                            revision, job_id, "open", now, now))
        else:
            if row["session_id"] != scope.session_id:
                raise ConflictError("report already recorded in another session; use its scoped history")
            occurrence_id = row["occurrence_id"]
        return issue_id, occurrence_id

    def receipt(self, event_id):
        row = self.c.execute("SELECT * FROM event_receipts WHERE event_id=?", (event_id,)).fetchone()
        return dict(row) if row else None

    def observation_identity(self, scope, diagnostic_type, logical_object, goal_id, revision, job_id, observation_key):
        identity = [diagnostic_type, logical_object, goal_id]
        if logical_object is None:
            identity.extend([revision, job_id, observation_key])
        issue_id = str(uuid.uuid5(uuid.NAMESPACE_URL, encode(
            [scope.profile_id, scope.project_id, scope.instance_id, digest(identity)])))
        occurrence_id = str(uuid.uuid5(uuid.NAMESPACE_URL, encode([issue_id, revision, job_id])))
        return issue_id, occurrence_id

    def semantic_receipt(self, key):
        if key is None:
            return None
        row = self.c.execute("SELECT * FROM event_receipts WHERE semantic_key=?", (key,)).fetchone()
        return dict(row) if row else None

    def insert_event(self, scope, payload, payload_hash, semantic_key, created_at):
        self.c.execute("INSERT INTO event_receipts VALUES (?,?,?,?,?,?,?)",
                       (payload["event_id"], scope.profile_id, scope.session_id, payload["run_id"],
                        payload_hash, semantic_key, created_at))
        self.c.execute("INSERT INTO learning_events VALUES (?,?,?,?,?,?,?,?,?,?)",
                       (payload["event_id"], scope.profile_id, scope.session_id, payload["run_id"],
                        payload["issue_id"], payload["occurrence_id"], payload["event_type"],
                        encode(payload["concept_ids"]), encode(payload), created_at))
        if payload["course"]:
            course = payload["course"]
            self.c.execute("INSERT INTO course_refs VALUES (?,?,?,?)",
                           (payload["event_id"], course["passage_id"], course["index_version"], course["interaction"]))
        if payload["occurrence_id"] and payload["event_type"] in ("retry_passed", "retry_failed"):
            self.c.execute("UPDATE occurrences SET status=?,updated_at=? WHERE occurrence_id=?",
                           ("retested" if payload["event_type"] == "retry_passed" else "open",
                            created_at, payload["occurrence_id"]))

    def events(self, profile_id, project_id=None, instance_id=None, session_id=None,
               issue_id=None, occurrence_id=None):
        clauses, values = ["e.profile_id=?"], [profile_id]
        for column, value in (("s.project_id", project_id), ("s.instance_id", instance_id),
                              ("e.session_id", session_id), ("e.issue_id", issue_id), ("e.occurrence_id", occurrence_id)):
            if value is not None:
                clauses.append(column + "=?")
                values.append(value)
        return [decode_row(r) for r in self.c.execute(
            "SELECT e.* FROM learning_events e JOIN sessions s ON e.session_id=s.session_id WHERE "
            + " AND ".join(clauses) + " ORDER BY e.created_at,e.event_id", values)
                if not self.ledger.blocks(profile_id, r["session_id"])]

    def history_page(self, scope, limit, *, after=None, issue_id=None, occurrence_id=None, concept_id=None, newest=False):
        self.check_scope(scope)
        clauses, values = ["profile_id=?", "session_id=?"], [scope.profile_id, scope.session_id]
        for column, value in (("issue_id", issue_id), ("occurrence_id", occurrence_id)):
            if value is not None:
                clauses.append(column + "=?")
                values.append(value)
        if concept_id is not None:
            # Exact JSON string token, not a substring of another concept. No JSON1 dependency.
            clauses.append("instr(concept_ids,?)>0")
            values.append(encode(concept_id))
        if after is not None:
            clauses.append("(created_at>? OR (created_at=? AND event_id>?))")
            values.extend((after[0], after[0], after[1]))
        values.append(limit)
        order = "created_at DESC,event_id DESC" if newest else "created_at,event_id"
        return [decode_row(r) for r in self.c.execute("SELECT * FROM learning_events WHERE "
            + " AND ".join(clauses) + " ORDER BY " + order + " LIMIT ?", values)]

    def replace_summaries(self, profile_id, summaries):
        self.c.execute("DELETE FROM concept_summary WHERE profile_id=?", (profile_id,))
        for (project_id, instance_id, concept_id), summary in summaries.items():
            self.c.execute("INSERT INTO concept_summary VALUES (?,?,?,?,?)",
                           (profile_id, project_id, instance_id, concept_id, encode(summary)))

    def event_scopes(self, profile_id):
        return {r["session_id"]: (r["project_id"], r["instance_id"]) for r in self.c.execute(
            "SELECT session_id,project_id,instance_id FROM sessions WHERE profile_id=?", (profile_id,))}

    def summaries(self, profile_id, project_id, instance_id):
        self.check_profile(profile_id)
        return [json.loads(r[0]) for r in self.c.execute(
            "SELECT payload FROM concept_summary WHERE profile_id=? AND project_id=? AND instance_id=? ORDER BY concept_id",
            (profile_id, project_id, instance_id))]

    def create_card(self, scope, run_id, card_id, card_type):
        run = self.check_run(scope, run_id)
        self.c.execute("INSERT INTO cards VALUES (?,?,?,?,?,?,?)",
                       (card_id, scope.profile_id, scope.session_id, run_id, run["state_version"], card_type, utc_now()))
        return {"card_id": card_id, "run_id": run_id, "state_version": run["state_version"], "card_type": card_type}

    def action_receipt(self, action_id):
        return decode_row(self.c.execute("SELECT * FROM action_receipts WHERE action_id=?", (action_id,)).fetchone())

    def card(self, scope, card_id):
        self.check_scope(scope)
        row = self.c.execute("SELECT * FROM cards WHERE card_id=? AND profile_id=? AND session_id=?",
                             (card_id, scope.profile_id, scope.session_id)).fetchone()
        if row is None:
            raise ScopeError("unknown or expired card")
        return dict(row)

    def save_action(self, scope, action, run_id, payload_hash, result):
        self.c.execute("INSERT INTO action_receipts VALUES (?,?,?,?,?,?,?,?,?)",
                       (action.action_id, action.card_id, scope.profile_id, scope.session_id, run_id,
                        payload_hash, action.receipt_ref, encode(result), utc_now()))

    def active_runs(self, profile_id, session_id=None):
        return [dict(r) for r in self.c.execute(
            """SELECT * FROM runs WHERE profile_id=? AND (? IS NULL OR session_id=?)
               AND status NOT IN ('Completed','Cancelled','Failed') ORDER BY run_id""",
            (profile_id, session_id, session_id))]

    def cancel_run(self, run_id):
        self.c.execute("UPDATE runs SET status='Cancelled',updated_at=? WHERE run_id=?", (utc_now(), run_id))

    def mirror_tombstone(self, row):
        self.c.execute("""INSERT INTO deletion_tombstones VALUES (?,?,?,?,?,?,?,?)
            ON CONFLICT(batch_id) DO UPDATE SET status=excluded.status,updated_at=excluded.updated_at,error_code=excluded.error_code""",
            tuple(row[k] for k in ("batch_id", "scope_type", "profile_id", "session_id", "status", "created_at", "updated_at", "error_code")))

    def delete_scope(self, profile_id, session_id=None):
        if session_id is None:
            self.c.execute("DELETE FROM learner_profile WHERE profile_id=?", (profile_id,))
        else:
            self.c.execute("DELETE FROM sessions WHERE profile_id=? AND session_id=?", (profile_id, session_id))
            # Project-wide issues can be shared by sessions; only remove truly empty ones.
            self.c.execute("DELETE FROM issues WHERE profile_id=? AND NOT EXISTS (SELECT 1 FROM occurrences o WHERE o.issue_id=issues.issue_id)", (profile_id,))

    def cleanup(self, event_cutoff, log_cutoff):
        # Defer active sessions entirely: their hint/reference evidence is needed for outcomes.
        inactive = """NOT EXISTS (SELECT 1 FROM runs r WHERE r.session_id=learning_events.session_id
            AND r.status NOT IN ('Completed','Cancelled','Failed'))"""
        count = self.c.execute("DELETE FROM learning_events WHERE created_at<? AND " + inactive, (event_cutoff,)).rowcount
        logs = self.c.execute("DELETE FROM technical_logs WHERE created_at<?", (log_cutoff,)).rowcount
        cards = self.c.execute("""DELETE FROM cards WHERE created_at<? AND NOT EXISTS
            (SELECT 1 FROM runs r WHERE r.session_id=cards.session_id AND r.status NOT IN ('Completed','Cancelled','Failed'))""",
            (event_cutoff,)).rowcount
        # ID-only receipts survive retention to reject late replays; no old payload survives.
        self.c.execute("""DELETE FROM occurrences WHERE created_at<? AND NOT EXISTS
            (SELECT 1 FROM learning_events e WHERE e.occurrence_id=occurrences.occurrence_id)
            AND NOT EXISTS (SELECT 1 FROM runs r WHERE r.session_id=occurrences.session_id
                AND r.status NOT IN ('Completed','Cancelled','Failed'))""", (event_cutoff,))
        self.c.execute("DELETE FROM issues WHERE NOT EXISTS (SELECT 1 FROM occurrences o WHERE o.issue_id=issues.issue_id)")
        return {"events_deleted": count, "logs_deleted": logs, "cards_deleted": cards,
                "active_session_policy": "defer_learning_events_and_cards"}

    def profiles(self):
        return [r[0] for r in self.c.execute("SELECT profile_id FROM learner_profile ORDER BY profile_id")]

    def log(self, scope, log_id, result_code, created_at):
        self.check_scope(scope)
        self.c.execute("INSERT INTO technical_logs VALUES (?,?,?,?,?)",
                       (log_id, scope.profile_id, scope.session_id, result_code, created_at))

    def export_rows(self, profile_id, session_id=None):
        self.check_profile(profile_id)
        params = (profile_id, session_id, session_id)
        rows = {}
        for table, order in (("sessions", "session_id"), ("runs", "run_id"),
                             ("occurrences", "occurrence_id"), ("cards", "card_id")):
            rows[table] = [decode_row(r) for r in self.c.execute(
                "SELECT * FROM " + table + " WHERE profile_id=? AND (? IS NULL OR session_id=?) ORDER BY " + order, params)
                          if not self.ledger.blocks(profile_id, r["session_id"])]
        rows["issues"] = [decode_row(r) for r in self.c.execute(
            """SELECT i.* FROM issues i WHERE i.profile_id=? AND (? IS NULL OR EXISTS
                (SELECT 1 FROM occurrences o WHERE o.issue_id=i.issue_id AND o.session_id=?)) ORDER BY i.issue_id""", params)]
        rows["learning_events"] = self.events(profile_id, session_id=session_id)
        visible_issues = {r["issue_id"] for r in rows["occurrences"]}
        rows["issues"] = [r for r in rows["issues"] if r["issue_id"] in visible_issues]
        visible_events = {r["event_id"] for r in rows["learning_events"]}
        rows["course_refs"] = [dict(r) for r in self.c.execute(
            """SELECT cr.*,e.run_id,e.issue_id,e.created_at FROM course_refs cr JOIN learning_events e USING(event_id)
                WHERE e.profile_id=? AND (? IS NULL OR e.session_id=?) ORDER BY e.created_at,e.event_id""", params)
                               if r["event_id"] in visible_events]
        rows["action_receipts"] = [decode_row(r) for r in self.c.execute(
            """SELECT action_id,card_id,session_id,run_id,result,created_at FROM action_receipts
               WHERE profile_id=? AND (? IS NULL OR session_id=?) ORDER BY created_at,action_id""", params)
                                  if not self.ledger.blocks(profile_id, r["session_id"])]
        return rows
