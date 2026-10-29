"""Observation-only adapter for the U2 GatewayTeachingSession contract.

This module intentionally has no import-time dependency on the U2 checkout. It
consumes the current public object shape (SessionBinding, RunRequest, RunState,
Occurrence, Evidence and Card) and stores only allowlisted identifiers/hashes.
"""
from __future__ import annotations

from dataclasses import asdict, is_dataclass, replace
import hashlib
import json

from .models import (ConflictError, LearningEvent, MemoryErrorBase, Scope,
                     ScopeError, U2TeachingFacts, identifier, utc_now)


_U2_WORKFLOW = "u2_gateway_session_v1"


def _get(value, name, default=None):
    if isinstance(value, dict):
        return value.get(name, default)
    return getattr(value, name, default)


def _text(value, name):
    if type(value) is not str or not value.strip():
        raise MemoryErrorBase("invalid U2 " + name)
    return value


def _state_value(state, name):
    value = _get(state, name)
    if name == "status":
        value = getattr(value, "value", value)
    return value


def _scope_binding(scope, session, request):
    if not isinstance(scope, Scope):
        raise ScopeError("trusted UA-10 Scope is required")
    binding = _get(session, "binding")
    if binding is None:
        raise MemoryErrorBase("U2 session has no public binding")
    fields = ("learner_id", "project_id", "instance_id", "session_id", "revision", "snapshot_id",
              "job_id", "diagnostic_id", "issue_id", "policy")
    values = {name: _text(_get(binding, name), name) for name in fields}
    if values["policy"] != "l1-l3-l4-denied":
        raise MemoryErrorBase("unsupported current U2 policy binding")
    if (values["learner_id"], values["project_id"], values["instance_id"], values["session_id"]) != (
            scope.profile_id, scope.project_id, scope.instance_id, scope.session_id):
        raise ScopeError("U2 SessionBinding differs from trusted UA-10 scope")
    if request is not None and (
            _get(request, "project_id") != values["project_id"]
            or _get(request, "revision") != values["revision"]
            or _get(request, "issue_id") != values["issue_id"]):
        raise ScopeError("U2 RunRequest differs from SessionBinding")
    # Validate opaque references with the same conservative rules used by UA-10.
    for name in fields:
        identifier(values[name])
    return binding, values


def _run_status(status, stage):
    status = getattr(status, "value", status)
    stage = getattr(stage, "value", stage)
    if status == "Running":
        return {"E0": "Created", "E1": "CollectingEvidence", "E2": "Analyzing",
                "E3": "PreparingResponse", "E4": "PreparingResponse", "E5": "WaitingStudent",
                "E6": "WaitingJob", "E7": "Evaluating"}.get(stage, "Analyzing")
    return {"WaitingStudent": "WaitingStudent", "WaitingEvidence": "CollectingEvidence",
            "WaitingApproval": "WaitingApproval", "RecoveryRequired": "RecoveryRequired",
            "Completed": "Completed", "Cancelled": "Cancelled", "Failed": "Failed",
            "BudgetExceeded": "Failed"}.get(status, "Analyzing")


def _active_run_status(status, stage):
    mapped = _run_status(status, stage)
    # Keep the DB run appendable until its explicit U2 terminal fact is committed.
    return "PreparingResponse" if mapped in ("Completed", "Cancelled", "Failed") else mapped


def _state_facts(state):
    run_id = identifier(_text(_state_value(state, "run_id"), "run id"))
    version = _state_value(state, "version")
    level = _state_value(state, "level")
    stage = _state_value(state, "stage")
    status = _state_value(state, "status")
    if type(version) is not int or version < 0 or type(level) is not int or level not in (1, 2, 3):
        raise MemoryErrorBase("invalid U2 RunState version/level")
    if type(stage) is not str or not stage or type(status) is not str or not status:
        raise MemoryErrorBase("invalid U2 RunState stage/status")
    return run_id, version, level, stage, status


def _base_event_id(kind, value):
    return kind + ":" + hashlib.sha256(value.encode("utf-8")).hexdigest()


def _original_mapping(service, scope, event):
    """Optional mapping enrichment never changes an already-received fact."""
    if service.repo.receipt(event.event_id) is None:
        return event
    prior = next((row for row in service.repo.events(scope.profile_id, session_id=scope.session_id)
                  if row["event_id"] == event.event_id), None)
    if prior is None:
        return event  # A retained receipt still enforces its original hash.
    return replace(event, concept_ids=tuple(prior["payload"]["concept_ids"]),
                   u2=replace(event.u2, concept_id=prior["payload"]["u2"].get("concept_id")))


