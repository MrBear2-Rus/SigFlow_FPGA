"""Portable course integration tests: generated material, no external textbook required."""
from contextlib import closing
from dataclasses import replace
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch

from sigflow_edu_agent.courses import SQLiteCourseProvider
from sigflow_edu_agent.domain import (CourseBundle, CourseMaterial, CourseReference, Evidence,
                                     RunRequest, Status)
from sigflow_edu_agent.context.models import ContextItem
from sigflow_edu_agent.deepseek import DeepSeekConfig, DeepSeekProvider
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.pedagogy import RuleProvider
from sigflow_edu_agent.rag.database import open_database
from sigflow_edu_agent.rag.importer import _write_index_file, content_hash
from sigflow_edu_agent.rag.retriever import retrieve_course_passages


class Source:
    def __init__(self):
        self.evidence = Evidence('ev1', 'p', 'r', 'j', 'latch', True, '报告观察到存储行为', True)
    def collect(self, project_id, revision):
        return self.evidence


class Courses:
    def __init__(self, bundle=None):
        self.bundle = bundle or CourseBundle('ok', tuple(material(str(i)) for i in range(2)))
        self.calls = []
    def prepare(self, question, concept_ids):
        self.calls.append((question, concept_ids))
        return self.bundle


def material(identifier, text='课程正文说明赋值路径与保持状态之间的关系。', version='sha256:' + 'a' * 64):
    return CourseMaterial(CourseReference('test:latch:' + identifier, version, '课程标题', '章节',
                          'https://example.org/course', 'source_excerpt', '测试自行编写的课程文字'), text)


class Probe:
    def __init__(self, bad_first=False):
        self.contexts = []
        self.bad_first = bad_first
    def generate(self, context):
        self.contexts.append(context)
        result = RuleProvider().generate(context)
        ids = [ref.passage_id for ref in context.course_refs[:1]]
        result['course_passage_ids'] = ['invented'] if self.bad_first and len(self.contexts) == 1 else ids
        return result


