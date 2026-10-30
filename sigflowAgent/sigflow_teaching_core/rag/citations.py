from __future__ import annotations

import json
import sqlite3
from pathlib import Path
from typing import Any, Iterable

from .database import DEFAULT_DB_PATH, open_database


class CitationValidationError(ValueError):
    """Raised when a model cites a passage outside the current retrieval result."""


def validate_citations(cited_passage_ids: Iterable[str], retrieved_passage_ids: Iterable[str]) -> bool:
    retrieved = list(retrieved_passage_ids)
    cited = list(cited_passage_ids)
    if any(type(i) is not str for i in cited + retrieved) or len(cited) != len(set(cited)):
        raise CitationValidationError("invalid evidence citation IDs")
    retrieved = set(retrieved)
    invalid = sorted({passage_id for passage_id in cited if passage_id not in retrieved})
    if invalid:
        raise CitationValidationError("citation was not retrieved in this call: " + ", ".join(invalid))
    return True


def render_citations(
    cited_passage_ids: Iterable[str],
    retrieval_result: dict[str, Any],
    *,
    db_path: Path = DEFAULT_DB_PATH,
) -> list[dict[str, Any]]:
    """Resolve citation details from SQLite after checking this retrieval's allow-list."""
    passages = retrieval_result.get("passages")
    if not isinstance(passages, list):
        raise CitationValidationError("retrieval result does not contain a passage list")
    by_id = {row["passage_id"]: row for row in passages}
    cited = list(cited_passage_ids)
    validate_citations(cited, by_id)
    if not cited:
        return []
    placeholders = ",".join("?" for _ in cited)
    connection = open_database(db_path)
    try:
        version = connection.execute("SELECT value FROM index_meta WHERE key='index_version'").fetchone()[0]
        if retrieval_result.get("index_version") != version:
            raise CitationValidationError("course database changed; retrieve again")
        rows = connection.execute(
            "SELECT passage_id, title, section, source_id, source_url, anchor, retrieved_at, content_type, course_version, content_hash "
            f"FROM passages WHERE passage_id IN ({placeholders})",
            cited,
        ).fetchall()
        metadata = {row["passage_id"]: dict(row) for row in rows}
        missing = [passage_id for passage_id in cited if passage_id not in metadata]
        if missing:
            raise CitationValidationError("retrieved passage is absent from the current index: " + ", ".join(missing))
        refs = connection.execute(
            "SELECT passage_id, source_id, source_url, anchor, reference_kind "
            f"FROM passage_sources WHERE passage_id IN ({placeholders}) ORDER BY passage_id, reference_order",
            cited,
        ).fetchall()
        refs_by_id: dict[str, list[dict[str, Any]]] = {passage_id: [] for passage_id in cited}
        for row in refs:
            refs_by_id[row["passage_id"]].append(
                {
                    "source_id": row["source_id"],
                    "source_url": row["source_url"],
                    "anchor": row["anchor"],
                    "reference_kind": row["reference_kind"],
                }
            )
        span_rows = connection.execute(
            "SELECT ps.passage_id, ps.document_id, sd.chapter, sd.source_url, sd.document_hash, "
            "ps.anchor, ps.line_start, ps.line_end, ps.heading_path_json, ps.text_hash "
            "FROM passage_spans ps JOIN source_documents sd ON sd.document_id = ps.document_id "
            f"WHERE ps.passage_id IN ({placeholders}) ORDER BY ps.passage_id, ps.span_order",
            cited,
        ).fetchall()
        spans_by_id: dict[str, list[dict[str, Any]]] = {passage_id: [] for passage_id in cited}
        for row in span_rows:
            span={
                "document_id":row["document_id"],
                "chapter": row["chapter"], "source_url": row["source_url"], "anchor": row["anchor"],
                "line_start": row["line_start"],
                "line_end": row["line_end"], "heading_path": json.loads(row["heading_path_json"]),
                "text_hash": row["text_hash"], "document_hash": row["document_hash"],
            }
            if metadata[row["passage_id"]]["content_type"]=="authored_note":
                span["source_url"]="sigflow-note:///"+row["document_id"]
                span["anchor"]=f"#L{row['line_start']}-L{row['line_end']}"
                span["locator_kind"]="local_authored_note"
            spans_by_id[row["passage_id"]].append(span)
        for pid,ref in metadata.items():
            if ref["content_type"]=="authored_note" and spans_by_id[pid]:
                ref["source_url"]=spans_by_id[pid][0]["source_url"]
                ref["anchor"]=spans_by_id[pid][0]["anchor"]
                for supporting in refs_by_id[pid]:
                    supporting["reference_kind"]="supporting_official_reference"
        return [{**metadata[passage_id], "source_references": refs_by_id[passage_id],
                 "source_spans": spans_by_id[passage_id]} for passage_id in cited]
    finally:
        connection.close()


def resolve_evidence_citations(cited_ids: Iterable[str], admitted_ids: Iterable[str], *,
                               index_version: str, db_path: Path = DEFAULT_DB_PATH) -> list[dict[str,Any]]:
    """Resolve v2 child citations from the DB, limited to actually packed IDs."""
    from .chunks import digest
    cited=list(cited_ids);admitted=list(admitted_ids)
    if any(type(i) is not str for i in cited+admitted) or len(cited)!=len(set(cited)):
        raise CitationValidationError("invalid evidence citation IDs")
    validate_citations(cited,admitted)
    connection=open_database(db_path)
    try:
        if connection.execute("SELECT value FROM index_meta WHERE key='index_version'").fetchone()[0]!=index_version:
            raise CitationValidationError("course database changed; retrieve again")
        answer=[]
        for cid in cited:
            row=connection.execute(
                "SELECT ec.*,p.title,p.section,p.course_version,p.content_type,p.source_id,p.license_or_usage_note "
                "FROM evidence_chunks ec JOIN passages p ON p.passage_id=ec.parent_id WHERE ec.chunk_id=?",(cid,)).fetchone()
            if row is None:raise CitationValidationError("evidence chunk is absent: "+cid)
            spans=json.loads(row["spans_json"])
            if digest(row["content"])!=row["content_hash"] or "\n\n".join(s["text"] for s in spans)!=row["content"]:
                raise CitationValidationError("evidence chunk content hash mismatch")
            for s in spans:
                source=connection.execute("SELECT document_hash, source_url FROM source_documents WHERE document_id=?",(s["document_id"],)).fetchone()
                parent_spans=connection.execute(
                    "SELECT line_start,line_end FROM passage_spans WHERE passage_id=? AND document_id=?",
                    (row["parent_id"],s["document_id"])).fetchall()
                if (source is None or source[0]!=s["document_hash"] or digest(s["text"])!=s["text_hash"]
                        or not any(p[0]<=s["line_start"]<=s["line_end"]<=p[1] for p in parent_spans)):
                    raise CitationValidationError("invalid evidence chunk source span")
                expected_url=("sigflow-note:///"+s["document_id"]) if row["content_type"]=="authored_note" else source[1]
                if s["source_url"]!=expected_url:
                    raise CitationValidationError("evidence locator does not identify its source document")
                if "github.com/" in expected_url and s.get("anchor")!=f"#L{s['line_start']}-L{s['line_end']}":
                    raise CitationValidationError("evidence line anchor mismatch")
            answer.append({**dict(row),"source_spans":spans,"index_version":index_version})
        return answer
    finally:connection.close()