def start_u2_run(service, scope, session, request, *, execution_mode="real"):
    binding, values = _scope_binding(scope, session, request)
    state = _get(session, "state")
    run_id, version, level, stage, status = _state_facts(state)
    if version != 0 or stage != "E0" or status != "Running":
        raise MemoryErrorBase("record U2 run start before advancing its initial state")
    if execution_mode not in ("real", "mock"):
        raise MemoryErrorBase("invalid U2 execution mode")
    event_id = _base_event_id("u2-start", run_id)
    event = LearningEvent(
        event_id, run_id, "u2_run_started", (),
        u2=U2TeachingFacts(external_issue_id=values["issue_id"], diagnostic_id=values["diagnostic_id"],
                           job_id=values["job_id"], snapshot_id=values["snapshot_id"],
                           status=status, stage=stage, current_version=version, current_level=level),
    )
    with service.db.transaction():
        service.repo.check_scope(scope)
        existing = service.db.connection.execute(
            "SELECT profile_id,session_id,revision,snapshot_id,execution_mode FROM runs WHERE run_id=?",
            (run_id,)).fetchone()
        if existing is None:
            service.repo.create_run(scope, run_id, values["revision"], values["snapshot_id"],
                                    values["policy"], _U2_WORKFLOW, "not_reported",
                                    execution_mode, "Created", utc_now())
        else:
            run = service.repo.check_run(scope, run_id)
            if (run["revision"], run["snapshot_id"], run["execution_mode"]) != (
                    values["revision"], values["snapshot_id"], execution_mode):
                raise ConflictError("U2 run ID reused with different binding")
        result = service._record(scope, event, run_status="Created", rebuild=False)
        if result["recorded"]:
            service._rebuild(scope.profile_id)
    return {"run_id": run_id, **result}


