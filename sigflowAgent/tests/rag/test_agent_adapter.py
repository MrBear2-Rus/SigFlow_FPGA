from dataclasses import FrozenInstanceError, asdict
from contextlib import closing
import importlib.util
import json
from pathlib import Path
import shutil
import sqlite3
import tempfile
import unittest

from sigflow_edu_agent.rag.agent_adapter import prepare_course_context, resolve_course_refs
from sigflow_edu_agent.rag.citations import CitationValidationError, render_citations
from sigflow_edu_agent.rag.importer import build_index, DEFAULT_RESOURCES_DIR
import os
SNAPSHOT_DIR = Path(os.environ.get('SIGFLOW_COURSE_SNAPSHOT', str(Path(__file__).resolve().parents[4] / 'doc/sources/ysyx/2607')))

from sigflow_edu_agent.rag.retriever import retrieve_course_passages


@unittest.skipUnless((SNAPSHOT_DIR / "manifest.jsonl").is_file(), "Set SIGFLOW_COURSE_SNAPSHOT for local original-text regression")
class AgentAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_parent = Path(__file__).resolve().parents[2] / 'tests' / '.rag-tmp'
        cls.temp_parent.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='ua09-agent-adapter-', dir=cls.temp_parent)
        cls.root = Path(cls.temp.name)
        cls.db = cls.root / 'course.sqlite'
        resources = DEFAULT_RESOURCES_DIR
        cls.version = build_index(resources, cls.db, snapshot_dir=SNAPSHOT_DIR)['index_version']

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()
        if not any(cls.temp_parent.iterdir()):
            cls.temp_parent.rmdir()

    def prepare(self):
        return prepare_course_context('组合逻辑中为什么出现锁存器', ['latch'], db_path=self.db)

    def test_context_is_path_free_and_does_not_repeat_authored_notes(self):
        prepared = self.prepare()
        self.assertEqual(prepared.status, 'ok')
        self.assertEqual(prepared.index_version, self.version)
        self.assertLessEqual(len(prepared.passages), 3)
        authored = [p for p in prepared.passages if p.reference.material_kind == 'authored_note']
        self.assertFalse(authored)
        self.assertTrue(any(p.reference.material_kind == 'source_excerpt' for p in prepared.passages))
        with closing(sqlite3.connect(self.db)) as connection:
            rows = connection.execute("SELECT COUNT(*) FROM passages WHERE content_type='authored_note'").fetchone()[0]
        self.assertEqual(rows, 9)
        for item in prepared.passages:
            self.assertNotIn('本地课程正文:', item.text)
            self.assertNotIn('本地讲解范围:', item.text)
            self.assertNotIn('D:\\', item.text)
            self.assertNotIn('local_path', repr(item.reference))
        with self.assertRaises(FrozenInstanceError):
            prepared.status = 'changed'

    def test_only_admitted_retrieved_ids_can_be_resolved(self):
        prepared = self.prepare()
        first, second = [p.reference.passage_id for p in prepared.passages[:2]]
        self.assertEqual(resolve_course_refs(prepared, [first], admitted_passage_ids=[first]),
                         (prepared.passages[0].reference,))
        for cited, admitted in (([second], [first]), (['invented'], [first]),
                                ([first, first], [first]), ([['not-a-string']], [first]),
                                ([], ['invented'])):
            with self.subTest(cited=cited), self.assertRaises(CitationValidationError):
                resolve_course_refs(prepared, cited, admitted_passage_ids=admitted)

    def test_explicit_database_ignores_manifest_and_prepared_refs_are_frozen(self):
        prepared = self.prepare()
        first = prepared.passages[0].reference
        second = self.root / 'second.sqlite'
        shutil.copyfile(self.db, second)
        with closing(sqlite3.connect(second)) as connection:
            connection.execute("UPDATE index_meta SET value=? WHERE key='index_version'", ('sha256:' + 'b' * 64,))
            connection.execute("UPDATE passages SET title='changed title' WHERE passage_id=?", (first.passage_id,))
            connection.commit()
        manifest = self.root / 'manifest.json'
        manifest.write_text(json.dumps({'database_file': second.name}), encoding='utf-8')
        try:
            self.assertEqual(retrieve_course_passages('锁存器', ['latch'], db_path=self.db)['index_version'], self.version)
            result = retrieve_course_passages('锁存器', ['latch'], db_path=self.db)
            with self.assertRaises(CitationValidationError):
                render_citations([first.passage_id], result, db_path=second)
            self.assertEqual(resolve_course_refs(prepared, [first.passage_id], admitted_passage_ids=[first.passage_id]), (first,))
        finally:
            manifest.unlink()
            second.unlink()

    def test_missing_database_is_not_created(self):
        missing = self.root / 'absent' / 'missing.sqlite'
        with self.assertRaises(FileNotFoundError):
            prepare_course_context('锁存器', db_path=missing)
        self.assertFalse(missing.exists())

    def test_no_match_is_not_a_database_failure(self):
        prepared = prepare_course_context('banana quantum teleportation', db_path=self.db)
        self.assertEqual(prepared.status, 'no_match')
        self.assertEqual(prepared.passages, ())
        self.assertEqual(prepared.index_version, self.version)

    @unittest.skipUnless(importlib.util.find_spec('sigflow_edu_agent'), 'Install the official U2 package for integration')
    def test_real_u2_loop_receives_course_and_returns_verified_reference(self):
        from sigflow_edu_agent.courses import SQLiteCourseProvider
        from sigflow_edu_agent.demo import FixtureSource
        from sigflow_edu_agent.domain import RunRequest, Status
        from sigflow_edu_agent.loop import TeachingLoop
        from sigflow_edu_agent.pedagogy import RuleProvider
        seen = []
        class OfflineProbe:
            def generate(self, context):
                seen.append(context)
                value = RuleProvider().generate(context)
                value['course_passage_ids'] = [context.course_refs[0].passage_id]
                return value
        loop = TeachingLoop(RunRequest('demo', 'r1', 'latch', '组合逻辑中为什么出现锁存器'),
                            FixtureSource(), OfflineProbe(), course_provider=SQLiteCourseProvider(self.db))
        state = loop.run_until_pause()
        self.assertEqual(state.status, Status.WAITING_STUDENT)
        self.assertIn(state.course_status, ('ok', 'partial'))
        self.assertTrue(any(item.kind == 'course' for item in seen[0].evidence.context_items))
        self.assertEqual(state.cards[0].evidence_ids, ('fixture-latch-1',))
        ref = state.cards[0].course_refs[0]
        self.assertEqual(ref.index_version, self.version)
        self.assertEqual(ref, seen[0].course_refs[0])
        self.assertNotIn('local_path', json.dumps(asdict(ref), ensure_ascii=False))
        self.assertEqual(seen[0].evidence.origin, 'fixture')


if __name__ == '__main__':
    unittest.main()
