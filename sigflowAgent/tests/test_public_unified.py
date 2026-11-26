"""Public synthetic fixtures: no model, private benchmark, network or experiment paths."""
from contextlib import closing
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch

from sigflow_teaching_core.rag import unified
from sigflow_teaching_core.rag.citations import CitationValidationError
from sigflow_teaching_core.rag.database import open_database
from sigflow_teaching_core.rag.importer import _create_schema
from sigflow_teaching_core.rag.gemma_index import new_index_file
from sigflow_embedding.provider import GemmaProvider, RUNTIME_VERSIONS, validate_runtime_versions


def digest(text):
    return 'sha256:' + hashlib.sha256(text.encode()).hexdigest()


def insert(connection, table, values):
    columns = connection.execute('PRAGMA table_info(' + table + ')').fetchall()
    record = {row[1]: values.get(row[1], 'fixture') for row in columns}
    connection.execute('INSERT INTO ' + table + ' VALUES (' + ','.join('?' for _ in record) + ')',
                       tuple(record.values()))


def fixture_database(path):
    with closing(sqlite3.connect(path)) as connection:
        _create_schema(connection, 'trigram')
        connection.execute('PRAGMA user_version=4')
        connection.executemany('INSERT INTO index_meta VALUES (?, ?)',
                               [('fts_tokenizer', 'trigram'), ('index_version', digest('fixture'))])
        insert(connection, 'sources', {'source_id': 'fixture', 'source_type': 'public_course',
                                      'base_url': 'https://example.org'})
        insert(connection, 'concepts', {'concept_id': 'latch', 'related_json': '[]'})
        insert(connection, 'concept_aliases', {'concept_id': 'latch', 'alias': 'latch', 'alias_norm': 'latch'})
        for suffix in ('a', 'b'):
            pid = 'fixture:' + suffix
            text = 'A latch retains its old value. ' + suffix
            insert(connection, 'passages', {
                'passage_id': pid, 'source_id': 'fixture', 'title': 'latch', 'section': 'latches',
                'course_id': 'fixture', 'language': 'en', 'concept_ids_json': '["latch"]',
                'content': text, 'content_hash': digest(text), 'content_type': 'source_excerpt',
                'reviewed_summary': '', 'summary_hash': digest(''), 'evidence_status': 'grounded',
                'source_url': 'https://example.org/latch', 'anchor': '#L1',
            })
            insert(connection, 'passage_concepts', {'passage_id': pid, 'concept_id': 'latch'})
            insert(connection, 'source_documents', {
                'document_id': pid, 'source_id': 'fixture', 'document_hash': digest(text),
                'source_url': 'https://example.org/latch',
            })
            insert(connection, 'passage_sources', {
                'passage_id': pid, 'reference_order': 0, 'source_id': 'fixture',
                'source_url': 'https://example.org/latch', 'reference_kind': 'primary',
            })
            insert(connection, 'passage_spans', {
                'passage_id': pid, 'span_order': 0, 'document_id': pid, 'line_start': 1,
                'line_end': 1, 'heading_path_json': '[]', 'text': text, 'text_hash': digest(text),
                'source_text_hash': digest(text), 'character_count': len(text), 'normalization': None,
            })
            connection.execute('INSERT INTO passage_fts VALUES (?, ?, ?, ?, ?, ?)',
                               (pid, 'latch', 'latches', text, '', 'latch'))
        connection.commit()


def fixture_model(path):
    path.mkdir()
    (path / '1_Pooling').mkdir()
    for name in ('config.json', 'modules.json', 'tokenizer_config.json', 'tokenizer.json'):
        (path / name).write_text('{}', encoding='utf-8')
    (path / 'model.safetensors').write_bytes(b'synthetic-contract-fixture-not-real-weights')
    (path / '1_Pooling/config.json').write_text(json.dumps({
        'embedding_dimension': 768, 'pooling_mode': 'mean', 'include_prompt': True,
    }), encoding='utf-8')
    (path / 'config_sentence_transformers.json').write_text(json.dumps({
        'default_prompt_name': None, 'prompts': {'SearchQuery': 'task: search result | query: '},
    }), encoding='utf-8')


