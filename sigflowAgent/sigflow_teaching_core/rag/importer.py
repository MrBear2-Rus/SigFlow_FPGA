from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sqlite3
from uuid import uuid4
from contextlib import closing
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

try:
    import yaml
except ImportError as exc:  # pragma: no cover - covered by the install instructions
    raise RuntimeError("PyYAML is required; install the project with `python -m pip install -e .`.") from exc


from .database import DEFAULT_DB_PATH, RESOURCE_ROOT, SCHEMA_VERSION, _normalize_alias

DEFAULT_RESOURCES_DIR = RESOURCE_ROOT

SOURCE_TYPES = {"public_course", "tool_documentation", "team_notes", "practice_site"}
LANGUAGES = {"zh-CN", "en"}
DIFFICULTIES = {"intro", "intermediate", "advanced"}
CONTENT_TYPES = {"allowed_excerpt", "reviewed_summary", "authored_note"}
PASSAGE_ID_RE = re.compile(r"^[a-z][a-z0-9_-]*:[a-z][a-z0-9_-]*:[a-z0-9][a-z0-9_-]*$")
PRACTICE_ID_RE = PASSAGE_ID_RE
SHA256_RE = re.compile(r"^sha256:[0-9a-f]{64}$")


class ImportValidationError(ValueError):
    """Raised when a seed bundle is incomplete or inconsistent."""


def content_hash(content: str) -> str:
    return "sha256:" + hashlib.sha256(content.encode("utf-8")).hexdigest()


def _read_yaml(path: Path, key: str) -> list[dict[str, Any]]:
    try:
        parsed = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as exc:
        raise ImportValidationError(f"{path}: cannot read YAML: {exc}") from exc
    if not isinstance(parsed, dict) or not isinstance(parsed.get(key), list):
        raise ImportValidationError(f"{path}: expected a top-level `{key}` list")
    return parsed[key]


