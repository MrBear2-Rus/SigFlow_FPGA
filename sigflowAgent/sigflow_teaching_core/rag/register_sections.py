"""Create stable, heading-bounded section records for the pinned YSYX snapshots."""

from __future__ import annotations

import hashlib
import json
import argparse
import re
from pathlib import Path
from typing import Any

import yaml

from .database import RESOURCE_ROOT


RESOURCE_DIR = RESOURCE_ROOT
PASSAGE_PATH = RESOURCE_DIR / "passages" / "ysyx_sections.jsonl"
CATALOG_PATH = RESOURCE_DIR / "ysyx_section_catalog.jsonl"
SPANS_PATH = RESOURCE_DIR / "source_spans.jsonl"
CONCEPTS_PATH = RESOURCE_DIR / "concepts.yaml"
HEADING_RE = re.compile(r"^(#{2,4})\s+(.+?)\s*$")
PLACEHOLDER_RE = re.compile(r"(?<!\])\[\](?!\()")
LINK_RE = re.compile(r"\[([^\]]*)\]\([^)]*\)")
IMAGE_RE = re.compile(r"!\[[^\]]*\]\([^)]*\)")


def _jsonl(path: Path) -> list[dict[str, Any]]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def _write_jsonl(path: Path, rows: list[dict[str, Any]]) -> None:
    path.write_text("".join(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n" for row in rows), encoding="utf-8")


def _text_quality(body: str, title: str) -> tuple[str, str | None, list[str]]:
    flags: list[str] = []
    if PLACEHOLDER_RE.search(body):
        flags.append("markdown_formula_or_symbol_missing")
    if re.search(r"^\s*\[\s*$", body, re.MULTILINE) or re.search(r"^\s*\]\s*$", body, re.MULTILINE):
        flags.append("markdown_link_placeholder")
    visible = IMAGE_RE.sub("", LINK_RE.sub(r"\1", body))
    if "![" in body and len(re.sub(r"\W", "", visible, flags=re.UNICODE)) < 120:
        flags.append("image_dependent_explanation")
    lines = [line.strip() for line in body.splitlines() if line.strip()]
    action_prefix = re.compile(r"^(尝试|请|完成|搭建|查看|运行|阅读|记录|分析|观察|在.+中|通过.+实现)")
    descriptive = sum(bool(action_prefix.search(line)) for line in lines)
    compact_text = re.sub(r"\s+", "", visible)
    if descriptive and descriptive / max(len(lines), 1) >= 0.7 and len(compact_text) < 700:
        flags.append("exercise_prompt_without_explanatory_answer")
    if len(compact_text) < 50:
        flags.append("too_little_explanatory_text")
    if flags:
        return "partial", "；".join(flags), flags
    return "grounded", None, flags


def _course_for(chapter: str, start: int) -> str:
    if chapter == "F3":
        return "ysyx-digital-logic"
    if chapter == "F4":
        return "ysyx-computer-systems"
    if chapter == "E1":
        return "ysyx-hdl-learning"
    if chapter == "E4":
        return "ysyx-processor-verification" if start >= 547 else "ysyx-rtl-engineering"
    if chapter == "D6":
        if start < 726:
            return "ysyx-verilog-semantics"
        return "ysyx-asic-flow" if start >= 1398 else "ysyx-rtl-synthesis"
    raise ValueError(f"unsupported YSYX chapter: {chapter}")


def build_records(snapshot_dir: Path, force_resegment: bool = False) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]]]:
    existing_passages = _jsonl(RESOURCE_DIR / "passages" / "ysyx.jsonl")
    existing_ids = {row["passage_id"] for row in existing_passages}
    existing_spans = [row for row in _jsonl(SPANS_PATH) if row["passage_id"] in existing_ids]
    manifest = _jsonl(snapshot_dir / "manifest.jsonl")
    passage_rows: list[dict[str, Any]] = []
    span_rows: list[dict[str, Any]] = []
    catalog_rows: list[dict[str, Any]] = []
    concept_rows: list[dict[str, Any]] = []

    for doc in manifest:
        chapter = doc["chapter"]
        lines = (snapshot_dir / doc["clean_path"]).read_text(encoding="utf-8").splitlines()
        # The first 12 lines are scrape metadata/navigation; trailing bracket pairs are empty embeds.
        first_body_line = 13
        end_line = len(lines)
        while end_line >= first_body_line and (not lines[end_line - 1].strip() or lines[end_line - 1].strip() in {"[", "]"}):
            end_line -= 1
        headings: list[tuple[int, int, str]] = []
        from .chunks import markdown_headings
        headings = [entry for entry in markdown_headings(lines)
                    if first_body_line <= entry[0] <= end_line]
        if not headings:
            continue

        # Keep the introductory material with the chapter title, then cut at the clean document's
        # semantic heading levels. No fixed character window or overlap is used.
        chapter_title = doc["title"].split(" | ")[0]
        # Include the duplicated article title and its opening subsection in one first unit;
        # the source's first heading is often just a reading warning or section preface.
        starts = [(first_body_line, 1, chapter_title)]
        starts.extend((line_no, depth, title) for line_no, depth, title in headings[1:])
        starts = sorted({entry[0]: entry for entry in starts}.values())
        for ordinal, (start, depth, title) in enumerate(starts):
            end = (starts[ordinal + 1][0] - 1) if ordinal + 1 < len(starts) else end_line
            if end < start:
                continue
            if ordinal == 0:
                heading_path = [chapter_title, headings[0][2]]
            else:
                ancestor_titles: list[str] = []
                for heading_line, heading_depth, heading_title in headings:
                    if heading_line > start:
                        break
                    ancestor_titles = ancestor_titles[:heading_depth - 2] + [heading_title]
                heading_path = [chapter_title, *ancestor_titles]
            text = "\n".join(lines[start - 1:end])
            body = "\n".join(lines[start:end])
            status, note, flags = _text_quality(body, title)
            path_material = json.dumps([chapter, heading_path], ensure_ascii=False, separators=(",", ":"))
            digest = hashlib.sha1(path_material.encode("utf-8")).hexdigest()[:12]
            passage_id = f"ysyx:{chapter.lower()}:section-{digest}"
            concept_id = f"ysyx_{chapter.lower()}_section_{digest}"
            url = doc["url"]
            section = " > ".join(heading_path)
            authored_preview = f"课程原文结构登记：{section}。以检索返回的原文片段为准。"
            passage_rows.append({
                "passage_id": passage_id, "course_id": _course_for(chapter, start),
                "course_version": "2607", "title": title, "section": section,
                "concept_ids": [concept_id], "content": authored_preview,
                "content_type": "reviewed_summary", "source_id": "ysyx", "source_url": url,
                "anchor": None, "author_or_source": "一生一芯官方文档；按本地章节标题结构登记",
                "license_or_usage_note": "原文缓存仅限本机索引；正文再分发许可未核实，遵循 sources.yaml 的使用边界。",
                "retrieved_at": doc["retrieved_at"][:10], "language": "zh-CN",
                "difficulty": "intermediate", "content_hash": "sha256:" + hashlib.sha256(authored_preview.encode("utf-8")).hexdigest(),
                "registration_status": "structure_registered_unreviewed", "quality_flags": flags,
            })
            span_rows.append({
                "passage_id": passage_id, "evidence_status": status,
                **({"evidence_note": note} if note else {}),
                "spans": [{"chapter": chapter, "line_start": start, "line_end": end}],
            })
            concept_rows.append({
                "concept_id": concept_id, "aliases": [title], "related": [],
            })
            catalog_rows.append({
                "passage_id": passage_id, "chapter": chapter, "course_id": _course_for(chapter, start),
                "title": title, "heading_path": heading_path, "line_start": start, "line_end": end,
                "character_count": len(text), "evidence_status": status,
                "registration_status": "structure_registered_unreviewed", "quality_flags": flags,
            })

    existing_span_ids = {row["passage_id"] for row in existing_spans}
    if existing_ids.intersection(row["passage_id"] for row in passage_rows):
        raise RuntimeError("section registration collided with an existing stable passage_id")
    if existing_ids != existing_span_ids:
        raise RuntimeError("existing hand-curated passages and source spans are inconsistent")
    current_generated = _jsonl(PASSAGE_PATH) if PASSAGE_PATH.exists() else []
    generated_ids = {row["passage_id"] for row in current_generated}
    if generated_ids != {row["passage_id"] for row in passage_rows}:
        # Existing generated rows must not silently disappear after snapshot changes.
        missing = sorted(generated_ids - {row["passage_id"] for row in passage_rows})
        if missing and not force_resegment:
            raise RuntimeError("snapshot update removed registered section IDs; review explicitly: " + ", ".join(missing[:5]))
    current_concepts = yaml.safe_load(CONCEPTS_PATH.read_text(encoding="utf-8"))["concepts"]
    known_concepts = {row["concept_id"] for row in current_concepts}
    concept_rows = [row for row in concept_rows if row["concept_id"] not in known_concepts]
    return passage_rows, span_rows, catalog_rows, current_concepts + concept_rows