def _observation(service, scope, session, values, state, occurrence, evidence, diagnostic_class=None,
                 trusted_concept_id=None):
    binding = _get(occurrence, "binding")
    for name in ("instance_id", "session_id", "learner_id", "project_id", "revision", "snapshot_id",
                 "job_id", "diagnostic_id", "issue_id", "policy"):
        if _get(binding, name) != values[name]:
            raise ScopeError("U2 occurrence binding changed")
    evidence_id = identifier(_text(_get(occurrence, "evidence_id"), "evidence id"))
    evidence_hash = _text(_get(occurrence, "evidence_sha256"), "evidence hash")
    origin = _text(_get(occurrence, "origin"), "evidence origin")
    if len(evidence_hash) != 64 or any(ch not in "0123456789abcdef" for ch in evidence_hash):
        raise MemoryErrorBase("invalid U2 occurrence digest")
    if origin not in ("tool", "rule", "fixture"):
        raise MemoryErrorBase("invalid U2 evidence origin")
    # The public Occurrence is only published by current U2 after valid_evidence
    # passes. Evidence is optional: current GatewayTeachingSession does not expose
    # it publicly, so a host that has the value may supply it for concept mapping
    # and an additional digest check. Never infer a concept when it is unavailable.
    concept = trusted_concept_id
    if evidence is not None:
        if not is_dataclass(evidence):
            raise MemoryErrorBase("U2 Evidence must use the current dataclass contract")
        raw = asdict(evidence)
        actual_hash = hashlib.sha256(json.dumps(raw, sort_keys=True, separators=(",", ":"),
                                                ensure_ascii=True).encode("utf-8")).hexdigest()
        if actual_hash != evidence_hash:
            raise ConflictError("U2 occurrence does not match the supplied Evidence")
        if (_get(evidence, "evidence_id") != evidence_id or _get(evidence, "job_id") != values["job_id"]
                or _get(evidence, "project_id") != values["project_id"]
                or _get(evidence, "revision") != values["revision"]
                or _get(evidence, "origin") != origin or _get(evidence, "complete") is not True):
            raise ConflictError("U2 Evidence binding or completeness differs from occurrence")
        concept = _get(evidence, "concept")
        if type(concept) is not str or not concept.strip():
            raise MemoryErrorBase("invalid U2 evidence concept")
    known_concept = concept if concept in service.concepts else None
    concepts = (known_concept,) if known_concept else ()
    diagnostic_type = "u2diag:" + values["diagnostic_id"]
    external_issue = "u2issue:" + values["issue_id"]
    goal_id = "u2_fixed_report"
    expected_issue, expected_occurrence = service.repo.observation_identity(
        scope, diagnostic_type, external_issue, goal_id, values["revision"], values["job_id"],
        "u2_gateway_report")
    old_occurrence = service.db.connection.execute(
        "SELECT session_id FROM occurrences WHERE occurrence_id=?", (expected_occurrence,)).fetchone()
    if old_occurrence is not None and old_occurrence["session_id"] != scope.session_id:
        # The fixed report was already attached to another session. Preserve the
        # current run/card facts, but never create a second observation or attach
        # a foreign-session occurrence to this run.
        return {"issue_id": expected_issue, "occurrence_id": expected_occurrence,
                "concept_ids": concepts, "u2_concept": known_concept,
                "recorded": False, "reason": "u2_occurrence_replay_other_session",
                "event_id": None, "persisted_here": False}
    # Optional host enrichment is a separate fact. Original observation and receipts
    # remain immutable, including when the host later omits optional Evidence.
    prior = next((row for row in service.repo.events(scope.profile_id, project_id=scope.project_id,
                 instance_id=scope.instance_id, session_id=scope.session_id)
                 if row["occurrence_id"] == expected_occurrence
                 and row["event_type"] == "u2_diagnostic_observed"), None)
    links = [row for row in service.repo.events(scope.profile_id, session_id=scope.session_id)
             if row["occurrence_id"] == expected_occurrence
             and row["event_type"] == "u2_observation_enriched"]
    old_fact = (links[-1] if links else prior)["payload"]["u2"] if prior else {}
    if old_fact.get("concept_id") and known_concept and old_fact["concept_id"] != known_concept:
        raise ConflictError("U2 concept mapping changed")
    if old_fact.get("diagnostic_type") and diagnostic_class and old_fact["diagnostic_type"] != diagnostic_class:
        raise ConflictError("U2 diagnostic class changed")
    known_concept = known_concept or old_fact.get("concept_id")
    diagnostic_class = diagnostic_class or old_fact.get("diagnostic_type")
    concepts = (known_concept,) if known_concept else ()
    existing_issue = service.db.connection.execute("SELECT concept_ids FROM issues WHERE issue_id=?",
                                                    (expected_issue,)).fetchone()
    if existing_issue is not None:
        # This is an issue projection update, not an event rewrite. Keep generic
        # LearningService issue-mapping conflict rules unchanged.
        issue = service.repo.check_issue(scope, expected_issue)
        if issue["concept_ids"] and concepts and tuple(issue["concept_ids"]) != concepts:
            raise ConflictError("U2 issue concept mapping changed")
        if not issue["concept_ids"] and concepts:
            service.db.connection.execute("UPDATE issues SET concept_ids=? WHERE issue_id=?",
                                          (json.dumps(list(concepts)), expected_issue))
        stored_concepts = tuple(service.repo.check_issue(scope, expected_issue)["concept_ids"])
    else:
        stored_concepts = concepts
    issue_id, occurrence_id = service.repo.issue_and_occurrence(
        scope, _state_value(state, "run_id"), diagnostic_type, external_issue, goal_id,
        stored_concepts, values["job_id"], values["revision"], "u2_gateway_report")
    fact = U2TeachingFacts(
        external_issue_id=values["issue_id"], diagnostic_id=values["diagnostic_id"],
        job_id=values["job_id"], snapshot_id=values["snapshot_id"], evidence_id=evidence_id,
        evidence_sha256=evidence_hash, evidence_origin=origin, evidence_integrity="u2_gate_passed",
        concept_id=known_concept, diagnostic_type=diagnostic_class,
    )
    event_id = _base_event_id("u2-observation", ":".join((scope.session_id, values["issue_id"],
                                                           values["diagnostic_id"], values["job_id"],
                                                           values["revision"])))
    event = LearningEvent(event_id, _state_value(state, "run_id"), "u2_diagnostic_observed", concepts,
                          issue_id, occurrence_id, evidence_refs=(evidence_id,), u2=fact)
    if prior:
        original = prior["payload"]["u2"]
        for name in ("external_issue_id", "diagnostic_id", "job_id", "snapshot_id", "evidence_id",
                     "evidence_sha256", "evidence_origin", "evidence_integrity"):
            if original.get(name) != getattr(fact, name):
                raise ConflictError("U2 replay changed fixed observation")
        changed = (known_concept, diagnostic_class) != (old_fact.get("concept_id"), old_fact.get("diagnostic_type"))
        if changed:
            enriched = replace(event, event_id=_base_event_id("u2-enrich", json.dumps(
                [occurrence_id, known_concept, diagnostic_class])), event_type="u2_observation_enriched")
            service._record(scope, enriched, rebuild=False, allow_terminal_enrichment=True)
        return {"issue_id": issue_id, "occurrence_id": occurrence_id, "concept_ids": concepts,
                "u2_concept": known_concept, "persisted_here": True,
                "event_id": enriched.event_id if changed else prior["event_id"], "recorded": changed,
                "reason": "observation_enriched" if changed else "event_replay"}
    result = service._record(scope, event, run_status=_active_run_status(
        _state_value(state, "status"), _state_value(state, "stage")), rebuild=False)
    return {"issue_id": issue_id, "occurrence_id": occurrence_id, "concept_ids": concepts,
            "u2_concept": known_concept, "persisted_here": True, **result}