def _read_jsonl(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    records: list[dict[str, Any]] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise ImportValidationError(f"{path}: cannot read JSONL: {exc}") from exc
    for line_number, line in enumerate(lines, start=1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ImportValidationError(f"{path}:{line_number}: invalid JSON: {exc.msg}") from exc
        if not isinstance(row, dict):
            raise ImportValidationError(f"{path}:{line_number}: each JSONL row must be an object")
        records.append(row)
    return records


def _require_text(row: dict[str, Any], name: str, where: str) -> str:
    value = row.get(name)
    if not isinstance(value, str) or not value.strip():
        raise ImportValidationError(f"{where}: `{name}` must be a non-empty string")
    return value


def _validate_url(url: Any, where: str, base_url: str | list[str] | None = None) -> str:
    if not isinstance(url, str):
        raise ImportValidationError(f"{where}: URL must be a string")
    parsed = urlparse(url)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password:
        raise ImportValidationError(f"{where}: expected an absolute HTTPS URL without credentials")
    bases = [base_url] if isinstance(base_url, str) else base_url or []
    if bases:
        if not any(parsed.hostname == (base := urlparse(candidate)).hostname
                   and parsed.path.startswith(base.path) for candidate in bases):
            raise ImportValidationError(f"{where}: URL must stay under its source's base_url")
    return url


def _source_base_urls(source: dict[str, Any]) -> list[str]:
    additional = source.get("additional_base_urls", [])
    if not isinstance(additional, list) or not all(isinstance(value, str) for value in additional):
        raise ImportValidationError(f"source `{source.get('source_id', '?')}`: additional_base_urls must be a list of URLs")
    return [source["base_url"], *additional]


def _validate_unique(rows: list[dict[str, Any]], key: str, where: str) -> None:
    seen: set[str] = set()
    for index, row in enumerate(rows, start=1):
        value = row.get(key)
        if not isinstance(value, str) or not value:
            raise ImportValidationError(f"{where} row {index}: `{key}` must be a non-empty string")
        if value in seen:
            raise ImportValidationError(f"{where}: duplicate {key} `{value}`")
        seen.add(value)


def _validate_resources(resources_dir: Path) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]]]:
    sources = _read_yaml(resources_dir / "sources.yaml", "sources")
    concepts = _read_yaml(resources_dir / "concepts.yaml", "concepts")
    passages = _read_jsonl(resources_dir / "passages" / "ysyx.jsonl")
    passages.extend(_read_jsonl(resources_dir / "passages" / "ysyx_sections.jsonl"))
    passages.extend(_read_jsonl(resources_dir / "passages" / "team_notes.jsonl"))
    passages.extend(_read_jsonl(resources_dir / "external_official_passages.jsonl"))
    practices = _read_jsonl(resources_dir / "practices.jsonl")

    _validate_unique(sources, "source_id", "sources.yaml")
    _validate_unique(concepts, "concept_id", "concepts.yaml")
    _validate_unique(passages, "passage_id", "passages")
    _validate_unique(practices, "practice_id", "practices")

    source_map: dict[str, dict[str, Any]] = {}
    for row in sources:
        where = f"source `{row.get('source_id', '?')}`"
        for key in ("title", "source_type", "base_url", "author_or_source", "license_or_usage_note", "review_status", "retrieved_at"):
            _require_text(row, key, where)
        if row["source_type"] not in SOURCE_TYPES:
            raise ImportValidationError(f"{where}: unsupported source_type `{row['source_type']}`")
        _validate_url(row["base_url"], where)
        for base in _source_base_urls(row)[1:]:
            _validate_url(base, where)
        source_map[row["source_id"]] = row

    concept_map: dict[str, dict[str, Any]] = {}
    for row in concepts:
        where = f"concept `{row.get('concept_id', '?')}`"
        concept_id = _require_text(row, "concept_id", where)
        aliases = row.get("aliases")
        related = row.get("related", [])
        if not isinstance(aliases, list) or not aliases or not all(isinstance(a, str) and a.strip() for a in aliases):
            raise ImportValidationError(f"{where}: `aliases` must be a non-empty list of strings")
        normalized_aliases = [_normalize_alias(alias) for alias in aliases]
        if len(normalized_aliases) != len(set(normalized_aliases)):
            raise ImportValidationError(f"{where}: aliases contain duplicate normalized text")
        if not isinstance(related, list) or not all(isinstance(c, str) for c in related):
            raise ImportValidationError(f"{where}: `related` must be a list of concept IDs")
        concept_map[concept_id] = row
    for concept_id, row in concept_map.items():
        for related_id in row.get("related", []):
            if related_id not in concept_map:
                raise ImportValidationError(f"concept `{concept_id}`: unknown related concept `{related_id}`")

    for row in passages:
        passage_id = _require_text(row, "passage_id", "passage")
        where = f"passage `{passage_id}`"
        if not PASSAGE_ID_RE.fullmatch(passage_id):
            raise ImportValidationError(f"{where}: expected `<source>:<topic>:<semantic-name>`")
        for key in ("course_id", "course_version", "title", "section", "content", "content_type", "source_id", "author_or_source", "license_or_usage_note", "retrieved_at", "language", "difficulty"):
            _require_text(row, key, where)
        if row["content_type"] not in CONTENT_TYPES:
            raise ImportValidationError(f"{where}: unsupported content_type `{row['content_type']}`")
        if row["language"] not in LANGUAGES:
            raise ImportValidationError(f"{where}: unsupported language `{row['language']}`")
        if row["difficulty"] not in DIFFICULTIES:
            raise ImportValidationError(f"{where}: unsupported difficulty `{row['difficulty']}`")
        source = source_map.get(row["source_id"])
        if source is None:
            raise ImportValidationError(f"{where}: unknown source_id `{row['source_id']}`")
        _validate_url(row.get("source_url"), where, _source_base_urls(source))
        anchor = row.get("anchor")
        if anchor is not None and not isinstance(anchor, str):
            raise ImportValidationError(f"{where}: `anchor` must be a string or null")
        concept_ids = row.get("concept_ids")
        if not isinstance(concept_ids, list) or not concept_ids or not all(isinstance(c, str) for c in concept_ids):
            raise ImportValidationError(f"{where}: `concept_ids` must be a non-empty list of strings")
        for concept_id in concept_ids:
            if concept_id not in concept_map:
                raise ImportValidationError(f"{where}: unknown concept_id `{concept_id}`")
        supporting_sources = row.get("supporting_sources", [])
        if not isinstance(supporting_sources, list):
            raise ImportValidationError(f"{where}: `supporting_sources` must be a list")
        seen_supporting_refs = {(row["source_id"], row["source_url"], anchor)}
        for supporting_ref in supporting_sources:
            if not isinstance(supporting_ref, dict):
                raise ImportValidationError(f"{where}: each supporting source must be an object")
            supporting_id = _require_text(supporting_ref, "source_id", where)
            supporting_source = source_map.get(supporting_id)
            if supporting_source is None:
                raise ImportValidationError(f"{where}: unknown supporting source_id `{supporting_id}`")
            supporting_url = _validate_url(supporting_ref.get("source_url"), where, _source_base_urls(supporting_source))
            supporting_anchor = supporting_ref.get("anchor")
            if supporting_anchor is not None and not isinstance(supporting_anchor, str):
                raise ImportValidationError(f"{where}: supporting `anchor` must be a string or null")
            ref_key = (supporting_id, supporting_url, supporting_anchor)
            if ref_key in seen_supporting_refs:
                raise ImportValidationError(f"{where}: duplicate source reference `{supporting_url}`")
            seen_supporting_refs.add(ref_key)
        declared_hash = row.get("content_hash")
        if not isinstance(declared_hash, str) or not SHA256_RE.fullmatch(declared_hash):
            raise ImportValidationError(f"{where}: `content_hash` must be sha256:<64 lowercase hex digits>")
        calculated_hash = content_hash(row["content"])
        if declared_hash != calculated_hash:
            raise ImportValidationError(f"{where}: content_hash mismatch; expected `{calculated_hash}`")

    for row in practices:
        practice_id = _require_text(row, "practice_id", "practice")
        where = f"practice `{practice_id}`"
        if not PRACTICE_ID_RE.fullmatch(practice_id):
            raise ImportValidationError(f"{where}: expected `<source>:practice:<semantic-name>`")
        for key in ("title", "source", "language", "difficulty", "note"):
            _require_text(row, key, where)
        if row["source"] not in source_map:
            raise ImportValidationError(f"{where}: unknown source `{row['source']}`")
        _validate_url(row.get("source_url"), where, _source_base_urls(source_map[row["source"]]))
        if row["language"] not in LANGUAGES:
            raise ImportValidationError(f"{where}: unsupported language `{row['language']}`")
        if row["difficulty"] not in DIFFICULTIES:
            raise ImportValidationError(f"{where}: unsupported difficulty `{row['difficulty']}`")
        concept_ids = row.get("concept_ids")
        if not isinstance(concept_ids, list) or not concept_ids or not all(isinstance(c, str) for c in concept_ids):
            raise ImportValidationError(f"{where}: `concept_ids` must be a non-empty list of strings")
        for concept_id in concept_ids:
            if concept_id not in concept_map:
                raise ImportValidationError(f"{where}: unknown concept_id `{concept_id}`")

    return sources, concepts, passages, practices