def register_sections(snapshot_dir: Path, force_resegment: bool = False) -> dict[str, int]:
    if force_resegment:
        current = yaml.safe_load(CONCEPTS_PATH.read_text(encoding="utf-8"))["concepts"]
        retained = [row for row in current if not re.match(r"^ysyx_[a-z0-9]+_section_[0-9a-f]{12}$", row["concept_id"])]
        CONCEPTS_PATH.write_text(yaml.safe_dump({"concepts": retained}, allow_unicode=True, sort_keys=False, width=120), encoding="utf-8")
    passage_rows, span_rows, catalog_rows, concepts = build_records(Path(snapshot_dir), force_resegment)
    generated_ids = {row["passage_id"] for row in (_jsonl(PASSAGE_PATH) if PASSAGE_PATH.exists() else [])}
    existing_span_rows = [row for row in _jsonl(SPANS_PATH) if row["passage_id"] not in generated_ids]
    _write_jsonl(PASSAGE_PATH, passage_rows)
    _write_jsonl(SPANS_PATH, existing_span_rows + span_rows)
    CONCEPTS_PATH.write_text(yaml.safe_dump({"concepts": concepts}, allow_unicode=True, sort_keys=False, width=120), encoding="utf-8")
    _write_jsonl(CATALOG_PATH, catalog_rows)
    counts = {"section_count": len(passage_rows)}
    for status in {row["evidence_status"] for row in catalog_rows}:
        counts[f"{status}_count"] = sum(row["evidence_status"] == status for row in catalog_rows)
    counts["chapters"] = len({row["chapter"] for row in catalog_rows})
    counts["characters"] = sum(row["character_count"] for row in catalog_rows)
    return counts


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Register all pinned YSYX clean Markdown sections by heading.")
    parser.add_argument("--snapshot-dir", type=Path, required=True)
    parser.add_argument("--force-resegment", action="store_true", help="Replace generated section IDs after reviewed boundary changes.")
    arguments = parser.parse_args()
    print(json.dumps(register_sections(arguments.snapshot_dir, arguments.force_resegment), ensure_ascii=False, indent=2))
