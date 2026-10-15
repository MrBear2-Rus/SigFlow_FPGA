"""One current read-only course database; older schemas are deliberately rejected."""
from pathlib import Path
import os
import re
import sqlite3

PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DB_PATH = PROJECT_ROOT / 'data' / 'course.sqlite'
RESOURCE_ROOT = Path(__file__).resolve().parent / 'resources' / 'courses'
SCHEMA_VERSION = 4


def _normalize_alias(value: str) -> str:
    return re.sub(r'[^a-z0-9\u3400-\u9fff]+', '', value.casefold())


def configured_database() -> Path:
    return Path(os.environ.get('SIGFLOW_COURSE_DB') or DEFAULT_DB_PATH).expanduser().resolve()


def open_database(db_path: Path = DEFAULT_DB_PATH) -> sqlite3.Connection:
    path = Path(db_path).expanduser().resolve()
    if not path.is_file():
        raise FileNotFoundError(f'Course database not found: {path}')
    connection = sqlite3.connect(path.as_uri() + '?mode=ro', uri=True)
    connection.row_factory = sqlite3.Row
    try:
        connection.execute('PRAGMA query_only = ON')
        connection.execute('BEGIN')
        if connection.execute('PRAGMA user_version').fetchone()[0] != SCHEMA_VERSION:
            raise ValueError('unsupported_course_schema: rebuild the database with the current importer')
        metadata = dict(connection.execute('SELECT key, value FROM index_meta'))
        if (metadata.get('fts_tokenizer') != 'trigram'
                or not re.fullmatch(r'sha256:[0-9a-f]{64}', metadata.get('index_version', ''))):
            raise ValueError('invalid_course_metadata: rebuild the current database')
    except Exception:
        connection.close()
        raise
    return connection