def _card_trace(state, index):
    generations = _get(state, 'card_generations', ())
    if index < len(generations):
        return generations[index]
    traces = _get(state, "trace", ())
    card_traces = [item for item in traces if _get(item, "stage") in ("E4", "E7")]
    if index < len(card_traces):
        return card_traces[index]
    return None


def _record_card(service, scope, session, values, state, card, index, occurrences_by_evidence):
    run_id = _state_value(state, "run_id")
    card_id = identifier(f"u2card:{run_id}:{index + 1}")
    evidence_ids = tuple(_get(card, "evidence_ids", ()))
    if len(evidence_ids) > 8 or any(type(item) is not str for item in evidence_ids):
        raise MemoryErrorBase("invalid U2 card evidence references")
    occurrence = next((occurrences_by_evidence[ref] for ref in evidence_ids
                       if ref in occurrences_by_evidence), None)
    issue_id = occurrence["issue_id"] if occurrence else None
    occurrence_id = occurrence["occurrence_id"] if occurrence else None
    concept_ids = occurrence["concept_ids"] if occurrence else ()
    course_refs = []
    for ref in _get(card, "course_refs", ()):
        passage_id = identifier(_text(_get(ref, "passage_id"), "course passage"))
        index_version = identifier(_text(_get(ref, "index_version"), "course index version"))
        course_refs.append((passage_id, index_version))
    level = _get(card, "level")
    producer = _get(card, "producer")
    hint = _get(card, "hint")
    question = _get(card, "question")
    if type(level) is not int or level not in (1, 2, 3) or producer not in ("model", "rule"):
        raise MemoryErrorBase("invalid U2 generated card")
    if type(hint) is not str or type(question) is not str:
        raise MemoryErrorBase("invalid U2 card content")
    card_hash = hashlib.sha256(json.dumps([hint, question], ensure_ascii=False,
                                         separators=(",", ":")).encode("utf-8")).hexdigest()
    trace = _card_trace(state, index)
    state_version = _get(trace, "version") if trace is not None else _state_value(state, "version")
    card_status = _state_value(trace, "status") if trace is not None else _state_value(state, "status")
    card_stage = _get(trace, "stage") if trace is not None else _state_value(state, "stage")
    event_id = _base_event_id("u2-card", card_id)
    fact = U2TeachingFacts(external_issue_id=values["issue_id"], diagnostic_id=values["diagnostic_id"],
                           evidence_id=evidence_ids[0] if evidence_ids else None,
                           evidence_integrity="u2_gate_passed" if occurrence else None,
                           concept_id=occurrence["u2_concept"] if occurrence else None,
                           card_id=card_id, card_level=level, card_producer=producer,
                           card_sha256=card_hash, card_hint=hint, card_question=question,
                           course_refs=tuple(course_refs),
                           current_version=state_version, current_level=level,
                           status=card_status, stage=card_stage)
    event = LearningEvent(event_id, run_id, "u2_card_generated", tuple(concept_ids), issue_id,
                          occurrence_id, evidence_refs=evidence_ids, u2=fact)
    if service.repo.receipt(event_id):
        prior = next((row for row in service.repo.events(scope.profile_id, session_id=scope.session_id)
                      if row["event_id"] == event_id), None)
        if prior is None:
            raise ConflictError("U2 card receipt retained without replayable fact")
        old = prior["payload"]
        for name in ("card_id", "card_level", "card_producer", "card_sha256", "card_hint", "card_question"):
            if old["u2"].get(name) != getattr(fact, name):
                raise ConflictError("U2 card content changed")
        if list(evidence_ids) != old["evidence_refs"] or list(map(list, course_refs)) != old["u2"]["course_refs"]:
            raise ConflictError("U2 card references changed")
        # Recovery reconstructs state metadata; it is not a new generation fact.
        return {"event_id": event_id, "recorded": False, "reason": "event_replay"}
    if service.repo.receipt(event_id) is None:
        service.repo.create_card(scope, run_id, card_id, "hint")
    return service._record(scope, event, run_status=_active_run_status(
        _state_value(state, "status"), _state_value(state, "stage")), rebuild=False)