def _select_fts_tokenizer() -> str:
    with closing(sqlite3.connect(":memory:")) as connection:
        try:
            connection.execute("CREATE VIRTUAL TABLE temp.fts_probe USING fts5(text, tokenize='trigram')")
        except sqlite3.OperationalError as exc:
            raise RuntimeError("Course retrieval requires SQLite FTS5 with the trigram tokenizer") from exc
    return "trigram"


def _concept_text(concept_ids: list[str], concept_map: dict[str, dict[str, Any]]) -> str:
    values: list[str] = []
    for concept_id in concept_ids:
        values.append(concept_id)
        values.extend(concept_map[concept_id]["aliases"])
    return " ".join(dict.fromkeys(values))


def _create_schema(connection: sqlite3.Connection, tokenizer: str) -> None:
    connection.executescript(
        """
        PRAGMA foreign_keys = ON;
        CREATE TABLE sources (
            source_id TEXT PRIMARY KEY,
            title TEXT NOT NULL,
            source_type TEXT NOT NULL,
            base_url TEXT NOT NULL,
            author_or_source TEXT NOT NULL,
            license_or_usage_note TEXT NOT NULL,
            review_status TEXT NOT NULL,
            retrieved_at TEXT NOT NULL
        );
        CREATE TABLE concepts (
            concept_id TEXT PRIMARY KEY,
            related_json TEXT NOT NULL
        );
        CREATE TABLE concept_aliases (
            concept_id TEXT NOT NULL REFERENCES concepts(concept_id) ON DELETE CASCADE,
            alias TEXT NOT NULL,
            alias_norm TEXT NOT NULL,
            PRIMARY KEY (concept_id, alias_norm)
        );
        CREATE INDEX concept_aliases_norm_idx ON concept_aliases(alias_norm);
        CREATE TABLE passages (
            passage_id TEXT PRIMARY KEY,
            course_id TEXT NOT NULL,
            course_version TEXT NOT NULL,
            title TEXT NOT NULL,
            section TEXT NOT NULL,
            concept_ids_json TEXT NOT NULL,
            content TEXT NOT NULL,
            content_type TEXT NOT NULL,
            reviewed_summary TEXT NOT NULL,
            summary_hash TEXT NOT NULL,
            evidence_status TEXT NOT NULL CHECK (evidence_status IN ('grounded', 'partial')),
            evidence_note TEXT,
            source_id TEXT NOT NULL REFERENCES sources(source_id),
            source_url TEXT NOT NULL,
            anchor TEXT,
            author_or_source TEXT NOT NULL,
            license_or_usage_note TEXT NOT NULL,
            retrieved_at TEXT NOT NULL,
            content_hash TEXT NOT NULL,
            language TEXT NOT NULL,
            difficulty TEXT NOT NULL
        );
        CREATE TABLE passage_sources (
            passage_id TEXT NOT NULL REFERENCES passages(passage_id) ON DELETE CASCADE,
            reference_order INTEGER NOT NULL,
            source_id TEXT NOT NULL REFERENCES sources(source_id),
            source_url TEXT NOT NULL,
            anchor TEXT,
            reference_kind TEXT NOT NULL CHECK (reference_kind IN ('primary', 'supporting')),
            PRIMARY KEY (passage_id, reference_order)
        );
        CREATE INDEX passage_sources_source_idx ON passage_sources(source_id, passage_id);
        CREATE TABLE source_documents (
            document_id TEXT PRIMARY KEY,
            chapter TEXT NOT NULL,
            source_id TEXT NOT NULL REFERENCES sources(source_id),
            source_url TEXT NOT NULL,
            title TEXT NOT NULL,
            document_hash TEXT NOT NULL,
            retrieved_at TEXT NOT NULL
        );
        CREATE TABLE passage_spans (
            passage_id TEXT NOT NULL REFERENCES passages(passage_id) ON DELETE CASCADE,
            span_order INTEGER NOT NULL,
            document_id TEXT NOT NULL REFERENCES source_documents(document_id),
            anchor TEXT,
            line_start INTEGER NOT NULL,
            line_end INTEGER NOT NULL,
            heading_path_json TEXT NOT NULL,
            text TEXT NOT NULL,
            text_hash TEXT NOT NULL,
            character_count INTEGER NOT NULL,
            source_text_hash TEXT NOT NULL,
            normalization TEXT,
            PRIMARY KEY (passage_id, span_order)
        );
        CREATE TABLE passage_concepts (
            passage_id TEXT NOT NULL REFERENCES passages(passage_id) ON DELETE CASCADE,
            concept_id TEXT NOT NULL REFERENCES concepts(concept_id),
            PRIMARY KEY (passage_id, concept_id)
        );
        CREATE INDEX passage_concepts_concept_idx ON passage_concepts(concept_id, passage_id);
        CREATE TABLE practices (
            practice_id TEXT PRIMARY KEY,
            title TEXT NOT NULL,
            source TEXT NOT NULL REFERENCES sources(source_id),
            source_url TEXT NOT NULL,
            language TEXT NOT NULL,
            difficulty TEXT NOT NULL,
            note TEXT NOT NULL
        );
        CREATE TABLE practice_concepts (
            practice_id TEXT NOT NULL REFERENCES practices(practice_id) ON DELETE CASCADE,
            concept_id TEXT NOT NULL REFERENCES concepts(concept_id),
            PRIMARY KEY (practice_id, concept_id)
        );
        CREATE INDEX practice_concepts_concept_idx ON practice_concepts(concept_id, practice_id);
        CREATE TABLE index_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
        CREATE TABLE source_file_provenance (
            source_path TEXT PRIMARY KEY,
            repository_path TEXT NOT NULL,
            source_id TEXT NOT NULL REFERENCES sources(source_id),
            repository TEXT NOT NULL,
            version TEXT NOT NULL,
            commit_sha TEXT NOT NULL,
            source_url TEXT NOT NULL,
            retrieved_at TEXT NOT NULL,
            sha256 TEXT NOT NULL,
            byte_count INTEGER NOT NULL,
            line_count INTEGER NOT NULL,
            license_id TEXT NOT NULL,
            license_basis TEXT NOT NULL,
            license_evidence_path TEXT NOT NULL,
            license_note TEXT NOT NULL,
            indexed INTEGER NOT NULL CHECK (indexed IN (0, 1)),
            review_status TEXT NOT NULL
        );
        CREATE TABLE passage_mappings (
            passage_id TEXT PRIMARY KEY REFERENCES passages(passage_id) ON DELETE CASCADE,
            title_zh TEXT NOT NULL,
            terms_json TEXT NOT NULL,
            questions_json TEXT NOT NULL,
            concept_ids_json TEXT NOT NULL,
            review_status TEXT NOT NULL
        );
        CREATE TABLE passage_translations (
            translation_id TEXT PRIMARY KEY,
            passage_id TEXT NOT NULL REFERENCES passages(passage_id) ON DELETE CASCADE,
            source_content_hash TEXT NOT NULL,
            translated_text TEXT NOT NULL,
            generation_method TEXT NOT NULL,
            review_status TEXT NOT NULL,
            created_at TEXT NOT NULL
        );
        CREATE INDEX passage_translations_passage_idx ON passage_translations(passage_id);
        CREATE TABLE evidence_chunks (
            chunk_id TEXT PRIMARY KEY,
            parent_id TEXT NOT NULL REFERENCES passages(passage_id) ON DELETE CASCADE,
            ordinal INTEGER NOT NULL,
            content TEXT NOT NULL,
            content_hash TEXT NOT NULL,
            spans_json TEXT NOT NULL,
            indivisible_over_target INTEGER NOT NULL,
            UNIQUE(parent_id, ordinal)
        );
        CREATE INDEX evidence_chunks_parent_idx ON evidence_chunks(parent_id);
        """
    )
    connection.execute(
        "CREATE VIRTUAL TABLE passage_fts USING fts5("
        "passage_id UNINDEXED, title, section, content, reviewed_summary, concept_text, "
        f"tokenize='{tokenizer}')"
    )


