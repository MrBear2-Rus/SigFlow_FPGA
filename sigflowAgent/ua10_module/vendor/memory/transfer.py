"""Validate an archival export before any write. Never restore permissions or caches."""
from __future__ import annotations

import json
from dataclasses import fields

from .models import (CourseInteraction, JobEvidence, LearningEvent, MemoryErrorBase, RUN_STATES,
                     Scope, ScopeError, U2TeachingFacts, identifier, timestamp)


def _only(row, keys):
    if not isinstance(row, dict) or set(row) - set(keys):
        raise MemoryErrorBase("unknown export fields (bodies/credentials are not supported)")
    return row


def parse_learning_export(text, expected_profile_id, concepts):
    identifier(expected_profile_id)
    try:
        data = json.loads(text)
        _only(data, ("export_version", "schema_version", "profile", "scope", "sessions", "runs", "issues",
                     "occurrences", "learning_events", "concept_summary", "cards", "action_receipts", "course_refs"))
        if data["export_version"] != 1 or data["scope"]["profile_id"] != expected_profile_id:
            raise ScopeError("export version/profile mismatch")
        profile = data["profile"]
        _only(profile, ("profile_id", "display_name", "preferences", "preferences_explicit", "created_at", "updated_at"))
        if profile["profile_id"] != expected_profile_id:
            raise ScopeError("export contains another profile")
        scopes, runs, issues, occurrences = {}, [], [], []
        for row in data["sessions"]:
            _only(row, ("profile_id", "session_id", "project_id", "instance_id", "status", "created_at", "updated_at"))
            scope = Scope(*(identifier(row[k]) for k in ("profile_id", "project_id", "instance_id", "session_id")))
            if scope.profile_id != expected_profile_id or scope.session_id in scopes:
                raise ScopeError("foreign/duplicate session in export")
            scopes[scope.session_id] = scope
        for row in data["runs"]:
            _only(row, ("run_id", "profile_id", "session_id", "revision", "snapshot_id", "policy_version", "workflow_version",
                        "model_version", "execution_mode", "status", "state_version", "hint_level", "created_at", "updated_at"))
            if row["profile_id"] != expected_profile_id or row["session_id"] not in scopes or row["status"] not in RUN_STATES:
                raise ScopeError("foreign run in export")
            for key in ("run_id", "revision", "snapshot_id", "policy_version", "workflow_version", "model_version"):
                identifier(row[key])
            runs.append({**row, "created_at": timestamp(row["created_at"])})
        by_run = {r["run_id"]: r for r in runs}
        for row in data["issues"]:
            _only(row, ("issue_id", "profile_id", "project_id", "instance_id", "diagnostic_type", "logical_object", "goal_id",
                        "identity_key", "concept_ids", "status", "created_at", "updated_at"))
            if row["profile_id"] != expected_profile_id or set(row["concept_ids"]) - concepts:
                raise ScopeError("foreign issue/concepts in export")
            for key in ("issue_id", "project_id", "instance_id", "diagnostic_type", "goal_id", "identity_key"):
                identifier(row[key])
            if row["logical_object"] is not None:
                identifier(row["logical_object"])
            issues.append({**row, "status": "open", "created_at": timestamp(row["created_at"]),
                           "updated_at": timestamp(row["updated_at"])})
        by_issue = {i["issue_id"]: i for i in issues}
        for row in data["occurrences"]:
            _only(row, ("occurrence_id", "profile_id", "session_id", "run_id", "issue_id", "revision", "job_id",
                        "status", "created_at", "updated_at"))
            scope = scopes[row["session_id"]]
            issue = by_issue[row["issue_id"]]
            if (row["profile_id"] != expected_profile_id or by_run[row["run_id"]]["session_id"] != scope.session_id
                or issue["project_id"] != scope.project_id or issue["instance_id"] != scope.instance_id):
                raise ScopeError("foreign occurrence in export")
            for key in ("occurrence_id", "revision", "job_id"):
                identifier(row[key])
            occurrences.append({**row, "status": "open", "created_at": timestamp(row["created_at"]),
                                "updated_at": timestamp(row["updated_at"])})
        events = []
        allowed_payload = {f.name for f in fields(LearningEvent)}
        for row in data["learning_events"]:
            _only(row, ("event_id", "profile_id", "session_id", "run_id", "issue_id", "occurrence_id", "event_type",
                        "concept_ids", "payload", "created_at"))
            if row["profile_id"] != expected_profile_id:
                raise ScopeError("foreign event in export")
            scope = scopes[row["session_id"]]
            if by_run[row["run_id"]]["session_id"] != scope.session_id:
                raise ScopeError("event/run scope mismatch")
            payload = _only(row["payload"], allowed_payload | {"evidence_kind", "new_job"})
            args = {k: v for k, v in payload.items() if k in allowed_payload}
            if any(args.get(k) != row[k] for k in ("event_id", "run_id", "issue_id", "occurrence_id", "event_type", "concept_ids")):
                raise ScopeError("export event envelope/payload mismatch")
            if args.get("job"):
                _only(args["job"], {f.name for f in fields(JobEvidence)})
                args["job"] = JobEvidence(**{**args["job"], "kind": "mock"})
            if args.get("course"):
                _only(args["course"], {f.name for f in fields(CourseInteraction)})
                args["course"] = CourseInteraction(**args["course"])
            if args.get("u2"):
                _only(args["u2"], {f.name for f in fields(U2TeachingFacts)})
                args["u2"]["course_refs"] = tuple(tuple(item) for item in args["u2"].get("course_refs", ()))
                args["u2"] = U2TeachingFacts(**args["u2"])
            args["concept_ids"] = tuple(args["concept_ids"])
            args["evidence_refs"] = tuple(args["evidence_refs"])
            args["created_at"] = timestamp(row["created_at"])
            events.append((scope, LearningEvent(**args)))
        events.sort(key=lambda item: (item[1].created_at, item[1].event_id))
        return {"display_name": profile["display_name"], "scopes": scopes, "runs": runs, "issues": issues,
                "occurrences": occurrences, "events": events}
    except (KeyError, TypeError, json.JSONDecodeError) as exc:
        raise MemoryErrorBase("invalid learning export") from exc