def _record_action(service, scope, values, state, previous_state, action_result, action_id, action_kind,
                   card_id, concept_ids, issue_id, occurrence_id):
    if type(action_id) is not str or not action_id or len(action_id) > 128:
        raise MemoryErrorBase("invalid U2 action reference")
    if action_kind not in ("hint_next", "finish", "cancel", "refresh_evidence", "execute_plan", "observe_verification"):
        raise MemoryErrorBase("unsupported U2 observation action")
    before_run, before_version, before_level, _, _ = _state_facts(previous_state)
    transition = action_result if action_result is not None else state
    run_id, after_version, after_level, transition_stage, transition_status = _state_facts(transition)
    current_run, current_version, current_level, stage, status = _state_facts(state)
    if (before_run != run_id or current_run != run_id or after_version <= before_version
            or current_version < after_version or current_level != after_level):
        raise ConflictError("U2 action transition does not advance the bound run")
    if action_kind == "hint_next" and (after_level != before_level + 1 or transition_stage != "E1"
                                        or transition_status != "Running"):
        raise ConflictError("U2 accepted hint result differs from its state transition")
    if action_kind == "finish" and transition_status != "Completed":
        raise ConflictError("U2 finish action has no completed result")
    if action_kind == "cancel" and transition_status != "Cancelled":
        raise ConflictError("U2 cancel action has no cancelled result")
    event_type = "u2_hint_accepted" if action_kind == "hint_next" else "u2_action_accepted"
    action_hash = hashlib.sha256(action_id.encode("utf-8")).hexdigest()
    fact = U2TeachingFacts(external_issue_id=values["issue_id"],
                           card_id=card_id, action_kind=action_kind, action_sha256=action_hash,
                           previous_version=before_version, current_version=after_version,
                           previous_level=before_level, current_level=after_level,
                           status=transition_status, stage=transition_stage)
    # U2 action IDs are session-local, whereas UA-10 receipts are global.
    event_id = _base_event_id('u2-action', run_id + ':' + action_hash)
    legacy_id = _base_event_id('u2-action', action_hash)
    legacy = service.repo.receipt(legacy_id)
    if legacy and (legacy['profile_id'], legacy['session_id'], legacy['run_id']) == (
            scope.profile_id, scope.session_id, run_id):
        event_id = legacy_id
    event = LearningEvent(event_id, run_id, event_type,
                          tuple(concept_ids), issue_id, occurrence_id, u2=fact)
    event = _original_mapping(service, scope, event)
    return service._record(scope, event, run_status=_active_run_status(status, stage), rebuild=False)