class PublicTests(unittest.TestCase):
    def test_runtime_drift_is_rejected(self):
        installed = dict(RUNTIME_VERSIONS, torch='2.8.0+cpu')
        validate_runtime_versions(installed)
        installed['transformers'] = '5.22.2'
        with self.assertRaises(ValueError):
            validate_runtime_versions(installed)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.db = self.root / 'course.sqlite'
        fixture_database(self.db)
        self.ids = [{'passage_id': 'fixture:a', 'score': 2.0},
                    {'passage_id': 'fixture:b', 'score': 1.0}]

    def evidence(self):
        return unified.build_evidence(self.ids, digest('fixture'), self.db)

    def test_public_fts_api_and_rrf(self):
        result = unified.KnowledgeSearch(unified.RetrievalConfig(self.db, mode='fts5')).retrieve('latch')
        self.assertEqual({row['passage_id'] for row in result['passages']}, {'fixture:a', 'fixture:b'})
        self.assertEqual(unified.fuse_results(self.ids, self.ids)[0]['score'], 2/61)

    def test_corrupt_body_rejected(self):
        with closing(sqlite3.connect(self.db)) as connection:
            connection.execute("UPDATE passages SET content='corrupt'")
            connection.commit()
        with self.assertRaises(CitationValidationError):
            self.evidence()

    def test_corrupt_span_rejected(self):
        with closing(sqlite3.connect(self.db)) as connection:
            connection.execute("UPDATE passage_spans SET text_hash='corrupt'")
            connection.commit()
        with self.assertRaises(CitationValidationError):
            self.evidence()

    def test_changed_body_and_hash_cannot_bypass_source_text(self):
        with closing(sqlite3.connect(self.db)) as connection:
            connection.execute('UPDATE passages SET content=?, content_hash=?',
                               ('altered quote', digest('altered quote')))
            connection.commit()
        with self.assertRaises(CitationValidationError):
            self.evidence()

    def test_primary_source_mismatch_rejected(self):
        with closing(sqlite3.connect(self.db)) as connection:
            connection.execute("UPDATE passage_sources SET reference_kind='supporting'")
            connection.commit()
        with self.assertRaises(CitationValidationError):
            self.evidence()

    def test_missing_or_duplicate_citation_rejected(self):
        original = unified.render_citations
        for transform in (lambda rows: rows[:1], lambda rows: [rows[0], rows[0]]):
            with self.subTest(transform=transform):
                with patch.object(unified, 'render_citations',
                                  side_effect=lambda *a, **kw: transform(original(*a, **kw))):
                    with self.assertRaises(CitationValidationError):
                        self.evidence()

    def test_reordered_citations_match_by_id(self):
        original = unified.render_citations
        with patch.object(unified, 'render_citations',
                          side_effect=lambda *a, **kw: list(reversed(original(*a, **kw)))):
            rows = self.evidence()
        self.assertEqual([row['passage_id'] for row in rows], ['fixture:a', 'fixture:b'])
        for row in rows:
            self.assertEqual(digest(row['content']), row['content_hash'])

    def test_one_snapshot_during_concurrent_write(self):
        with closing(sqlite3.connect(self.db)) as writer:
            writer.execute('PRAGMA journal_mode=WAL')
        original = unified.render_citations
        def concurrent_write(*args, **kwargs):
            citations = original(*args, **kwargs)
            with closing(sqlite3.connect(self.db)) as writer:
                writer.execute("UPDATE passages SET content='changed after citation'")
                writer.commit()
            return citations
        with patch.object(unified, 'render_citations', side_effect=concurrent_write):
            rows = self.evidence()
        self.assertTrue(all(digest(row['content']) == row['content_hash'] for row in rows))

    def test_full_api_keeps_lexical_and_evidence_in_one_snapshot(self):
        with closing(sqlite3.connect(self.db)) as writer:
            writer.execute('PRAGMA journal_mode=WAL')
        original = unified.search_lexical
        def publish_new_version(*args, **kwargs):
            result = original(*args, **kwargs)
            with closing(sqlite3.connect(self.db)) as writer:
                writer.execute("UPDATE passages SET content=?, content_hash=?",
                               ('new version', digest('new version')))
                writer.execute("UPDATE index_meta SET value=? WHERE key='index_version'",
                               (digest('new version'),))
                writer.commit()
            return result
        search = unified.KnowledgeSearch(unified.RetrievalConfig(self.db, mode='fts5'))
        with patch.object(unified, 'search_lexical', side_effect=publish_new_version):
            result = search.retrieve('latch')
        self.assertEqual(result['index_version'], digest('fixture'))
        self.assertTrue(all(row['content'].startswith('A latch') for row in result['passages']))

    def test_empty_candidate_list_still_checks_version(self):
        with self.assertRaises(CitationValidationError):
            unified.build_evidence([], digest('wrong version'), self.db)

    def test_configuration_drift_and_live_change(self):
        model = self.root / 'model'
        fixture_model(model)
        initial = GemmaProvider(model)
        for name in ('config.json', 'modules.json', 'tokenizer_config.json',
                     'tokenizer.json', 'model.safetensors'):
            file = model / name
            original = file.read_bytes()
            file.write_text('{"changed":true}', encoding='utf-8')
            self.assertNotEqual(initial.fingerprint, GemmaProvider(model).fingerprint)
            with self.assertRaises(ValueError):
                initial.validate_assets()
            file.write_bytes(original)
        sentence = model / 'config_sentence_transformers.json'
        data = json.loads(sentence.read_text())
        data['default_prompt_name'] = 'SearchQuery'
        sentence.write_text(json.dumps(data))
        with self.assertRaises(ValueError):
            GemmaProvider(model)

    def test_pooling_and_query_prompt_changes_rejected(self):
        model = self.root / 'model'
        fixture_model(model)
        pooling = model / '1_Pooling/config.json'
        original = pooling.read_bytes()
        pooling.write_text('{"embedding_dimension":512,"pooling_mode":"cls"}')
        with self.assertRaises(ValueError):
            GemmaProvider(model)
        pooling.write_bytes(original)
        sentence = model / 'config_sentence_transformers.json'
        data = json.loads(sentence.read_text())
        data['prompts']['SearchQuery'] = 'wrong prefix'
        sentence.write_text(json.dumps(data))
        with self.assertRaises(ValueError):
            GemmaProvider(model)

    def test_new_model_configuration_is_detected(self):
        model = self.root / 'model'
        fixture_model(model)
        provider = GemmaProvider(model)
        (model / 'sentence_bert_config.json').write_text('{"max_seq_length":512}')
        with self.assertRaises(ValueError):
            provider.validate_assets()
        self.assertNotEqual(provider.fingerprint, GemmaProvider(model).fingerprint)

    def test_missing_model_asset_is_rejected(self):
        model = self.root / 'model'
        fixture_model(model)
        (model / 'tokenizer_config.json').unlink()
        with self.assertRaises(ValueError):
            GemmaProvider(model)

    def test_atomic_index_publication_never_overwrites(self):
        output = self.root / 'index.sqlite'
        def publish(number):
            try:
                with new_index_file(output) as connection:
                    connection.execute('CREATE TABLE owner (number INTEGER)')
                    connection.execute('INSERT INTO owner VALUES (?)', (number,))
                return True
            except FileExistsError:
                return False
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(publish, (1, 2)))
        self.assertEqual(sum(results), 1)
        with closing(sqlite3.connect(output)) as connection:
            self.assertEqual(connection.execute('SELECT count(*) FROM owner').fetchone()[0], 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
