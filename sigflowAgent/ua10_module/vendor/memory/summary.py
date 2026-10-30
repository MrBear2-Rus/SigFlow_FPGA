"""Pure deterministic projection. Events are facts; summaries can be discarded."""
from __future__ import annotations


def derive_summaries(events, session_scopes):
    # Enrichment supplies metadata for the original observation, not a new
    # diagnostic at delivery time. Preserve causal ordering and learning outcomes.
    mappings = {row["occurrence_id"]: row["payload"]["concept_ids"] for row in events
                if row["event_type"] == "u2_observation_enriched" and row["payload"]["concept_ids"]}
    summaries, attempts, run_help, seen_jobs = {}, {}, {}, set()
    for row in events:
        p = row["payload"]
        if p["event_type"] == "u2_observation_enriched":
            continue
        if p["event_type"] == "u2_diagnostic_observed" and not p["concept_ids"] and row["occurrence_id"] in mappings:
            p = {**p, "concept_ids": mappings[row["occurrence_id"]]}
        project_id, instance_id = session_scopes[row["session_id"]]
        if p["evidence_kind"] == "real" and p["job"] and p["event_type"] in ("issue_observed", "retry_passed", "retry_failed"):
            key = (row["profile_id"], project_id, instance_id, p["issue_id"], p["job"]["job_id"],
                   "observation" if p["event_type"] == "issue_observed" else "outcome")
            if key in seen_jobs:
                continue
            seen_jobs.add(key)
        attempt_key = (row["session_id"], p.get("occurrence_id") or p["run_id"])
        attempt = attempts.setdefault(attempt_key, {"hint": 0, "reference": False, "help": False,
                                                     "help_requested": False, "modified": False,
                                                     "reflection": False, "runs": set()})
        attempt["runs"].add(p["run_id"])
        mock = p["evidence_kind"] != "real"
        if p["evidence_kind"] == "imported_unverified":
            # Imported history cannot supply real edit/reflection facts. Retain conservative help.
            attempt["help"] = True
            attempt["reference"] |= p["reference_viewed"] or p["hint_level"] == 4
        if not mock:
            if p["event_type"] == "u2_hint_accepted":
                # A request is evidence of help-seeking, but without a UI receipt
                # it does not prove the generated card reached the student.
                attempt["help_requested"] = True
            general = run_help.setdefault(p["run_id"], {"hint": 0, "reference": False, "help": False})
            if not p["occurrence_id"]:
                general["hint"] = max(general["hint"], p["hint_level"])
                general["reference"] |= p["reference_viewed"] or p["event_type"] == "reference_viewed"
                general["help"] |= p["hint_level"] > 0 or p["course"] is not None
                # A run-wide reference can arrive after its issue observation. Carry it forward
                # to those occurrences even if the retest later happens in a different run.
                for related_attempt in attempts.values():
                    if p["run_id"] in related_attempt["runs"]:
                        related_attempt["hint"] = max(related_attempt["hint"], general["hint"])
                        related_attempt["reference"] |= general["reference"]
                        related_attempt["help"] |= general["help"]
            attempt["hint"] = max(attempt["hint"], general["hint"])
            attempt["reference"] |= general["reference"]
            attempt["help"] |= general["help"]
            attempt["hint"] = max(attempt["hint"], p["hint_level"])
            attempt["reference"] |= p["reference_viewed"] or p["hint_level"] == 4 or p["event_type"] == "reference_viewed"
            attempt["help"] |= p["hint_level"] > 0 or p["course"] is not None
            attempt["modified"] |= p["student_modified"]
            attempt["reflection"] |= bool(p["explanation_ref"] or p["followup_ref"])
        job = p["job"]
        checked_job = bool(not mock and job and job["kind"] == "real" and job["complete"]
                           and job["validation"] in ("test_expectation", "diagnostic_absence")
                           and job["scope_ref"] and p.get("new_job"))
        successful = checked_job and job["outcome"] == "passed" and attempt["modified"]
        for concept_id in p["concept_ids"]:
            key = (project_id, instance_id, concept_id)
            summary = summaries.setdefault(key, {
                "concept_id": concept_id, "status": "needs_review", "practice_count": 0,
                "assistance_level": 0, "reference_viewed": False, "last_seen": None,
                "recent_evidence": [], "reason": "insufficient_learning_evidence", "mock_event_count": 0,
                "observation_scope": "recorded_exercise_only",
                "imported_event_count": 0,
            })
            if p["evidence_kind"] == "imported_unverified":
                summary["imported_event_count"] += 1
                continue
            if mock:
                summary["mock_event_count"] += 1
                continue
            summary["last_seen"] = row["created_at"]
            summary["assistance_level"] = max(summary["assistance_level"], attempt["hint"])
            summary["reference_viewed"] |= attempt["reference"]
            if not summary["recent_evidence"]:
                summary.update(status="exposed", reason="concept_encountered")
            if p["event_type"] in ("issue_observed", "u2_diagnostic_observed", "retry_failed"):
                summary.update(status="needs_review", reason="observed_issue_or_failed_retry")
            elif attempt["reference"] and p["event_type"] in ("reference_viewed", "hint_presented"):
                summary.update(status="reference_viewed", reason="reference_seen_requires_new_practice")
            if p["event_type"] in ("retry_passed", "retry_failed"):
                if checked_job:
                    summary["practice_count"] += 1
                if successful:
                    if attempt["help"] or attempt["reference"]:
                        summary.update(status="assisted_success", reason="new_scoped_result_after_student_edit_with_help")
                    elif attempt["help_requested"]:
                        summary.update(status="needs_review", reason="help_requested_delivery_unconfirmed")
                    elif attempt["reflection"]:
                        summary.update(status="independent_success", reason="new_scoped_result_edit_and_reflection_without_help")
                    else:
                        summary.update(status="needs_review", reason="success_without_explanation_or_followup")
                elif p["event_type"] == "retry_passed":
                    summary.update(status="needs_review", reason="success_evidence_insufficient")
            summary["recent_evidence"].append({
                "event_id": row["event_id"], "event_type": p["event_type"],
                "assistance": "L" + str(attempt["hint"]) if attempt["hint"] else "none",
                "reference_viewed": attempt["reference"], "time": row["created_at"],
                "outcome": job["outcome"] if job else None,
                "evidence_refs": p["evidence_refs"][:3],
            })
            summary["recent_evidence"] = summary["recent_evidence"][-3:]
        if p["event_type"] in ("retry_passed", "retry_failed") and not mock:
            # A subsequent test requires another student edit/reflection. Help remains conservative.
            attempt["modified"] = False
            attempt["reflection"] = False
    return summaries