def record_u2_snapshot(service, scope, session, *, evidence=None, previous_state=None,
                       action_id=None, action_kind=None, action_result=None, checkpoint_ref=None,
                       diagnostic_type=None, trusted_concept_id=None):
    if trusted_concept_id is not None and trusted_concept_id not in service.concepts:
        raise MemoryErrorBase('unknown trusted concept')
    if diagnostic_type is not None:
        identifier(diagnostic_type)
    binding, values = _scope_binding(scope, session, request=None)
    state = _get(session, "state")
    run_id, version, level, stage, status = _state_facts(state)
    with service.db.transaction():
        service.repo.check_scope(scope)
        run = service.repo.check_run(scope, run_id)
        if (run["revision"], run["snapshot_id"], run["policy_version"]) != (
                values["revision"], values["snapshot_id"], values["policy"]):
            raise ScopeError("U2 run differs from its stored binding")
        start = next((row for row in service.repo.events(
            scope.profile_id, project_id=scope.project_id, instance_id=scope.instance_id,
            session_id=scope.session_id) if row["run_id"] == run_id
            and row["event_type"] == "u2_run_started"), None)
        start_fact = (start["payload"].get("u2") or {}) if start else {}
        if any(start_fact.get(field) != values[value_field] for field, value_field in (
                ("external_issue_id", "issue_id"), ("diagnostic_id", "diagnostic_id"),
                ("job_id", "job_id"), ("snapshot_id", "snapshot_id"))):
            raise ScopeError("U2 SessionBinding differs from the recorded run start")
        recorded = []
        occurrences = _get(session, "occurrences", ())
        if not isinstance(occurrences, (tuple, list)) or len(occurrences) > 8:
            raise MemoryErrorBase("invalid U2 occurrence collection")
        by_evidence = {}
        for item in occurrences:
            if _get(item, "origin") == "fixture" and run["execution_mode"] != "mock":
                raise ScopeError("U2 fixture evidence requires a mock run")
            observation = _observation(service, scope, session, values, state, item, evidence, diagnostic_type,
                                       trusted_concept_id)
            record = {"issue_id": observation["issue_id"],
                      "occurrence_id": observation["occurrence_id"],
                      "concept_ids": observation["concept_ids"],
                      "u2_concept": observation["u2_concept"]}
            if not observation["persisted_here"]:
                record["issue_id"] = None
                record["occurrence_id"] = None
            by_evidence[_get(item, "evidence_id")] = record
            recorded.append(observation)

        cards = _get(state, "cards", ())
        if not isinstance(cards, (tuple, list)) or len(cards) > 32:
            raise MemoryErrorBase("invalid U2 card collection")
        for index, card in enumerate(cards):
            recorded.append(_record_card(service, scope, session, values, state, card, index, by_evidence))

        run_concepts = tuple(sorted({concept for item in by_evidence.values()
                                     for concept in item["concept_ids"]}))
        run_concept = run_concepts[0] if len(run_concepts) == 1 else None

        if action_id is not None or action_kind is not None or previous_state is not None:
            if previous_state is None or action_id is None or action_kind is None:
                raise MemoryErrorBase("U2 action recording requires before state, action ID and kind")
            matching = next(iter(by_evidence.values()), {})
            prior_cards = _get(previous_state, "cards", ())
            card_id = (identifier(f"u2card:{run_id}:{len(prior_cards)}") if prior_cards else None)
            recorded.append(_record_action(service, scope, values, state, previous_state, action_result,
                                           action_id, action_kind, card_id,
                                           matching.get("concept_ids", ()), matching.get("issue_id"),
                                           matching.get("occurrence_id")))

        if checkpoint_ref is not None:
            checkpoint_ref = identifier(checkpoint_ref)
            fact = U2TeachingFacts(external_issue_id=values["issue_id"], status=status, stage=stage,
                                   current_version=version, current_level=level,
                                   checkpoint_ref=checkpoint_ref, concept_id=run_concept)
            checkpoint_event = LearningEvent(
                _base_event_id("u2-checkpoint", f"{run_id}:{version}:{checkpoint_ref}"),
                run_id, "u2_checkpoint_saved", run_concepts, u2=fact)
            checkpoint_event = _original_mapping(service, scope, checkpoint_event)
            recorded.append(service._record(scope, checkpoint_event,
                                            run_status=_active_run_status(status, stage), rebuild=False))

        if status in ("Completed", "Cancelled", "Failed", "BudgetExceeded"):
            fact = U2TeachingFacts(external_issue_id=values["issue_id"], status=status, stage=stage,
                                   current_version=version, current_level=level,
                                   checkpoint_ref=checkpoint_ref, concept_id=run_concept)
            terminal = LearningEvent(
                _base_event_id("u2-end", run_id), run_id, "u2_run_ended", run_concepts, u2=fact)
            terminal = _original_mapping(service, scope, terminal)
            recorded.append(service._record(scope, terminal,
                                            run_status=_run_status(status, stage), rebuild=False))
        if any(item.get("recorded") for item in recorded if isinstance(item, dict)):
            service._rebuild(scope.profile_id)
    return {"run_id": run_id, "u2_state_version": version, "status": status,
            "records": recorded}
