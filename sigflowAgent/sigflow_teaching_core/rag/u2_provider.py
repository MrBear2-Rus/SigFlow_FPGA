"""Development composition using U2's existing course evidence interface."""
from ..domain import CourseBundle, CourseMaterial, CourseReference, CourseSpan
from .unified import KnowledgeSearch, RetrievalConfig


class UnifiedCourseProvider:
    def __init__(self, config: RetrievalConfig):
        self.search = KnowledgeSearch(config)
        self.search.validate_configuration()

    def prepare(self, question: str, concept_ids: tuple[str, ...]) -> CourseBundle:
        result = self.search.retrieve(question, top_k=20)
        materials = []
        for rank, row in enumerate(result["passages"], 1):
            spans = []
            urls = []
            for span in row["source_spans"]:
                urls.append(span["source_url"] + (span.get("anchor") or ""))
                spans.append(CourseSpan(
                    span["document_id"], span["source_url"], span["line_start"],
                    span["line_end"], span["text_hash"],
                    span.get("source_text_hash", span["text_hash"]), span["document_hash"],
                    span.get("normalization"), span.get("locator_kind", "pinned_source"),
                ))
            authored = row["content_type"] == "authored_note"
            kind = "authored_note" if authored else "source_excerpt"
            note = "本地辅助讲解，待审核" if authored else "固定版本原文；不证明当前工程事实"
            reference = CourseReference(
                row["passage_id"], row["index_version"], row["title"], row["section"],
                urls[0], kind, note, tuple(urls), row["course_version"],
                source_content_hash=row["content_hash"], parent_id=row["passage_id"],
                source_spans=tuple(spans),
            )
            materials.append(CourseMaterial(
                reference, row["content"], row["passage_id"], row["source_id"],
                ("source:" + row["source_id"],), row["score"], rank,
            ))
        return CourseBundle(
            result["status"], tuple(materials), "evidence-v2", question,
            retrieved_candidates=tuple(row["passage_id"] for row in result["passages"]),
            selection_diagnostics={"answerability": "not_assessed"},
        )
