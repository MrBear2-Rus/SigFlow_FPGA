from __future__ import annotations

import base64
import json
import re
import uuid
from dataclasses import asdict
from datetime import datetime, timedelta
from pathlib import Path
from typing import Optional

import yaml

from .database import Database
from .models import (Action, ConflictError, CourseInteraction, DeletedScopeError, EVENT_TYPES,
                     HintPreferences, JobEvidence, LearningEvent, MemoryErrorBase, PersonalizationPolicy,
                     RetentionPolicy, RUN_STATES, RunController, Scope, ScopeError, TERMINAL_STATES,
                     U2TeachingFacts,
                     identifier, timestamp, utc_now)
from .repository import LearningRepository, TombstoneLedger, digest, encode
from .summary import derive_summaries


class LearningService:
    """Trusted runtime API, never registered as an arbitrary LLM database-write tool.

    Caller authenticates the learner and verifies Gateway evidence / UI receipts.
    Scope provides storage isolation, not authentication of the human caller.
    No mutable current-profile state or in-memory learner cache is maintained.
    """

    def __init__(self, path: Optional[Path] = None, *, tombstone_path: Optional[Path] = None,
                 concepts_path: Optional[Path] = None, run_controller: Optional[RunController] = None):
        self.db = Database(path)
        try:
            self.ledger = TombstoneLedger(tombstone_path or self.db.path.with_suffix(".deletions.sqlite"))
            self.repo = LearningRepository(self.db, self.ledger)
            self.run_controller = run_controller
            bundled = Path(__file__).resolve().parent / 'data' / 'concepts.yaml'
            concepts_path = concepts_path or (bundled if bundled.exists() else
                Path(__file__).resolve().parents[1] / "resources/courses/concepts.yaml")
            concept_rows = yaml.safe_load(Path(concepts_path).read_text(encoding="utf-8"))["concepts"]
            self.concepts = {r["concept_id"] for r in concept_rows}
            self.concept_labels = {}
            for row in concept_rows:
                aliases = row.get("aliases", ())
                self.concept_labels[row["concept_id"]] = next(
                    (alias for alias in aliases if isinstance(alias, str)
                     and any("\u3400" <= char <= "\u9fff" for char in alias)),
                    row["concept_id"])
            # Reconcile even completed intents when a stale main DB backup was restored.
            self.reconcile_deletions()
        except Exception:
            if hasattr(self, "ledger"):
                self.ledger.db.close()
            self.db.close()
            raise

    @property
    def schema_version(self):
        return self.db.schema_version

    def close(self):
        self.ledger.db.close()
        self.db.close()

    @staticmethod
    def _validate_display_name(display_name):
        if not isinstance(display_name, str) or not 1 <= len(display_name.strip()) <= 64:
            raise MemoryErrorBase("display name must be 1..64 characters")
        if re.search(r"(?i)(sk-|api[_ -]?key|bearer|token|password|[\x00-\x1f])", display_name):
            raise MemoryErrorBase("display name cannot contain credentials/control lines")

    def create_profile(self, profile_id=None, display_name="匿名学习者"):
        profile_id = identifier(profile_id or str(uuid.uuid4()))
        self._validate_display_name(display_name)
        with self.db.transaction():
            return self.repo.create_profile(profile_id, display_name, asdict(HintPreferences()))

    def set_preferences(self, profile_id, preferences: HintPreferences, *, student_explicit=False):
        if student_explicit is not True:
            raise MemoryErrorBase("preferences require an explicit student settings action")
        if (type(preferences.start_level) is not int or preferences.start_level not in (1, 2)
            or preferences.explanation_length not in ("short", "standard", "detailed")
            or preferences.wording not in ("plain", "technical")
            or type(preferences.prerequisite_reminders) is not bool
            or type(preferences.practice_suggestions) is not bool):
            raise MemoryErrorBase("unsupported personalization preference")
        with self.db.transaction():
            self.repo.set_preferences(profile_id, asdict(preferences))

    def personalization(self, profile_id, policy: PersonalizationPolicy):
        profile = self.repo.check_profile(profile_id)
        prefs = profile["preferences"]
        if (type(policy.max_start_level) is not int or policy.max_start_level not in (1, 2)
            or type(policy.allow_l2_start) is not bool):
            raise MemoryErrorBase("invalid policy start limit")
        use_l2 = bool(profile["preferences_explicit"] and prefs["start_level"] == 2
                      and policy.allow_l2_start and policy.max_start_level >= 2)
        return {"start_level": 2 if use_l2 else 1, "can_return_to_l1": True,
                "reason": "explicit_student_preference_and_policy" if use_l2 else "default_or_policy_requires_l1",
                "explanation_length": prefs["explanation_length"], "wording": prefs["wording"],
                "prerequisite_reminders": prefs["prerequisite_reminders"],
                "practice_suggestions": prefs["practice_suggestions"]}

    def create_session(self, profile_id, project_id, instance_id, session_id=None):
        scope = Scope(*(identifier(v) for v in (profile_id, project_id, instance_id, session_id or str(uuid.uuid4()))))
        with self.db.transaction():
            self.repo.create_session(scope, "open")
        return scope

    def bind_session(self, profile_id, project_id, instance_id, session_id):
        scope = Scope(profile_id, project_id, instance_id, session_id)
        self.repo.check_scope(scope)
        return scope

    def create_run(self, scope, revision, snapshot_id, *, run_id=None, policy_version="edu-default-v1",
                   workflow_version="unconnected-v1", model_version="none", execution_mode="mock",
                   status="Created", created_at=None):
        run_id = identifier(run_id or str(uuid.uuid4()))
        for value in (revision, snapshot_id, policy_version, workflow_version, model_version):
            identifier(value)
        if status not in RUN_STATES or execution_mode not in ("real", "mock"):
            raise MemoryErrorBase("invalid run status/mode")
        with self.db.transaction():
            self.repo.create_run(scope, run_id, revision, snapshot_id, policy_version, workflow_version,
                                 model_version, execution_mode, status, timestamp(created_at or utc_now()))
        return run_id

    def start_u2_run(self, scope, session, request, *, execution_mode="real"):
        """Persist a current U2 GatewayTeachingSession before the host advances it.

        The caller authenticates the learner and constructs `scope`; SessionBinding
        is checked for consistency but is not treated as an authentication token.
        """
        from .u2_adapter import start_u2_run
        return start_u2_run(self, scope, session, request, execution_mode=execution_mode)

    def record_u2_snapshot(self, scope, session, *, evidence=None, previous_state=None,
                           action_id=None, action_kind=None, action_result=None, checkpoint_ref=None,
                           diagnostic_type=None, trusted_concept_id=None):
        """Save facts from an already-completed U2 transition; never advances U2."""
        from .u2_adapter import record_u2_snapshot
        return record_u2_snapshot(self, scope, session, evidence=evidence,
                                  previous_state=previous_state, action_id=action_id,
                                  action_kind=action_kind, action_result=action_result,
                                  checkpoint_ref=checkpoint_ref, diagnostic_type=diagnostic_type,
                                  trusted_concept_id=trusted_concept_id)

    def set_run_status(self, scope, run_id, status):
        if status not in RUN_STATES:
            raise MemoryErrorBase("invalid run status")
        with self.db.transaction():
            run = self.repo.check_run(scope, run_id)
            if run["status"] in TERMINAL_STATES and status != run["status"]:
                raise ConflictError("terminal run cannot be reopened")
            return self.repo.update_run(scope, run_id, status=status)

    def _validate_job(self, scope, run, job):
        for field in ("job_id", "project_id", "instance_id", "revision", "snapshot_id", "report_ref", "input_fingerprint"):
            identifier(getattr(job, field))
        if job.scope_ref:
            identifier(job.scope_ref)
        if (job.project_id != scope.project_id or job.instance_id != scope.instance_id
            or job.revision != run["revision"] or job.snapshot_id != run["snapshot_id"]):
            raise ScopeError("Job evidence binding differs from run/project/snapshot")
        if (job.kind not in ("real", "mock") or job.kind != run["execution_mode"]
            or job.validation not in ("test_expectation", "diagnostic_absence", "process_exit", "synth_exit")
            or job.outcome not in ("passed", "failed") or type(job.complete) is not bool):
            raise MemoryErrorBase("invalid Job evidence metadata")

    def observe_issue(self, scope, event_id, run_id, diagnostic_type, goal_id, concept_ids, job: JobEvidence,
                      *, logical_object=None, observation_key="diagnostic-0", evidence_refs=(), created_at=None):
        for value in (diagnostic_type, goal_id, observation_key):
            identifier(value)
        if logical_object is not None:
            identifier(logical_object)
        run = self.repo.check_run(scope, run_id)
        self._validate_job(scope, run, job)
        concepts = tuple(sorted(concept_ids))
        predicted_issue, predicted_occurrence = self.repo.observation_identity(
            scope, diagnostic_type, logical_object, goal_id, job.revision, job.job_id, observation_key)
        proposed = LearningEvent(event_id, run_id, "issue_observed", concepts, predicted_issue, predicted_occurrence,
                                 evidence_refs=tuple(evidence_refs), job=job, created_at=created_at)
        # Validate before writing; observation creation and event insertion share one transaction.
        with self.db.transaction():
            receipt = self.repo.receipt(event_id)
            if receipt:
                replay = self._check_event_replay(scope, proposed, receipt)
                return {**replay, "issue_id": predicted_issue, "occurrence_id": predicted_occurrence}
            semantic = self.repo.semantic_receipt(self._semantic_key(scope, proposed))
            if semantic:
                return {"event_id": semantic["event_id"], "recorded": False, "reason": "job_report_replay",
                        "issue_id": predicted_issue, "occurrence_id": predicted_occurrence}
            issue_id, occurrence_id = self.repo.issue_and_occurrence(
                scope, run_id, diagnostic_type, logical_object, goal_id, concepts,
                job.job_id, job.revision, observation_key)
            event = LearningEvent(event_id, run_id, "issue_observed", concepts, issue_id, occurrence_id,
                                  evidence_refs=tuple(evidence_refs), job=job, created_at=created_at)
            result = self._record(scope, event)
        return {**result, "issue_id": issue_id, "occurrence_id": occurrence_id}

    def _validated_payload(self, scope, event):
        run = self.repo.check_run(scope, event.run_id)
        identifier(event.event_id)
        u2_types = {"u2_run_started", "u2_diagnostic_observed", "u2_card_generated",
                    "u2_hint_accepted", "u2_action_accepted", "u2_checkpoint_saved", "u2_run_ended",
                    "u2_observation_enriched"}
        if event.event_type not in EVENT_TYPES or (not event.concept_ids and event.event_type not in u2_types) or len(event.concept_ids) > 16:
            raise MemoryErrorBase("invalid event type/concepts")
        if len(set(event.concept_ids)) != len(event.concept_ids) or set(event.concept_ids) - self.concepts:
            raise MemoryErrorBase("duplicate or unknown UA-09 concept IDs")
        if type(event.hint_level) is not int or event.hint_level not in range(5):
            raise MemoryErrorBase("invalid hint level")
        for value in (event.reference_viewed, event.student_modified):
            if type(value) is not bool:
                raise MemoryErrorBase("event flags must be boolean")
        if len(event.evidence_refs) > 16:
            raise MemoryErrorBase("too many evidence references")
        for value in (*event.evidence_refs, event.explanation_ref, event.followup_ref):
            if value is not None:
                identifier(value)
        occurrence = None
        if event.issue_id:
            issue = self.repo.check_issue(scope, event.issue_id)
            if set(event.concept_ids) - set(issue["concept_ids"]):
                raise ScopeError("event concepts outside issue mapping")
            if not event.occurrence_id:
                raise ScopeError("issue-specific events require a concrete occurrence")
        if event.occurrence_id:
            if not event.issue_id:
                raise ScopeError("occurrence requires issue")
            occurrence = self.repo.check_occurrence(scope, event.occurrence_id, event.issue_id)
        if event.event_type in ("issue_observed", "retry_passed", "retry_failed") and (not occurrence or not event.job):
            raise MemoryErrorBase("observation/retry requires an occurrence and Job evidence")
        if event.event_type == "hint_presented" and not event.hint_level:
            raise MemoryErrorBase("hint event requires a hint level")
        if event.event_type == "student_modified" and not event.student_modified:
            raise MemoryErrorBase("student edit event requires explicit edit evidence")
        if event.event_type == "student_explanation" and not (event.explanation_ref or event.followup_ref):
            raise MemoryErrorBase("student explanation requires a checked reference")
        if event.event_type == "reference_viewed" and not event.reference_viewed:
            raise MemoryErrorBase("reference event requires explicit view flag")
        if event.hint_level == 4 and not event.reference_viewed:
            raise MemoryErrorBase("L4 must retain reference_viewed")
        self._validate_u2_fact(event)
        if event.job:
            self._validate_job(scope, run, event.job)
            expected = {"retry_passed": "passed", "retry_failed": "failed"}.get(event.event_type)
            if expected and event.job.outcome != expected:
                raise MemoryErrorBase("event/Job outcome mismatch")
        course_types = {"course_shown": "shown", "course_clicked": "clicked", "course_used": "used"}
        if event.course:
            identifier(event.course.passage_id)
            identifier(event.course.index_version)
            if course_types.get(event.event_type) != event.course.interaction:
                raise MemoryErrorBase("course interaction/event mismatch")
        elif event.event_type in course_types:
            raise MemoryErrorBase("course event requires passage/index reference")
        payload = event.payload()
        payload["concept_ids"] = sorted(payload["concept_ids"])
        payload["evidence_kind"] = run["execution_mode"]
        payload["new_job"] = bool(occurrence and event.job and event.job.job_id != occurrence["job_id"]
                                  and event.job.revision != occurrence["revision"])
        return payload

    @staticmethod
    def _validate_u2_fact(event):
        u2_types = {"u2_run_started", "u2_diagnostic_observed", "u2_card_generated",
                    "u2_hint_accepted", "u2_action_accepted", "u2_checkpoint_saved", "u2_run_ended",
                    "u2_observation_enriched"}
        if event.event_type not in u2_types:
            if event.u2 is not None:
                raise MemoryErrorBase("U2 facts are only valid on U2 teaching events")
            return
        fact = event.u2
        if not isinstance(fact, U2TeachingFacts):
            raise MemoryErrorBase("U2 teaching event requires structured U2 facts")
        for name in ("external_issue_id", "diagnostic_id", "job_id", "snapshot_id", "evidence_id",
                     "evidence_origin", "evidence_integrity", "concept_id", "card_id", "card_producer",
                     "action_kind", "status", "stage", "checkpoint_ref", "diagnostic_type"):
            value = getattr(fact, name)
            if value is not None:
                identifier(value)
        for name in ("card_hint", "card_question"):
            value = getattr(fact, name)
            if value is not None and (type(value) is not str or not value.strip() or len(value) > 1200):
                raise MemoryErrorBase("invalid U2 card text")
        for name in ("evidence_sha256", "card_sha256", "action_sha256"):
            value = getattr(fact, name)
            if value is not None and re.fullmatch(r"[a-f0-9]{64}", value) is None:
                raise MemoryErrorBase("invalid U2 fact hash")
        for name in ("card_level", "previous_version", "current_version", "previous_level", "current_level"):
            value = getattr(fact, name)
            if value is not None and (type(value) is not int or value < 0):
                raise MemoryErrorBase("invalid U2 state value")
        if not isinstance(fact.course_refs, tuple) or len(fact.course_refs) > 8:
            raise MemoryErrorBase("invalid U2 course references")
        for item in fact.course_refs:
            if not isinstance(item, tuple) or len(item) != 2:
                raise MemoryErrorBase("invalid U2 course reference")
            identifier(item[0]); identifier(item[1])
        if event.event_type == "u2_run_started" and not all((fact.external_issue_id, fact.diagnostic_id,
                                                               fact.job_id, fact.snapshot_id,
                                                               fact.status, fact.stage)):
            raise MemoryErrorBase("U2 run start requires binding and initial state")
        if event.event_type in ("u2_diagnostic_observed", "u2_observation_enriched"):
            if not all((fact.external_issue_id, fact.diagnostic_id, fact.job_id, fact.snapshot_id,
                        fact.evidence_id, fact.evidence_sha256, fact.evidence_origin)):
                raise MemoryErrorBase("U2 observation lacks fixed-report binding")
            if fact.evidence_integrity != "u2_gate_passed" or not event.issue_id or not event.occurrence_id:
                raise MemoryErrorBase("U2 observation lacks a verified occurrence")
        if event.event_type == "u2_card_generated":
            if not fact.card_id or fact.card_level not in (1, 2, 3) or fact.card_producer not in ("rule", "model"):
                raise MemoryErrorBase("invalid generated U2 card fact")
        if event.event_type in ("u2_hint_accepted", "u2_action_accepted"):
            if (not fact.action_kind or not fact.action_sha256 or fact.previous_version is None
                    or fact.current_version is None or fact.current_version <= fact.previous_version):
                raise MemoryErrorBase("invalid accepted U2 action fact")
            if event.event_type == "u2_hint_accepted" and (
                    fact.action_kind != "hint_next" or fact.previous_level is None
                    or fact.current_level != fact.previous_level + 1 or fact.current_level > 3):
                raise MemoryErrorBase("invalid accepted U2 hint transition")
        if event.event_type == "u2_checkpoint_saved" and not fact.checkpoint_ref:
            raise MemoryErrorBase("U2 checkpoint reference is required")
        if event.event_type == "u2_run_ended" and fact.status not in (
                "Completed", "Cancelled", "Failed", "BudgetExceeded", "RecoveryRequired"):
            raise MemoryErrorBase("invalid terminal U2 status")

    def _check_event_replay(self, scope, event, receipt):
        self.repo.check_run(scope, event.run_id)
        raw = event.payload()
        if receipt["payload_hash"].startswith("legacy:"):
            raw["created_at"] = timestamp(event.created_at) if event.created_at else receipt["created_at"]
            payload_hash = "legacy:" + digest({"scope": asdict(scope), "event": raw})
        else:
            payload_hash = digest({"scope": asdict(scope), "event": raw})
        if (receipt["profile_id"] != scope.profile_id or receipt["session_id"] != scope.session_id
            or receipt["payload_hash"] != payload_hash):
            raise ConflictError("event_id reused with different scope/payload")
        return {"event_id": event.event_id, "recorded": False, "reason": "event_replay"}

    def _semantic_key(self, scope, event):
        if event.event_type == "u2_diagnostic_observed" and event.u2:
            return digest([scope.profile_id, scope.project_id, scope.instance_id, event.issue_id,
                           event.occurrence_id, event.u2.job_id, "u2_fixed_report_observation"])
        if event.event_type not in ("issue_observed", "retry_passed", "retry_failed"):
            return None
        return digest([scope.profile_id, scope.project_id, scope.instance_id, event.issue_id,
                       "observation" if event.event_type == "issue_observed" else "outcome", event.job.job_id])

    def _record(self, scope, event, run_status=None, *, imported=False, rebuild=True,
                allow_terminal_enrichment=False):
        receipt = self.repo.receipt(event.event_id)
        if receipt:
            return self._check_event_replay(scope, event, receipt)
        payload = self._validated_payload(scope, event)
        if imported:
            payload["evidence_kind"] = "imported_unverified"
        payload_hash = digest({"scope": asdict(scope), "event": event.payload()})
        run = self.repo.check_run(scope, event.run_id)
        if run["status"] in TERMINAL_STATES and not (
                allow_terminal_enrichment and event.event_type == "u2_observation_enriched"
                and run_status is None):
            raise ConflictError("cannot append new facts to terminal run")
        if run_status and run_status not in RUN_STATES:
            raise MemoryErrorBase("invalid run state")
        semantic_key = self._semantic_key(scope, event)
        old = self.repo.semantic_receipt(semantic_key)
        if old:
            if event.event_type == "u2_diagnostic_observed":
                prior = next((row for row in self.repo.events(
                    scope.profile_id, project_id=scope.project_id, instance_id=scope.instance_id,
                    session_id=scope.session_id, issue_id=event.issue_id)
                    if row["event_id"] == old["event_id"]), None)
                if prior is None or any(prior["payload"].get(key) != event.payload().get(key)
                                        for key in ("event_type", "issue_id", "occurrence_id", "concept_ids",
                                                    "evidence_refs", "u2")):
                    raise ConflictError("U2 replay changed the fixed report observation")
                return {"event_id": old["event_id"], "recorded": False, "reason": "u2_occurrence_replay"}
            # Another viewing of the same report is not another fact or practice.
            return {"event_id": old["event_id"], "recorded": False, "reason": "job_report_replay"}
        time = timestamp(event.created_at or utc_now())
        payload["created_at"] = time
        self.repo.insert_event(scope, payload, payload_hash, semantic_key, time)
        self.repo.update_run(scope, event.run_id, status=run_status)
        if rebuild:
            self._rebuild(scope.profile_id)
        return {"event_id": event.event_id, "recorded": True, "reason": "recorded"}

    def record_learning_event(self, scope, event: LearningEvent, *, run_status=None):
        with self.db.transaction():
            return self._record(scope, event, run_status)

    def _rebuild(self, profile_id):
        summaries = derive_summaries(self.repo.events(profile_id), self.repo.event_scopes(profile_id))
        self.repo.replace_summaries(profile_id, summaries)
        return summaries

    def rebuild_summaries(self, profile_id):
        with self.db.transaction():
            self.repo.check_profile(profile_id)
            self._rebuild(profile_id)

    def get_learning_summary(self, scope, *, limit=12):
        self.repo.check_scope(scope)
        if type(limit) is not int or not 1 <= limit <= 30:
            raise MemoryErrorBase("summary limit must be 1..30")
        summaries = self.repo.summaries(scope.profile_id, scope.project_id, scope.instance_id)
        summaries.sort(key=lambda s: (s["last_seen"] or "", s["concept_id"]), reverse=True)
        # Keep the model DTO small: no full events, source text, identifiers of other learners.
        return [{k: s[k] for k in ("concept_id", "status", "practice_count", "assistance_level", "last_seen",
                                   "recent_evidence", "reason", "observation_scope")} for s in summaries[:limit]]

    def get_teaching_context(self, scope, concepts, *, max_chars=600):
        """Return concise, deterministic history for U2; no model call or raw bodies."""
        from .teaching_context import get_teaching_context
        return get_teaching_context(self, scope, concepts, max_chars=max_chars)

    def get_concept_summary(self, scope, concept_id):
        self.repo.check_scope(scope)
        return next((s for s in self.repo.summaries(scope.profile_id, scope.project_id, scope.instance_id)
                     if s["concept_id"] == concept_id), None)

    def get_history(self, scope, *, limit=20, cursor=None, issue_id=None, occurrence_id=None, concept_id=None):
        self.repo.check_scope(scope)
        if type(limit) is not int or not 1 <= limit <= 100:
            raise MemoryErrorBase("history limit must be 1..100")
        if issue_id:
            self.repo.check_issue(scope, issue_id)
        if occurrence_id:
            self.repo.check_occurrence(scope, occurrence_id, issue_id)
        binding = digest([asdict(scope), issue_id, occurrence_id, concept_id])
        after = None
        if cursor:
            try:
                decoded = json.loads(base64.urlsafe_b64decode(cursor.encode("ascii")))
                if decoded["binding"] != binding:
                    raise ScopeError("history cursor belongs to another scope/filter")
                after = (decoded["time"], decoded["event_id"])
            except (ValueError, KeyError, UnicodeError, TypeError) as exc:
                raise ScopeError("invalid history cursor") from exc
        rows = self.repo.history_page(scope, limit + 1, after=after, issue_id=issue_id,
                                      occurrence_id=occurrence_id, concept_id=concept_id)
        page = rows[:limit]
        next_cursor = None
        if len(rows) > limit:
            last = page[-1]
            next_cursor = base64.urlsafe_b64encode(encode({"binding": binding, "time": last["created_at"],
                                                         "event_id": last["event_id"]}).encode()).decode("ascii")
        return {"items": page, "next_cursor": next_cursor}

    def get_issue_history(self, scope, issue_id, **pagination):
        return self.get_history(scope, issue_id=issue_id, **pagination)

    def get_occurrence_history(self, scope, issue_id, occurrence_id, **pagination):
        return self.get_history(scope, issue_id=issue_id, occurrence_id=occurrence_id, **pagination)

    def get_recent_concept_evidence(self, scope, concept_id, *, limit=5):
        self.repo.check_scope(scope)
        if type(limit) is not int or not 1 <= limit <= 20:
            raise MemoryErrorBase("evidence limit must be 1..20")
        return list(reversed(self.repo.history_page(scope, limit, concept_id=concept_id, newest=True)))

    def create_card(self, scope, run_id, card_id=None, card_type="hint"):
        if card_type not in ("hint", "feedback"):
            raise MemoryErrorBase("UA-10 cards cannot unlock reference solutions")
        with self.db.transaction():
            return self.repo.create_card(scope, run_id, identifier(card_id or str(uuid.uuid4())), card_type)

    def record_action(self, scope, action: Action, event: Optional[LearningEvent] = None):
        for value in (action.action_id, action.card_id, action.receipt_ref):
            identifier(value)
        if action.action_type not in ("acknowledge", "more_hint", "return_l1"):
            raise MemoryErrorBase("unsupported action; L4 authorization belongs to UA-06")
        if type(action.expected_state_version) is not int or action.expected_state_version < 0:
            raise MemoryErrorBase("invalid state version")
        payload_hash = digest({"scope": asdict(scope), "action": asdict(action), "event": event.payload() if event else None})
        with self.db.transaction():
            self.repo.check_scope(scope)
            old = self.repo.action_receipt(action.action_id)
            if old:
                if old["payload_hash"] != payload_hash or old["profile_id"] != scope.profile_id:
                    raise ConflictError("action ID replay changed payload/scope")
                return old["result"]
            card = self.repo.card(scope, action.card_id)
            run = self.repo.check_run(scope, card["run_id"])
            if (action.expected_state_version != card["state_version"] or card["state_version"] != run["state_version"]
                or run["status"] in TERMINAL_STATES):
                raise ConflictError("stale card or terminal run")
            level = 1 if action.action_type == "return_l1" else run["hint_level"]
            if action.action_type == "more_hint":
                level = min(3, level + 1)
            if event:
                if event.run_id != run["run_id"]:
                    raise ScopeError("action event belongs to another run")
                if event.reference_viewed or event.hint_level == 4 or event.event_type == "reference_viewed":
                    raise MemoryErrorBase("UA-10 action cannot authorize or record an L4 unlock")
                if event.event_type == "hint_presented" and event.hint_level != level:
                    raise ConflictError("action hint event differs from resulting level")
                result = self._record(scope, event)
                if not result["recorded"]:
                    raise ConflictError("new action cannot consume an already recorded event")
            updated = self.repo.update_run(scope, run["run_id"], hint_level=level)
            result = {"action_id": action.action_id, "run_id": run["run_id"], "hint_level": level,
                      "state_version": updated["state_version"], "reference_unlocked": False}
            self.repo.save_action(scope, action, run["run_id"], payload_hash, result)
            return result

    def export_profile(self, profile_id, *, format="json", session_id=None):
        if format not in ("json", "markdown"):
            raise MemoryErrorBase("export format must be json or markdown")
        # One read/write transaction gives a consistent export against another DB connection.
        with self.db.transaction():
            profile = self.repo.check_profile(profile_id)
            if session_id:
                session = next((s for s in self.repo.export_rows(profile_id)["sessions"] if s["session_id"] == session_id), None)
                if session is None:
                    raise ScopeError("export session outside profile")
                self.repo.check_scope(Scope(profile_id, session["project_id"], session["instance_id"], session_id))
            rows = self.repo.export_rows(profile_id, session_id)
            summaries = derive_summaries(rows["learning_events"], self.repo.event_scopes(profile_id))
            data = {"export_version": 1, "schema_version": self.schema_version,
                    "profile": profile, "scope": {"profile_id": profile_id, "session_id": session_id},
                    **rows, "concept_summary": [dict(project_id=k[0], instance_id=k[1], **v)
                                                 for k, v in sorted(summaries.items())]}
        if format == "json":
            return json.dumps(data, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
        # Full safe DTOs included so Markdown preserves the same learning evidence as JSON.
        return "# 学习记录导出\n\n范围：" + profile_id + (" / " + session_id if session_id else "") + "\n\n" + \
            "记录只描述当前练习证据。Mock 不属于真实学习证据。\n\n```json\n" + \
            json.dumps(data, ensure_ascii=False, sort_keys=True, indent=2) + "\n```\n"

    def export_session(self, scope, *, format="json"):
        self.repo.check_scope(scope)
        return self.export_profile(scope.profile_id, session_id=scope.session_id, format=format)

    def import_learning_export(self, text, *, expected_profile_id):
        """Explicit local archival restore, no grants/cards and no verified learning claims.

        Restore only into an absent profile. Never merge arbitrary exports into live runs.
        Imported tool evidence is unverified until a future Gateway adapter rechecks it.
        """
        from .transfer import parse_learning_export
        plan = parse_learning_export(text, expected_profile_id, self.concepts)
        self._validate_display_name(plan["display_name"])
        with self.db.transaction():
            if self.repo.profile_exists(expected_profile_id):
                raise ConflictError("import requires an absent profile; live history cannot be overwritten")
            self.repo.create_profile(expected_profile_id, plan["display_name"], asdict(HintPreferences()))
            # Preference provenance is not re-authorized by a backup file.
            for scope in plan["scopes"].values():
                self.repo.create_session(scope, "recovered")
            for run in plan["runs"]:
                self.repo.create_run(plan["scopes"][run["session_id"]], run["run_id"], run["revision"], run["snapshot_id"],
                    run["policy_version"], run["workflow_version"], run["model_version"], "mock", "Created", run["created_at"])
            for issue in plan["issues"]:
                self.repo.restore_issue(issue)
            for occurrence in plan["occurrences"]:
                self.repo.restore_occurrence(occurrence)
            imported_count = 0
            for scope, event in plan["events"]:
                imported_count += int(self._record(scope, event, imported=True)["recorded"])
            for run in plan["runs"]:
                status = run["status"] if run["status"] in TERMINAL_STATES else "RecoveryRequired"
                self.repo.update_run(plan["scopes"][run["session_id"]], run["run_id"], status=status)
            self._rebuild(expected_profile_id)
        return {"profile_id": expected_profile_id, "events_imported": imported_count,
                "evidence_kind": "imported_unverified", "operational_receipts_restored": False}

    def _finish_deletion(self, row):
        try:
            with self.db.transaction():
                self.repo.mirror_tombstone(row)
                self._rebuild(row["profile_id"])
            active = self.repo.active_runs(row["profile_id"], row["session_id"])
            for run in active:
                if run["execution_mode"] == "real":
                    if self.run_controller is None or not self.run_controller.cancel_and_wait(run):
                        raise RuntimeError("active_run_not_quiescent")
                with self.db.transaction():
                    self.repo.cancel_run(run["run_id"])
            with self.db.transaction():
                self.repo.delete_scope(row["profile_id"], row["session_id"])
                self._rebuild(row["profile_id"])
                self.repo.mirror_tombstone({**row, "status": "pending"})
            self.db.checkpoint()
            finished = self.ledger.update(row, "completed")
        except Exception:
            # Do not persist exception text: it may contain paths, requests or secrets.
            finished = self.ledger.update(row, "failed", "cancel_delete_or_checkpoint_failed")
        with self.db.transaction():
            self.repo.mirror_tombstone(finished)
        return finished

    def delete_profile(self, profile_id):
        identifier(profile_id)
        if not self.ledger.blocks(profile_id):
            self.repo.check_profile(profile_id)
        return self._finish_deletion(self.ledger.begin(profile_id))

    def delete_session(self, scope):
        # Authorize ownership even for a retry; tombstone itself carries the original owner.
        if not self.ledger.blocks(scope.profile_id, scope.session_id):
            self.repo.check_scope(scope)
        elif not any(r["profile_id"] == scope.profile_id and r["session_id"] == scope.session_id for r in self.ledger.rows()):
            raise DeletedScopeError("profile already deleted")
        return self._finish_deletion(self.ledger.begin(scope.profile_id, scope.session_id))

    def reconcile_deletions(self):
        return [self._finish_deletion(row) for row in self.ledger.rows()]

    def get_purge_status(self, profile_id):
        return [r for r in self.ledger.rows() if r["profile_id"] == profile_id]

    def record_technical_log(self, scope, log_id, result_code, created_at=None):
        identifier(log_id)
        identifier(result_code)
        with self.db.transaction():
            self.repo.log(scope, log_id, result_code, timestamp(created_at or utc_now()))

    def purge_expired_data(self, policy: RetentionPolicy = RetentionPolicy(), *, now=None):
        if (type(policy.learning_days) is not int or policy.learning_days < 1
            or type(policy.technical_log_days) is not int or policy.technical_log_days < 1):
            raise MemoryErrorBase("retention windows must be positive integer days")
        if policy.debug_body_sampling:
            raise MemoryErrorBase("debug body storage is not implemented; sampling remains disabled")
        current = datetime.fromisoformat(timestamp(now or utc_now()))
        with self.db.transaction():
            result = self.repo.cleanup(timestamp((current - timedelta(days=policy.learning_days)).isoformat()),
                                       timestamp((current - timedelta(days=policy.technical_log_days)).isoformat()))
            for profile_id in self.repo.profiles():
                self._rebuild(profile_id)
        return result
