from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from .database import PROJECT_ROOT
from .retriever import retrieve_course_passages


DEFAULT_DATASET = PROJECT_ROOT / "tests" / "evaluation" / "course_retrieval_dev.jsonl"


def load_questions(path: Path) -> list[dict[str, Any]]:
    questions: list[dict[str, Any]] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ValueError(f"{path}:{line_number}: invalid JSON: {exc.msg}") from exc
        if not isinstance(row, dict) or not isinstance(row.get("question"), str):
            raise ValueError(f"{path}:{line_number}: expected a question object")
        if not isinstance(row.get("acceptable_passage_ids"), list):
            raise ValueError(f"{path}:{line_number}: acceptable_passage_ids must be a list")
        questions.append(row)
    return questions


def run_evaluation(path: Path = DEFAULT_DATASET, db_path: Path | None = None) -> dict[str, Any]:
    questions = load_questions(Path(path))
    rows: list[dict[str, Any]] = []
    answerable_total = 0
    answerable_hits = 0
    no_match_total = 0
    no_match_correct = 0
    per_query_recall: list[float] = []
    reciprocal_ranks: list[float] = []
    required_source_total = 0
    required_source_hits = 0
    false_positive_total = 0
    for question in questions:
        result = retrieve_course_passages(
            question["question"],
            concept_ids=question.get("concept_ids"),
            course_id=question.get("course_id"),
            language=question.get("language"),
            top_k=3,
            **({"db_path": db_path} if db_path is not None else {}),
        )
        acceptable = set(question["acceptable_passage_ids"])
        expected_no_match = question.get("expected_status") == "no_match" or not acceptable
        rankings = result.get("candidate_rankings") or [
            {"rank": rank, "passage_id": passage_id}
            for rank, passage_id in enumerate(result["retrieved_passage_ids"], 1)
        ]
        ranked_ids = [item["passage_id"] for item in rankings]
        retrieved_ids = result["retrieved_passage_ids"]
        if expected_no_match:
            no_match_total += 1
            hit = result["status"] == "no_match"
            no_match_correct += int(hit)
            false_positive_total += int(not hit)
        else:
            answerable_total += 1
            hits_at_10 = acceptable.intersection(ranked_ids[:10])
            hit = bool(acceptable.intersection(ranked_ids[:3]))
            answerable_hits += int(hit)
            per_query_recall.append(len(hits_at_10) / len(acceptable) if acceptable else 0.0)
            first_rank = next((rank for rank, passage_id in enumerate(ranked_ids[:3], 1) if passage_id in acceptable), None)
            reciprocal_ranks.append(1.0 / first_rank if first_rank else 0.0)
            for source_id in question.get("required_sources", []):
                required_source_total += 1
                required_source_hits += int(any(
                    passage_id in acceptable and passage_id.startswith(source_id + ":")
                    for passage_id in ranked_ids[:10]
                ))
        rows.append({
            "case": question.get("case"),
            "question": question["question"],
            "expected_status": "no_match" if expected_no_match else "match",
            "retrieved_passage_ids": retrieved_ids,
            "ranked_passage_ids_top10": ranked_ids[:10],
            "candidate_rankings": rankings[:10],
            "hit": hit,
            "recall_at_10": (len(acceptable.intersection(ranked_ids[:10])) / len(acceptable)) if acceptable else None,
            "source_coverage": result.get("source_coverage", []),
            "missing_sources": result.get("missing_sources", []),
        })
    return {
        "question_count": len(questions),
        "answerable_question_count": answerable_total,
        "hit_at_3": (answerable_hits / answerable_total) if answerable_total else None,
        "no_match_question_count": no_match_total,
        "no_match_correct": no_match_correct,
        "no_match_accuracy": (no_match_correct / no_match_total) if no_match_total else None,
        "recall_at_10": (sum(per_query_recall) / len(per_query_recall)) if per_query_recall else None,
        "mrr_at_3": (sum(reciprocal_ranks) / len(reciprocal_ranks)) if reciprocal_ranks else None,
        "required_source_coverage": (required_source_hits / required_source_total) if required_source_total else None,
        "required_source_hits": required_source_hits,
        "required_source_total": required_source_total,
        "false_positive_negative_queries": false_positive_total,
        "false_positive_rate": (false_positive_total / no_match_total) if no_match_total else None,
        "results": rows,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the small development retrieval set; it is not the final teacher set.")
    parser.add_argument("--dataset", type=Path, default=DEFAULT_DATASET)
    parser.add_argument("--db", type=Path, default=None)
    args = parser.parse_args()
    print(json.dumps(run_evaluation(args.dataset, args.db), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
