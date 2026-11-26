"""UA-09 public API: historical lexical retrieval, Gemma dense, and fixed RRF."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import sqlite3
from threading import RLock
from typing import Literal

from .citations import CitationValidationError, render_citations
from .database import open_database
from .retriever import retrieve_course_candidates

Mode = Literal["fts5", "gemma_dense", "gemma_hybrid"]
RRF_K = 60
CANDIDATE_LIMIT = 20


@dataclass(frozen=True)
class RetrievalConfig:
    course_db: Path
    model_path: Path | None = None
    vector_index: Path | None = None
    mode: Mode = "gemma_dense"


def search_lexical(
    query: str, database: Path, *, connection: sqlite3.Connection | None = None,
) -> list[dict]:
    """Preserve the original concept/alias/identifier and FTS5 semantics."""
    result = retrieve_course_candidates(query, limit=CANDIDATE_LIMIT, db_path=database,
                                        _connection=connection)
    return [{"passage_id": row["passage_id"], "score": row["ranking_score"]}
            for row in result["passages"]]


class KnowledgeSearch:
    """Own read-only course vectors; share the model, never student or course state."""

    def __init__(self, config: RetrievalConfig):
        self.config = config
        self._dense = None
        self._index_lock = RLock()

    def _load_index(self):
        with self._index_lock:
            if self._dense is None:
                self._read_index()

    def _read_index(self):
        import numpy as np
        from sigflow_embedding import get_provider

        if self.config.model_path is None or self.config.vector_index is None:
            raise ValueError("Dense mode requires explicit model and vector index paths")
        provider = get_provider(self.config.model_path)
        connection = sqlite3.connect(
            self.config.vector_index.resolve().as_uri() + "?mode=ro", uri=True,
        )
        try:
            metadata = dict(connection.execute("SELECT key, value FROM meta"))
            contract = json.loads(metadata["encoder_contract"])
            if contract.get("contract_version") != 2:
                raise ValueError("Legacy index: explicitly verify and migrate to a new contract-v2 file")
            for key, value in provider.contract.items():
                if contract.get(key) != value:
                    raise ValueError("Embedding index contract mismatch: " + key)
            rows = connection.execute("SELECT record, vector FROM units ORDER BY ordinal")
            records, vectors = [], []
            for record, vector in rows:
                records.append(json.loads(record))
                vectors.append(np.frombuffer(vector, dtype="<f4"))
        finally:
            connection.close()
        matrix = np.stack(vectors)
        ids = [record["parent_passage_id"] for record in records]
        if len(ids) != len(set(ids)):
            raise ValueError("Duplicate passage IDs in vector index")
        if matrix.shape != (len(records), 768) or not np.isfinite(matrix).all():
            raise ValueError("Invalid index dimensions or values")
        if not np.allclose(np.linalg.norm(matrix, axis=1), 1, atol=1e-4):
            raise ValueError("Index vectors must be L2 normalized")
        self._dense = provider, records, matrix

    def validate_configuration(self) -> dict:
        """Check index metadata and corpus before accepting a development U2 request."""
        if self.config.mode != "fts5" and self._dense is None:
            self._load_index()
        return {"index_version": self._validate_corpus(), "mode": self.config.mode}

    def _validate_corpus(self, connection: sqlite3.Connection | None = None) -> str:
        """Fail on edits/deletions/additions, even after vectors have been cached."""
        owns_connection = connection is None
        connection = connection if connection is not None else open_database(self.config.course_db)
        try:
            version = connection.execute(
                "SELECT value FROM index_meta WHERE key='index_version'"
            ).fetchone()[0]
            live = {}
            for row in connection.execute(
                "SELECT passage_id, content_hash, title FROM passages "
                "WHERE evidence_status='grounded'"
            ):
                live[row["passage_id"]] = (row["content_hash"], row["title"])
        finally:
            if owns_connection:
                connection.close()
        if self._dense is not None:
            indexed = {}
            for record in self._dense[1]:
                if record["unit_id"] != record["parent_passage_id"]:
                    raise ValueError("This API requires full-passage vectors")
                digest = "sha256:" + hashlib.sha256(record["content"].encode()).hexdigest()
                if digest != record["parent_content_hash"]:
                    raise ValueError("Corrupt indexed text")
                indexed[record["parent_passage_id"]] = (digest, record["title"])
            if live != indexed:
                raise ValueError("Course corpus changed: rebuild a separate Gemma index")
        return version

    def search_dense(self, query: str, *, connection: sqlite3.Connection | None = None) -> list[dict]:
        import numpy as np

        if self._dense is None:
            self._load_index()
        self._validate_corpus(connection)
        provider, records, matrix = self._dense
        vector = provider.encode_queries([query])[0]
        similarities = matrix @ vector
        order = np.argsort(-similarities, kind="stable")[:CANDIDATE_LIMIT]
        results = []
        for position in order:
            results.append({"passage_id": records[position]["parent_passage_id"],
                            "score": float(similarities[position])})
        return results

    def retrieve(self, query: str, top_k: int = 5, mode: Mode | None = None) -> dict:
        """Return citation-validated candidates; retrieval never claims answer sufficiency."""
        mode = mode or self.config.mode
        if mode not in {"fts5", "gemma_dense", "gemma_hybrid"}:
            raise ValueError("Unknown retrieval mode")
        if not isinstance(query, str) or not query.strip():
            raise ValueError("Query must be nonempty")
        if type(top_k) is not int or not 1 <= top_k <= CANDIDATE_LIMIT:
            raise ValueError("top_k must be in 1..20")
        connection = open_database(self.config.course_db)
        try:
            return self._retrieve_snapshot(query, top_k, mode, connection)
        finally:
            connection.close()

    def _retrieve_snapshot(
        self, query: str, top_k: int, mode: Mode, connection: sqlite3.Connection,
    ) -> dict:
        """All course reads, including lexical scoring, share this one read transaction."""
        lexical, dense = [], []
        if mode in {"fts5", "gemma_hybrid"}:
            lexical = search_lexical(query, self.config.course_db, connection=connection)
        if mode in {"gemma_dense", "gemma_hybrid"}:
            dense = self.search_dense(query, connection=connection)
        version = self._validate_corpus(connection)
        if mode == "gemma_hybrid":
            ranked = fuse_results(lexical, dense)
        else:
            ranked = lexical if mode == "fts5" else dense
        evidence = build_evidence(ranked[:top_k], version, self.config.course_db,
                                   connection=connection)
        for passage in evidence:
            passage["retrieval_method"] = mode
        return {"query": query, "mode": mode, "index_version": version,
                "status": "ok" if evidence else "no_match", "passages": evidence,
                "channels": {"fts5": lexical, "gemma_dense": dense},
                "answerability": "not_assessed"}


def fuse_results(lexical: list[dict], dense: list[dict]) -> list[dict]:
    """RRF sums 1/(60 + one-based rank); ties use passage ID as in the experiment."""
    scores: dict[str, float] = {}
    for channel in (lexical, dense):
        for rank, candidate in enumerate(channel, 1):
            passage_id = candidate["passage_id"]
            scores[passage_id] = scores.get(passage_id, 0) + 1 / (RRF_K + rank)
    ordered = sorted(scores, key=lambda passage_id: (-scores[passage_id], passage_id))
    return [{"passage_id": passage_id, "score": scores[passage_id]} for passage_id in ordered]


def build_evidence(
    ranked: list[dict], version: str, database: Path, *, connection: sqlite3.Connection | None = None,
) -> list[dict]:
    ids = [row["passage_id"] for row in ranked]
    if len(ids) != len(set(ids)):
        raise CitationValidationError("Duplicate candidate IDs")
    owns_connection = connection is None
    connection = connection if connection is not None else open_database(database)
    try:
        current = connection.execute("SELECT value FROM index_meta WHERE key='index_version'").fetchone()[0]
        if current != version:
            raise CitationValidationError('Course snapshot version mismatch')
        citations = render_citations(
            ids, {"passages": ranked, "index_version": version},
            db_path=database, connection=connection,
        )
        by_id = {row["passage_id"]: row for row in citations}
        if len(citations) != len(ids) or set(by_id) != set(ids):
            raise CitationValidationError("Citation IDs/count do not match candidates")
        evidence = []
        for candidate in ranked:
            citation = by_id[candidate["passage_id"]]
            if not citation["source_spans"]:
                raise CitationValidationError("Missing registered source document")
            row = connection.execute(
                "SELECT content, evidence_status, source_id FROM passages WHERE passage_id=?",
                (candidate["passage_id"],),
            ).fetchone()
            if row is None or row["evidence_status"] != "grounded":
                raise CitationValidationError("Only grounded registered passages may be returned")
            digest = "sha256:" + hashlib.sha256(row["content"].encode()).hexdigest()
            if digest != citation["content_hash"] or row["source_id"] != citation["source_id"]:
                raise CitationValidationError("Passage content/source does not match citation")
            validate_parent_spans(connection, candidate["passage_id"], row["content"], citation)
            evidence.append({**citation, "content": row["content"],
                             "score": candidate["score"], "index_version": version})
        return evidence
    finally:
        if owns_connection:
            connection.close()


def validate_parent_spans(
    connection: sqlite3.Connection, passage_id: str, content: str, citation: dict,
) -> None:
    """Parent quotes must cover exactly the returned text in this same SQLite snapshot."""
    primary = connection.execute(
        "SELECT 1 FROM passage_sources WHERE passage_id=? AND source_id=? "
        "AND reference_kind='primary'", (passage_id, citation['source_id']),
    ).fetchone()
    if primary is None:
        raise CitationValidationError('Passage primary source does not match registered reference')
    spans = connection.execute(
        "SELECT ps.*, sd.source_id AS document_source, sd.document_hash "
        "FROM passage_spans ps LEFT JOIN source_documents sd ON sd.document_id=ps.document_id "
        "WHERE ps.passage_id=? ORDER BY ps.span_order", (passage_id,),
    ).fetchall()
    if not spans or "\n\n".join(span["text"] for span in spans) != content:
        raise CitationValidationError("Passage text does not match registered source spans")
    if len(spans) != len(citation['source_spans']):
        raise CitationValidationError('Source span count mismatch')
    allowed = {citation['source_id']} | {row['source_id'] for row in citation['source_references']}
    for span, locator in zip(spans, citation['source_spans']):
        digest = "sha256:" + hashlib.sha256(span["text"].encode()).hexdigest()
        if digest != span["text_hash"] or not 1 <= span["line_start"] <= span["line_end"]:
            raise CitationValidationError("Invalid source span hash/line range")
        if span['document_source'] not in allowed:
            raise CitationValidationError('Source document is outside registered passage references')
        for key in ('document_id', 'document_hash', 'text_hash', 'line_start', 'line_end'):
            if span[key] != locator[key]:
                raise CitationValidationError('Citation locator does not match registered span')
