"""Small bridges to UA-09/UA-11; no teaching Agent and no EDA execution."""
from __future__ import annotations

from dataclasses import replace
from pathlib import Path

from sigflow_teaching_core.rag.citations import render_citations
from sigflow_teaching_core.rag.database import DEFAULT_DB_PATH
from ua11.fixture_models import Fixture
from ua11.fixture_runner import run_fixture

from .models import CourseInteraction, JobEvidence, LearningEvent, MemoryErrorBase


class CourseReferenceAdapter:
    def __init__(self, db_path: Path = DEFAULT_DB_PATH):
        self.path = Path(db_path)

    def interaction(self, passage_id, retrieval_result, interaction):
        if interaction not in ("shown", "clicked", "used"):
            raise MemoryErrorBase("invalid course interaction")
        citations = render_citations([passage_id], retrieval_result, db_path=self.path)
        if not citations:
            raise MemoryErrorBase("course citation unavailable")
        return CourseInteraction(passage_id, retrieval_result["index_version"], interaction)

    def record(self, service, scope, event: LearningEvent, retrieval_result):
        if event.course is None:
            raise MemoryErrorBase("course event requires a passage")
        course = self.interaction(event.course.passage_id, retrieval_result, event.course.interaction)
        return service.record_learning_event(scope, replace(event, course=course))


def record_mock_fixture(service, scope, run_id, event_id, fixture: Fixture):
    if fixture.kind != "mock":
        raise MemoryErrorBase("real fixture metadata cannot replace verified Gateway evidence")
    result = run_fixture(fixture)
    if not result["passed"]:
        raise MemoryErrorBase("UA-11 fixture structure check failed")
    run = service.repo.check_run(scope, run_id)
    if run["execution_mode"] != "mock":
        raise MemoryErrorBase("Mock fixtures require a mock run")
    metadata = fixture.metadata
    if result["output"]["issue_detected"] is not True:
        return service.record_learning_event(scope, LearningEvent(
            event_id, run_id, "concept_exposed", tuple(metadata["concept_ids"]),
            evidence_refs=tuple(result["output"]["evidence_refs"])))
    job = JobEvidence("mock-job:" + fixture.fixture_id, scope.project_id, scope.instance_id,
                      run["revision"], run["snapshot_id"], metadata["evidence"]["report_ref"],
                      metadata["inputs"]["rtl_hash"], kind="mock", outcome="failed",
                      complete=metadata["evidence"]["completeness"] == "mock_complete")
    return service.observe_issue(scope, event_id, run_id, metadata["category"], "fixture:" + fixture.fixture_id,
                                 tuple(metadata["concept_ids"]), job, observation_key=fixture.fixture_id,
                                 evidence_refs=tuple(result["output"]["evidence_refs"]))