class CourseIntegrationTests(unittest.TestCase):
    def loop(self, source=None, provider=None, courses=None):
        return TeachingLoop(RunRequest('p', 'r', 'i', '为什么出现锁存器？'), source or Source(),
                            provider, course_provider=courses)

    def test_course_runs_in_e3_and_citations_keep_engineering_identity(self):
        courses, probe = Courses(), Probe()
        loop = self.loop(provider=probe, courses=courses)
        for _ in range(3): loop.step()
        self.assertEqual(courses.calls, [])
        loop.step()
        self.assertEqual(len(courses.calls), 1)
        state = loop.run_until_pause()
        self.assertEqual(state.course_status, 'ok')
        self.assertEqual(state.cards[0].evidence_ids, ('ev1',))
        self.assertEqual(state.cards[0].course_refs, probe.contexts[0].course_refs[:1])
        self.assertEqual(probe.contexts[0].evidence.origin, 'fixture')

    def test_repair_preserves_exact_material_and_reference_allowlist(self):
        probe = Probe(bad_first=True)
        state = self.loop(provider=probe, courses=Courses()).run_until_pause()
        self.assertEqual(state.cards[0].producer, 'model')
        self.assertEqual(len(probe.contexts), 2)
        self.assertTrue(probe.contexts[1].repair)
        self.assertEqual(probe.contexts[0].evidence, probe.contexts[1].evidence)
        self.assertEqual(probe.contexts[0].course_refs, probe.contexts[1].course_refs)

    def test_invalid_or_duplicate_citations_fall_back_without_false_refs(self):
        for ids in (['invented'], ['test:latch:0'] * 2, [4], 'test:latch:0', [['bad']]):
            class Bad:
                def generate(self, context):
                    value = RuleProvider().generate(context)
                    value['course_passage_ids'] = ids
                    return value
            with self.subTest(ids=ids):
                state = self.loop(provider=Bad(), courses=Courses()).run_until_pause()
                self.assertEqual(state.cards[0].producer, 'rule')
                self.assertEqual(state.cards[0].course_refs, ())

    def test_budget_never_displaces_engineering_or_allows_omitted_citation(self):
        source = Source()
        original = ContextItem('report', 'report', 'p', 'r', '工' * 1400)
        source.evidence = replace(source.evidence, context_items=(original,))
        probe = Probe()
        state = self.loop(source, probe, Courses()).run_until_pause()
        self.assertEqual(state.course_status, 'omitted')
        self.assertEqual(probe.contexts[0].evidence.context_items, (original,))
        self.assertEqual(probe.contexts[0].course_refs, ())
        self.assertEqual(len(state.course_omitted), 2)

    def test_partial_packing_only_admits_retained_reference(self):
        courses = Courses(CourseBundle('ok', (material('a', '文' * 1200), material('b', '文' * 1200))))
        probe = Probe()
        state = self.loop(provider=probe, courses=courses).run_until_pause()
        self.assertEqual(state.course_status, 'partial')
        self.assertEqual([r.passage_id for r in probe.contexts[0].course_refs], ['test:latch:a'])

    def test_missing_courses_no_match_and_fault_are_distinct(self):
        self.assertEqual(self.loop().run_until_pause().course_status, 'disabled')
        self.assertEqual(self.loop(courses=Courses(CourseBundle('no_match'))).run_until_pause().course_status, 'no_match')
        class Broken:
            def prepare(self, *args): raise RuntimeError('private-path-not-to-return')
        state = self.loop(courses=Broken()).run_until_pause()
        self.assertEqual(state.course_status, 'unavailable')
        self.assertEqual(state.status, Status.WAITING_STUDENT)
        self.assertNotIn('private-path', repr(state))

    def test_malformed_material_and_mixed_versions_are_unavailable(self):
        bundles = [CourseBundle('ok', (material('a'), material('b', version='different'))),
                   CourseBundle('ok', (material('a', '\ud800'),)),
                   CourseBundle('no_match', (material('a'),)),
                   CourseBundle('ok', (replace(material('a'), reference=replace(material('a').reference, source_url='file:///private')),))]
        for bundle in bundles:
            with self.subTest(bundle=bundle):
                state = self.loop(courses=Courses(bundle)).run_until_pause()
                self.assertEqual(state.course_status, 'unavailable')

    def test_invalid_engineering_evidence_prevents_course_query(self):
        source, courses = Source(), Courses()
        source.evidence = replace(source.evidence, complete=False)
        self.assertEqual(self.loop(source, courses=courses).run_until_pause().status, Status.WAITING_EVIDENCE)
        self.assertEqual(courses.calls, [])

    def test_next_hint_requeries_and_drops_previous_reference_permission(self):
        courses, probe = Courses(), Probe()
        loop = self.loop(provider=probe, courses=courses)
        state = loop.run_until_pause()
        courses.bundle = CourseBundle('no_match')
        loop.act('hint_next', 'next', state.version)
        state = loop.run_until_pause()
        self.assertEqual(len(courses.calls), 2)
        self.assertEqual(state.course_status, 'no_match')
        self.assertEqual(state.cards[-1].course_refs, ())

    def test_mocked_model_receives_course_text_and_allowed_ids(self):
        with patch('sigflow_edu_agent.deepseek.http.client.HTTPSConnection') as factory:
            reply = factory.return_value.getresponse.return_value
            reply.status = 200
            candidate = {'level': 1, 'hint': '请检查保持状态的条件。', 'question': '是否有未赋值路径？',
                         'evidence_ids': ['ev1'], 'course_passage_ids': ['test:latch:0']}
            reply.read.return_value = json.dumps({'choices': [{'finish_reason': 'stop', 'message': {'content': json.dumps(candidate)}}]}).encode()
            state = self.loop(provider=DeepSeekProvider(DeepSeekConfig('fake-key', 'fake-model')), courses=Courses()).run_until_pause()
            body = factory.return_value.request.call_args.kwargs['body']
            payload = json.loads(body)
            context = json.loads(payload['messages'][1]['content'])
            self.assertLessEqual(len(body), 16384)
            self.assertEqual(len(context['course_refs']), 2)
            self.assertTrue(any(i['kind'] == 'course' for i in context['evidence']['context_items']))
            self.assertNotIn('local_path', body.decode())
            self.assertEqual(len(state.cards[0].course_refs), 1)