def _write_index_file(
    path: Path,
    sources: list[dict[str, Any]],
    concepts: list[dict[str, Any]],
    passages: list[dict[str, Any]],
    practices: list[dict[str, Any]],
    documents: list[dict[str, Any]],
    evidence: dict[str, dict[str, Any]],
    tokenizer: str,
    index_version: str,
    source_files: list[dict[str, Any]] | tuple = (),
    mappings: list[dict[str, Any]] | tuple = (),
    translations: list[dict[str, Any]] | tuple = (),
) -> None:
    concept_map = {row["concept_id"]: row for row in concepts}
    source_map = {row["source_id"]: row for row in sources}
    connection = sqlite3.connect(path)
    connection.row_factory = sqlite3.Row
    try:
        _create_schema(connection, tokenizer)
        connection.execute(f"PRAGMA user_version = {SCHEMA_VERSION}")
        connection.executemany(
            "INSERT INTO sources VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            [tuple(row[key] for key in ("source_id", "title", "source_type", "base_url", "author_or_source", "license_or_usage_note", "review_status", "retrieved_at")) for row in sources],
        )
        connection.executemany(
            "INSERT INTO source_file_provenance VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            [(row["source_path"], row["repository_path"], row["source_id"], row["repository"], row["version"], row["commit"],
              row["source_url"], row["retrieved_at"], row["sha256"], row["bytes"], row["line_count"],
              row["license_id"], row["license_basis"], row["license_evidence_path"], row["license_note"], int(bool(row["indexed"])),
              row["review_status"]) for row in source_files],
        )
        connection.executemany(
            "INSERT INTO concepts(concept_id, related_json) VALUES (?, ?)",
            [(row["concept_id"], json.dumps(row.get("related", []), ensure_ascii=False)) for row in concepts],
        )
        alias_rows = []
        for row in concepts:
            for alias in row["aliases"]:
                alias_rows.append((row["concept_id"], alias, _normalize_alias(alias)))
        connection.executemany("INSERT INTO concept_aliases VALUES (?, ?, ?)", alias_rows)
        connection.executemany(
            "INSERT INTO source_documents VALUES (?, ?, ?, ?, ?, ?, ?)",
            [(d["document_id"], d["chapter"], d["source_id"], d["source_url"], d["title"],
              d["document_hash"], d["retrieved_at"]) for d in documents],
        )

        for row in passages:
            source = evidence[row["passage_id"]]
            connection.execute(
                "INSERT INTO passages VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    row["passage_id"], row["course_id"], row["course_version"], row["title"], row["section"],
                    json.dumps(row["concept_ids"], ensure_ascii=False), source["content"],
                    "authored_note" if row["content_type"] == "authored_note" else "source_excerpt",
                    row["content"], row["content_hash"], source["evidence_status"], source["evidence_note"],
                    row["source_id"], row["source_url"], row.get("anchor"),
                    row.get("author_or_source", source_map[row["source_id"]]["author_or_source"]),
                    row.get("license_or_usage_note", source_map[row["source_id"]]["license_or_usage_note"]),
                    row["retrieved_at"], source["content_hash"], row["language"], row["difficulty"],
                ),
            )
            connection.executemany(
                "INSERT INTO passage_spans VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                [(row["passage_id"], s["span_order"], s["document_id"], s["anchor"], s["line_start"],
                  s["line_end"], json.dumps(s["heading_path"], ensure_ascii=False), s["text"],
                  s["text_hash"], s["character_count"], s.get("source_text_hash", s["text_hash"]),
                  s.get("normalization")) for s in source["spans"]],
            )
            connection.executemany(
                "INSERT INTO passage_concepts VALUES (?, ?)",
                [(row["passage_id"], concept_id) for concept_id in row["concept_ids"]],
            )
            source_refs = [(row["source_id"], row["source_url"], row.get("anchor"), "primary")]
            source_refs.extend(
                (ref["source_id"], ref["source_url"], ref.get("anchor"), "supporting")
                for ref in row.get("supporting_sources", [])
            )
            connection.executemany(
                "INSERT INTO passage_sources VALUES (?, ?, ?, ?, ?, ?)",
                [
                    (row["passage_id"], order, source_id, source_url, anchor, reference_kind)
                    for order, (source_id, source_url, anchor, reference_kind) in enumerate(source_refs)
                ],
            )
            connection.execute(
                "INSERT INTO passage_fts(passage_id, title, section, content, reviewed_summary, concept_text) VALUES (?, ?, ?, ?, ?, ?)",
                (row["passage_id"], row["title"], row["section"], source["content"], row.get("search_text", row["content"]),
                 _concept_text(row["concept_ids"], concept_map)),
            )
            from .chunks import build_chunks
            parent = {**row, "content": source["content"],
                      "content_type": "authored_note" if row["content_type"] == "authored_note" else "source_excerpt"}
            for chunk in build_chunks(parent, source["spans"], {d["document_id"]: d for d in documents}):
                connection.execute("INSERT INTO evidence_chunks VALUES (?, ?, ?, ?, ?, ?, ?)",
                    (chunk["chunk_id"], chunk["parent_id"], chunk["ordinal"], chunk["content"],
                     chunk["content_hash"], json.dumps(chunk["source_spans"],ensure_ascii=False),
                     int(chunk["indivisible_over_target"])))
        connection.executemany(
            "INSERT INTO passage_mappings VALUES (?, ?, ?, ?, ?, ?)",
            [(row["passage_id"], row["title_zh"], json.dumps(row["terms"], ensure_ascii=False),
              json.dumps(row["questions_zh"], ensure_ascii=False), json.dumps(row["concept_ids"], ensure_ascii=False),
              row.get("review_status", "repository_mapping_unreviewed")) for row in mappings],
        )
        connection.executemany(
            "INSERT INTO passage_translations VALUES (?, ?, ?, ?, ?, ?, ?)",
            [(row["translation_id"], row["passage_id"], row["source_content_hash"], row["translated_text"],
              row["generation_method"], row["review_status"], row["created_at"]) for row in translations],
        )
        for row in practices:
            connection.execute(
                "INSERT INTO practices VALUES (?, ?, ?, ?, ?, ?, ?)",
                (row["practice_id"], row["title"], row["source"], row["source_url"], row["language"], row["difficulty"], row["note"]),
            )
            connection.executemany(
                "INSERT INTO practice_concepts VALUES (?, ?)",
                [(row["practice_id"], concept_id) for concept_id in row["concept_ids"]],
            )
        connection.executemany(
            "INSERT INTO index_meta(key, value) VALUES (?, ?)",
            [("fts_tokenizer", tokenizer), ("retriever", "sqlite-fts5"), ("index_version", index_version),
             ("evidence_format", "source-units-v1")],
        )
        connection.commit()
        connection.execute("PRAGMA optimize")
    finally:
        connection.close()


