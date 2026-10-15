"""Resolve curated course source spans against pinned local documents and notes."""

from __future__ import annotations

import hashlib
import re
from html.parser import HTMLParser
from pathlib import Path
from typing import Any
from urllib.parse import unquote


HEADING_RE = re.compile(r"^(#{1,6})\s+(.+?)\s*$")


class _IdCollector(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.ids: set[str] = set()

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        self.ids.update(value for key, value in attrs if key == "id" and value)


def _digest(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode("utf-8")).hexdigest()


def load_source_evidence(
    passages: list[dict[str, Any]], resources_dir: Path, snapshot_dir: Path,
) -> tuple[list[dict[str, Any]], dict[str, dict[str, Any]]]:
    from .importer import ImportValidationError, _read_jsonl, _read_yaml, _source_base_urls, _validate_url

    manifest_path = snapshot_dir / "manifest.jsonl"
    if not manifest_path.is_file():
        raise ImportValidationError(f"YSYX snapshot manifest is missing: {manifest_path}")
    documents: list[dict[str, Any]] = []
    document_by_chapter: dict[str, dict[str, Any]] = {}
    for row in _read_jsonl(manifest_path):
        chapter = row.get("chapter")
        clean_path = row.get("clean_path")
        if not isinstance(chapter, str) or chapter in document_by_chapter or not isinstance(clean_path, str):
            raise ImportValidationError("YSYX snapshot has an invalid or duplicate chapter")
        relative = Path(clean_path)
        if relative.is_absolute() or ".." in relative.parts or relative.suffix != ".md":
            raise ImportValidationError(f"unsafe clean_path for YSYX {chapter}")
        path = (snapshot_dir / relative).resolve()
        if not path.is_file() or not path.is_relative_to(snapshot_dir.resolve()):
            raise ImportValidationError(f"YSYX clean Markdown is missing: {path}")
        body = path.read_text(encoding="utf-8")
        if _digest(body) != "sha256:" + str(row.get("content_hash")):
            raise ImportValidationError(f"YSYX snapshot hash mismatch: {chapter}")
        raw_relative = Path(str(row.get("raw_path", "")))
        if raw_relative.is_absolute() or ".." in raw_relative.parts or raw_relative.suffix != ".html":
            raise ImportValidationError(f"unsafe raw_path for YSYX {chapter}")
        raw_path = (snapshot_dir / raw_relative).resolve()
        if not raw_path.is_file() or not raw_path.is_relative_to(snapshot_dir.resolve()):
            raise ImportValidationError(f"YSYX raw HTML is missing: {raw_path}")
        raw = raw_path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != row.get("raw_sha256"):
            raise ImportValidationError(f"YSYX raw HTML hash mismatch: {chapter}")
        collector = _IdCollector()
        collector.feed(raw.decode("utf-8"))
        document = {
            "document_id": f"ysyx:2607:{chapter}", "chapter": chapter,
            "source_id": "ysyx", "source_url": row["url"], "title": row["title"],
            "local_path": str(path), "document_hash": _digest(body),
            "retrieved_at": row["retrieved_at"], "lines": body.splitlines(),
            "html_ids": collector.ids,
        }
        documents.append(document)
        document_by_chapter[chapter] = document

    # Existing local notes remain authored material with their original identity.
    sources = {row["source_id"]: row for row in _read_yaml(resources_dir / "sources.yaml", "sources")}
    external_root = resources_dir.resolve()
    for row in _read_jsonl(resources_dir / "external_documents.jsonl"):
        chapter = row.get("chapter")
        document_id = row.get("document_id")
        source_id = row.get("source_id")
        source = sources.get(source_id)
        relative = Path(str(row.get("local_path", "")))
        if (not isinstance(chapter, str) or not chapter.startswith("external:")
                or not isinstance(document_id, str) or not document_id
                or any(d["document_id"] == document_id for d in documents)
                or not isinstance(row.get("title"), str) or not row["title"].strip()
                or not isinstance(row.get("retrieved_at"), str) or not row["retrieved_at"].strip()
                or chapter in document_by_chapter
                or source is None or relative.is_absolute() or ".." in relative.parts
                or relative.suffix != ".md"):
            raise ImportValidationError(f"invalid external course document: {document_id or '?'}")
        url = _validate_url(row.get("source_url"), f"external document `{chapter}`", _source_base_urls(source))
        path = (resources_dir / relative).resolve()
        if not path.is_file() or not path.is_relative_to(external_root):
            raise ImportValidationError(f"external course note is missing or outside resources: {path}")
        body = path.read_text(encoding="utf-8")
        digest = _digest(body)
        if digest != row.get("document_hash") or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
            raise ImportValidationError(f"external course note hash mismatch: {chapter}")
        document = {
            "document_id": document_id, "chapter": chapter,
            "source_id": source_id, "source_url": url, "title": row.get("title"),
            "local_path": str(path), "document_hash": digest,
            "retrieved_at": row.get("retrieved_at"), "lines": body.splitlines(),
            "html_ids": set(),
        }
        documents.append(document)
        document_by_chapter[chapter] = document

    # Pinned official sources use a separate explicit file allow-list. Resolve
    # only paths listed there, under data/external_sources; never walk a repo.
    from .external_materials import RAW_ROOT, digest_bytes
    file_rows = _read_jsonl(resources_dir / "external_source_files.jsonl")
    file_catalog = {row.get("source_path"): row for row in file_rows if isinstance(row.get("source_path"), str)}
    if len(file_catalog) != len(file_rows):
        raise ImportValidationError("external_source_files.jsonl has duplicate or invalid paths")
    for row in _read_jsonl(resources_dir / "external_official_documents.jsonl"):
        source_path = row.get("source_path")
        catalog = file_catalog.get(source_path)
        source_id = row.get("source_id")
        source = sources.get(source_id)
        chapter = row.get("chapter")
        document_id = row.get("document_id")
        if (catalog is None or source is None or not isinstance(chapter, str)
                or not chapter.startswith("external:") or not isinstance(document_id, str)
                or not document_id.startswith("official:") or document_id in {d["document_id"] for d in documents}
                or chapter in document_by_chapter or row.get("sha256") != catalog.get("sha256")):
            raise ImportValidationError(f"invalid official source document: {source_path or '?'}")
        url = _validate_url(row.get("source_url"), f"official source `{source_path}`", _source_base_urls(source))
        raw_path = (RAW_ROOT / Path(*Path(str(source_path)).parts)).resolve()
        if not raw_path.is_relative_to(RAW_ROOT.resolve()) or not raw_path.is_file():
            raise ImportValidationError(f"official source file is missing or outside the allow-list root: {source_path}")
        raw = raw_path.read_bytes()
        if digest_bytes(raw) != catalog.get("sha256"):
            raise ImportValidationError(f"official source hash mismatch: {source_path}")
        body = raw.decode("utf-8")
        document = {
            "document_id": document_id, "chapter": chapter, "source_id": source_id,
            "source_url": url, "title": row.get("title"), "local_path": str(raw_path),
            "document_hash": catalog["sha256"], "retrieved_at": row.get("retrieved_at"),
            "lines": body.splitlines(), "html_ids": set(), "source_path": source_path,
        }
        documents.append(document)
        document_by_chapter[chapter] = document

    span_records = _read_jsonl(resources_dir / "source_spans.jsonl")
    span_records.extend(_read_jsonl(resources_dir / "external_official_spans.jsonl"))
    if len(span_records) != len(passages):
        raise ImportValidationError("source_spans.jsonl must map every passage exactly once")
    passage_map = {row["passage_id"]: row for row in passages}
    evidence: dict[str, dict[str, Any]] = {}
    for record in span_records:
        passage_id = record.get("passage_id")
        if passage_id not in passage_map or passage_id in evidence:
            raise ImportValidationError(f"invalid or duplicate source span passage_id: {passage_id}")
        status = record.get("evidence_status")
        if status not in {"grounded", "partial"}:
            raise ImportValidationError(f"{passage_id}: invalid evidence_status")
        if status == "partial" and not record.get("evidence_note"):
            raise ImportValidationError(f"{passage_id}: partial evidence needs a note")
        spans = record.get("spans")
        if not isinstance(spans, list) or not spans:
            raise ImportValidationError(f"{passage_id}: at least one source span is required")
        passage = passage_map[passage_id]
        known_refs = [(passage["source_url"], passage.get("anchor"))]
        known_refs.extend((ref["source_url"], ref.get("anchor")) for ref in passage.get("supporting_sources", []))
        resolved = []
        seen = set()
        for ordinal, span in enumerate(spans):
            chapter = span.get("chapter")
            document = document_by_chapter.get(chapter)
            if document is None or document["source_url"] not in [url for url, _ in known_refs]:
                raise ImportValidationError(f"{passage_id}: span chapter is not a declared source: {chapter}")
            if span.get("anchor") is not None and (document["source_url"], span["anchor"]) not in known_refs:
                raise ImportValidationError(f"{passage_id}: span anchor is not an allow-listed citation: {span.get('anchor')}")
            start, end = span.get("line_start"), span.get("line_end")
            if (not isinstance(start, int) or isinstance(start, bool) or not isinstance(end, int)
                    or isinstance(end, bool) or start < 1 or end < start or end > len(document["lines"])):
                raise ImportValidationError(f"{passage_id}: invalid line range for {chapter}")
            if (chapter, start, end) in seen:
                raise ImportValidationError(f"{passage_id}: duplicate source span")
            seen.add((chapter, start, end))
            original_excerpt = "\n".join(document["lines"][start - 1:end])
            if span.get("source_text_hash") and _digest(original_excerpt) != span["source_text_hash"]:
                raise ImportValidationError(f"{passage_id}: original source mapping hash mismatch for {chapter}")
            excerpt = span.get("normalized_text", original_excerpt)
            if document.get("source_path") is not None:
                excerpt = excerpt.strip("\n")
            if (not isinstance(excerpt, str) or not excerpt.strip()
                    or (status == "grounded" and document.get("source_path") is None and len(excerpt.strip()) < 30)):
                raise ImportValidationError(f"{passage_id}: source span is empty or too short")
            headings = span.get("heading_path")
            if not isinstance(headings, list) or not all(isinstance(item, str) for item in headings):
                headings = []
                from .chunks import markdown_headings
                for _, depth, title in markdown_headings(document["lines"][:start]):
                    headings = headings[:depth - 1] + [title]
            anchor = span.get("anchor")
            if anchor is None:
                anchor = next(anchor for url, anchor in known_refs if url == document["source_url"])
            if anchor and (document.get("source_path") is not None):
                match = re.fullmatch(r"#L(\d+)-L(\d+)", anchor)
                if not match or (int(match.group(1)), int(match.group(2))) != (start, end):
                    raise ImportValidationError(f"{passage_id}: official line anchor does not match its source span: {anchor}")
            elif anchor and (not anchor.startswith("#") or unquote(anchor[1:]) not in document["html_ids"]):
                raise ImportValidationError(f"{passage_id}: source anchor is absent from pinned HTML: {anchor}")
            resolved.append({
                "span_order": ordinal, "document_id": document["document_id"], "chapter": chapter,
                "source_url": document["source_url"], "anchor": anchor,
                "local_path": document["local_path"], "line_start": start, "line_end": end,
                "heading_path": headings, "text": excerpt, "text_hash": _digest(excerpt),
                "character_count": len(excerpt), "source_text_hash": _digest(original_excerpt),
                "normalization": span.get("normalization"),
            })
        combined = "\n\n".join(span["text"] for span in resolved)
        evidence[passage_id] = {
            "evidence_status": status, "evidence_note": record.get("evidence_note"),
            "content": combined, "content_hash": _digest(combined), "spans": resolved,
        }
    return documents, evidence