class CurrentDatabaseTests(unittest.TestCase):
    def setUp(self):
        parent = Path(__file__).parent / '.rag-tmp'
        parent.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'course.sqlite'

    def make_database(self):
        text = '测试课程说明：组合逻辑中，不完整赋值可能导致输出保持旧值，需要核对每条条件路径。'
        source = {'source_id': 'test', 'title': 'Test', 'source_type': 'public_course', 'base_url': 'https://example.org/',
                  'author_or_source': 'test author', 'license_or_usage_note': 'self-authored test fixture', 'review_status': 'test', 'retrieved_at': '2026-10-12'}
        passage = {'passage_id': 'test:latch:hold', 'course_id': 'test', 'course_version': 'test', 'title': '锁存器',
                   'section': '测试章节', 'concept_ids': ['latch'], 'content': text, 'content_hash': content_hash(text),
                   'source_id': 'test', 'source_url': 'https://example.org/course', 'content_type': 'allowed_excerpt',
                   'retrieved_at': '2026-10-12', 'language': 'zh-CN', 'difficulty': 'intro'}
        document = {'document_id': 'd', 'chapter': 'test', 'source_id': 'test', 'source_url': passage['source_url'],
                    'title': 'test source', 'document_hash': content_hash(text), 'retrieved_at': '2026-10-12',
                    'lines':text.splitlines()}
        span = {'span_order': 0, 'document_id': 'd', 'anchor': None, 'line_start': 1, 'line_end': 1,
                'heading_path': ['test'], 'text': text, 'text_hash': content_hash(text), 'character_count': len(text),
                'source_url':passage['source_url']}
        evidence = {'content': text, 'content_hash': content_hash(text), 'evidence_status': 'grounded', 'evidence_note': None, 'spans': [span]}
        _write_index_file(self.path, [source], [{'concept_id': 'latch', 'aliases': ['锁存器', 'latch']}], [passage], [],
                          [document], {passage['passage_id']: evidence}, 'trigram', 'sha256:' + 'a' * 64)

    def test_generated_database_drives_real_loop_without_original_course(self):
        self.make_database()
        probe = Probe()
        state = TeachingLoop(RunRequest('p', 'r', 'i', '锁存器'), Source(), probe,
                             course_provider=SQLiteCourseProvider(self.path)).run_until_pause()
        self.assertEqual(state.course_status, 'ok')
        self.assertEqual(state.cards[0].course_refs[0].parent_id, 'test:latch:hold')
        self.assertTrue(state.cards[0].course_refs[0].passage_id.startswith('test:latch:hold--c'))
        with closing(open_database(self.path)) as connection:
            with self.assertRaises(sqlite3.OperationalError): connection.execute('DELETE FROM passages')
            self.assertEqual(connection.execute('PRAGMA integrity_check').fetchone()[0], 'ok')

    def test_old_schema_is_rejected_instead_of_adapted(self):
        self.make_database()
        with closing(sqlite3.connect(self.path)) as connection:
            connection.execute('PRAGMA user_version=2')
        with self.assertRaisesRegex(ValueError, 'unsupported_course_schema'):
            retrieve_course_passages('锁存器', db_path=self.path)

    def test_old_fts_tokenizer_is_rejected(self):
        self.make_database()
        with closing(sqlite3.connect(self.path)) as connection:
            connection.execute("UPDATE index_meta SET value='unicode61' WHERE key='fts_tokenizer'")
            connection.commit()
        with self.assertRaisesRegex(ValueError, 'invalid_course_metadata'):
            retrieve_course_passages('锁存器', db_path=self.path)

    def test_absent_database_is_not_created_by_agent(self):
        state = TeachingLoop(RunRequest('p', 'r', 'i', '锁存器'), Source(), Probe(),
                             course_provider=SQLiteCourseProvider(self.path)).run_until_pause()
        self.assertEqual(state.course_status, 'unavailable')
        self.assertFalse(self.path.exists())


if __name__ == '__main__':
    unittest.main()
