"""Explicit full-passage rebuild from the registered course database."""
from pathlib import Path
import json
import sqlite3
import time
import os
import tempfile
from contextlib import contextmanager

from sigflow_embedding import get_provider
from .database import open_database


@contextmanager
def new_index_file(output: Path):
    """Publish a completed SQLite without replacing any existing target, including races."""
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix='.gemma-', suffix='.sqlite', dir=output.parent)
    os.close(descriptor)
    temporary = Path(name)
    connection = sqlite3.connect(temporary)
    try:
        yield connection
        connection.commit()
        connection.close()
        if os.name == 'nt':
            os.rename(temporary, output)
        else:
            os.link(temporary, output)
    finally:
        connection.close()
        temporary.unlink(missing_ok=True)


def build_index(database: Path, model_path: Path, output: Path) -> dict:
    """Build a new index; never overwrite a previous model's index or course database."""
    if output.exists():
        raise FileExistsError("Choose a new index path; existing indexes are preserved")
    provider = get_provider(model_path)
    source = open_database(database)
    try:
        rows = source.execute(
            "SELECT passage_id, title, content, content_hash FROM passages "
            "WHERE evidence_status='grounded' ORDER BY passage_id"
        ).fetchall()
        version = source.execute(
            "SELECT value FROM index_meta WHERE key='index_version'"
        ).fetchone()[0]
    finally:
        source.close()
    started = time.perf_counter()
    records = []
    for row in rows:
        records.append({"unit_id": row["passage_id"],
                        "parent_passage_id": row["passage_id"], "title": row["title"],
                        "content": row["content"], "parent_content_hash": row["content_hash"]})
    vectors = provider.encode_documents(
        [row["content"] for row in records], [row["title"] for row in records],
    )
    manifest = {"course_index_version": version, "count": len(records),
                "embedding_fingerprint": provider.fingerprint,
                "build_seconds": time.perf_counter() - started}
    with new_index_file(output) as connection:
        with connection:
            connection.execute("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT)")
            connection.execute(
                "CREATE TABLE units (ordinal INTEGER PRIMARY KEY, unit_id TEXT, "
                "parent_id TEXT, record TEXT, vector BLOB)"
            )
            for key, value in (("encoder_contract", provider.contract), ("manifest", manifest)):
                connection.execute("INSERT INTO meta VALUES (?, ?)", (key, json.dumps(value)))
            for ordinal, (record, vector) in enumerate(zip(records, vectors)):
                connection.execute(
                    "INSERT INTO units VALUES (?, ?, ?, ?, ?)",
                    (ordinal, record["unit_id"], record["parent_passage_id"],
                     json.dumps(record, ensure_ascii=False), vector.astype("<f4").tobytes()),
                )
    return manifest