def refresh_content_hashes(resources_dir: Path = DEFAULT_RESOURCES_DIR) -> int:
    changed = 0
    for path in sorted((Path(resources_dir) / "passages").glob("*.jsonl")):
        rows = _read_jsonl(path)
        for row in rows:
            calculated = content_hash(row["content"])
            if row.get("content_hash") != calculated:
                row["content_hash"] = calculated
                changed += 1
        path.write_text("".join(json.dumps(row, ensure_ascii=False) + "\n" for row in rows), encoding="utf-8")
    return changed


def build_index(resources_dir: Path = DEFAULT_RESOURCES_DIR,
                db_path: Path = DEFAULT_DB_PATH, *, snapshot_dir: Path) -> dict[str, Any]:
    """Build one current database, replacing it atomically; no history or rollback."""
    resources_dir, db_path = Path(resources_dir), Path(db_path).resolve()
    sources, concepts, registered, practices = _validate_resources(resources_dir)
    from .source_evidence import load_source_evidence
    documents, evidence = load_source_evidence(registered, resources_dir, Path(snapshot_dir))
    passages = [p for p in registered if evidence[p["passage_id"]]["evidence_status"] == "grounded"]
    source_files = _read_jsonl(resources_dir / "external_source_files.jsonl")
    mappings = _read_jsonl(resources_dir / "external_official_mappings.jsonl")
    translations = _read_jsonl(resources_dir / "external_official_translations.jsonl")
    source_map = {row["source_id"]: row for row in sources}
    for index, row in enumerate(source_files, start=1):
        where = f"official source file row {index}"
        for key in ("source_path", "repository_path", "source_id", "repository", "version", "commit", "source_url", "retrieved_at",
                    "sha256", "license_id", "license_basis", "license_evidence_path", "license_note", "review_status"):
            _require_text(row, key, where)
        if row["source_id"] not in source_map or not SHA256_RE.fullmatch(row["sha256"]):
            raise ImportValidationError(f"{where}: unknown source or invalid SHA-256")
        if (type(row.get("bytes")) is not int or row["bytes"] < 0 or type(row.get("line_count")) is not int
                or row["line_count"] < 1 or type(row.get("indexed")) is not bool):
            raise ImportValidationError(f"{where}: invalid size, line count, or indexed flag")
        _validate_url(row["source_url"], where, _source_base_urls(source_map[row["source_id"]]))
    if len({row["source_path"] for row in source_files}) != len(source_files):
        raise ImportValidationError("external source file paths must be unique")
    source_paths = {row["source_path"] for row in source_files}
    if any(row["license_evidence_path"] not in source_paths for row in source_files):
        raise ImportValidationError("official source license evidence path is absent from the fixed source list")
    official_ids = {row["passage_id"] for row in passages if row["source_id"] in {"lowrisc_style", "yosys_047", "verilator_5052"}
                    and row["passage_id"] not in {p["passage_id"] for p in _read_jsonl(resources_dir / "passages" / "team_notes.jsonl")}}
    mapping_ids = set()
    for index, row in enumerate(mappings, start=1):
        where = f"official mapping row {index}"
        passage_id = _require_text(row, "passage_id", where)
        if passage_id not in official_ids or passage_id in mapping_ids:
            raise ImportValidationError(f"{where}: unknown or duplicate passage_id")
        mapping_ids.add(passage_id)
        _require_text(row, "title_zh", where)
        for key in ("terms", "questions_zh", "concept_ids"):
            if not isinstance(row.get(key), list) or not all(isinstance(value, str) and value.strip() for value in row[key]):
                raise ImportValidationError(f"{where}: `{key}` must be a non-empty list of strings")
    if mapping_ids != official_ids:
        raise ImportValidationError("every official passage must have exactly one Chinese retrieval mapping")
    for index, row in enumerate(translations, start=1):
        where = f"official translation row {index}"
        for key in ("translation_id", "passage_id", "source_content_hash", "translated_text", "generation_method", "review_status", "created_at"):
            _require_text(row, key, where)
        passage_id = row["passage_id"]
        if passage_id not in evidence or row["source_content_hash"] != evidence[passage_id]["content_hash"]:
            raise ImportValidationError(f"{where}: translation source ID/hash does not match its official passage")
    if len({row["translation_id"] for row in translations}) != len(translations):
        raise ImportValidationError("translation IDs must be unique")
    document_ids = {span["document_id"] for p in passages for span in evidence[p["passage_id"]]["spans"]}
    documents = [d for d in documents if d["document_id"] in document_ids]
    from .chunks import build_chunks
    document_map = {d["document_id"]: d for d in documents}
    chunk_material = []
    for p in passages:
        parent = {**p, "content": evidence[p["passage_id"]]["content"],
                  "content_type": "authored_note" if p["content_type"]=="authored_note" else "source_excerpt"}
        chunk_material.extend(build_chunks(parent, evidence[p["passage_id"]]["spans"], document_map))
    # Keep aliases for unsupported concepts: asking explicitly about those must still yield no_match.
    material = {"evidence_format": "source-units-v1", "evidence_chunks": chunk_material,
                "sources": sources, "source_files": source_files, "mappings": mappings,
                "translations": translations, "concepts": concepts, "practices": practices,
                "passages": passages,
                "evidence": [{"passage_id": p["passage_id"], **evidence[p["passage_id"]]} for p in passages],
                "documents": [{k: v for k, v in d.items() if k not in {"lines", "html_ids", "local_path"}} for d in documents]}
    for item in material["evidence"]:
        item["spans"] = [{k: v for k, v in span.items() if k != "local_path"} for span in item["spans"]]
    index_version = content_hash(json.dumps(material, sort_keys=True, ensure_ascii=False, separators=(",", ":")))
    tokenizer = _select_fts_tokenizer()
    db_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = db_path.with_name("." + db_path.name + "." + uuid4().hex + ".tmp")
    try:
        _write_index_file(temporary, sources, concepts, passages, practices, documents, evidence,
                          tokenizer, index_version, source_files, mappings, translations)
        with closing(sqlite3.connect(temporary)) as connection:
            connection.execute("INSERT INTO passage_fts(passage_fts) VALUES ('optimize')")
            connection.commit()
            connection.execute("VACUUM")
            if connection.execute("PRAGMA integrity_check").fetchone()[0] != "ok" or connection.execute("PRAGMA foreign_key_check").fetchone():
                raise RuntimeError("course_database_integrity_failure")
        os.replace(temporary, db_path)
    finally:
        temporary.unlink(missing_ok=True)
    return {"index_version": index_version, "schema_version": SCHEMA_VERSION,
            "registered_passage_count": len(registered), "passage_count": len(passages),
            "excluded_partial_count": len(registered) - len(passages),
            "practice_count": len(practices), "official_source_file_count": len(source_files),
            "official_mapping_count": len(mappings), "official_translation_count": len(translations),
            "fts_tokenizer": tokenizer,
            "database_path": str(db_path), "database_bytes": db_path.stat().st_size}


def main() -> None:
    parser = argparse.ArgumentParser(description="Build the single current course database")
    parser.add_argument("--resources", type=Path, default=DEFAULT_RESOURCES_DIR)
    parser.add_argument("--db", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--snapshot-dir", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build_index(args.resources, args.db, snapshot_dir=args.snapshot_dir), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
