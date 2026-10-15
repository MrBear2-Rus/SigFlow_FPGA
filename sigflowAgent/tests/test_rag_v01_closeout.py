"""Offline v0.1 concept and real RAG -> E3 contracts; no generation service."""
from contextlib import closing
from dataclasses import replace
from pathlib import Path
import sqlite3
import tempfile
import unittest

from sigflow_edu_agent.rag.importer import build_index, DEFAULT_RESOURCES_DIR, _write_index_file, content_hash
from sigflow_edu_agent.rag.agent_adapter import prepare_course_context
from sigflow_edu_agent.rag.retriever import retrieve_course_candidates
from sigflow_edu_agent.rag.external_materials import _concepts
from sigflow_edu_agent.rag.citations import resolve_evidence_citations, CitationValidationError, validate_citations
from sigflow_edu_agent.courses import SQLiteCourseProvider, pack_courses
from sigflow_edu_agent.domain import Evidence, RunRequest, Status
from sigflow_edu_agent.context.models import ContextItem
from sigflow_edu_agent.loop import TeachingLoop


class CloseoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root=Path(__file__).parent/'.rag-tmp';root.mkdir(exist_ok=True)
        cls.temp=tempfile.TemporaryDirectory(dir=root)
        cls.db=Path(cls.temp.name)/'full.sqlite'
        build_index(DEFAULT_RESOURCES_DIR,cls.db,snapshot_dir=Path('D:/EDA/doc/sources/ysyx/2607'))
        cls.provider=SQLiteCourseProvider(cls.db)
        # A single self-authored source enables deterministic complete/partial
        # source coverage tests independent of production retrieval quality.
        cls.small=Path(cls.temp.name)/'small.sqlite'
        text='A latch holds its prior value when the enable is inactive.'
        source={'source_id':'lowrisc_style','title':'Fixture only','source_type':'public_course',
                'base_url':'https://example.org/','author_or_source':'fixture',
                'license_or_usage_note':'self-authored fixture','review_status':'test','retrieved_at':'2026-10-15'}
        passage={'passage_id':'fixture:latch','course_id':'fixture','course_version':'fixture','title':'Latch',
                 'section':'Fixture','concept_ids':['latch'],'content':text,'content_hash':content_hash(text),
                 'source_id':'lowrisc_style','source_url':'https://example.org/fixture','content_type':'allowed_excerpt',
                 'retrieved_at':'2026-10-15','language':'en','difficulty':'intro'}
        document={'document_id':'fixture','chapter':'fixture','source_id':'lowrisc_style','source_url':passage['source_url'],
                  'title':'Fixture','document_hash':content_hash(text),'retrieved_at':'2026-10-15','lines':[text]}
        span={'span_order':0,'document_id':'fixture','anchor':None,'line_start':1,'line_end':1,
              'heading_path':['Fixture'],'text':text,'text_hash':content_hash(text),'character_count':len(text),
              'source_url':passage['source_url']}
        evidence={'content':text,'content_hash':content_hash(text),'evidence_status':'grounded','evidence_note':None,'spans':[span]}
        missing_source={**source,'source_id':'yosys_047','title':'Yosys fixture without passages'}
        _write_index_file(cls.small,[source,missing_source],[{'concept_id':'latch','aliases':['latch']},
                          {'concept_id':'unsupported_fixture','aliases':['unsupported fixture']}],[passage],[],[document],
                          {'fixture:latch':evidence},'trigram','sha256:'+'a'*64)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def evidence(self, size=0):
        return Evidence('ev','p','r','j','unknown',True,'Offline fixture',context_items=(
            (ContextItem('report','report','p','r','x'*size),) if size else ()))

    def run_loop(self, question, db=None, size=0, concepts='unknown'):
        value=replace(self.evidence(size),concept=concepts)
        class Source:
            def collect(self,*args):return value
        return TeachingLoop(RunRequest('p','r','i',question),Source(),
                            course_provider=SQLiteCourseProvider(db or self.db)).run_until_pause()

    def test_diagnostic_positive_paraphrase_and_negative(self):
        for queries,concept,wrong,code in [
            (['UNDRIVEN','未驱动','无驱动源'],'undriven_signal','multiple_driver','UNDRIVEN'),
            (['MULTIDRIVEN','多驱动','multiple drivers'],'multiple_driver','undriven_signal','MULTIDRIVEN')]:
            for query in queries:
                with self.subTest(query=query):
                    result=retrieve_course_candidates(query,db_path=self.db)
                    if query!=code:self.assertIn(concept,result['matched_concept_ids'])
                    self.assertNotIn(wrong,result['matched_concept_ids'])
                    if query==code:self.assertEqual(result['passages'][0]['title'],code)
        self.assertNotIn('multiple_driver',_concepts('UNDRIVEN','multiple drivers cross-reference','verilator_5052'))
        self.assertNotIn('undriven_signal',_concepts('MULTIDRIVEN','undriven cross-reference','verilator_5052'))

    def test_commands_parsing_and_errors_are_distinct(self):
        for query,expected in [('read_verilog','tool_command'),('工具命令','tool_command'),
                               ('parser','syntax_parsing'),('语法解析','syntax_parsing')]:
            result=retrieve_course_candidates(query,db_path=self.db)
            self.assertIn(expected,result['matched_concept_ids'])
            self.assertNotIn('syntax_error',result['matched_concept_ids'])
        for query in ['syntax error','parser error','语法错误']:
            self.assertIn('syntax_error',retrieve_course_candidates(query,db_path=self.db)['matched_concept_ids'])

    def test_source_proven_mapping_corrections(self):
        with closing(sqlite3.connect(self.db)) as c:
            for title,concept,wrong in [('Module Instantiation','port_connection','sequential_logic'),
                    ('Always be explicit about the widths of number literals.','bit_width','sequential_logic')]:
                row=c.execute('select concept_ids_json from passages where title=?',(title,)).fetchone()[0]
                self.assertIn(concept,row);self.assertNotIn(wrong,row)
        for query in ['module instantiation','模块实例化','端口连接']:
            self.assertIn('port_connection',retrieve_course_candidates(query,db_path=self.db)['matched_concept_ids'])
        for query in ['number literal width','数字字面量位宽']:
            self.assertIn('bit_width',retrieve_course_candidates(query,db_path=self.db)['matched_concept_ids'])
        self.assertNotIn('syntax_error',_concepts('Parsing','The front-end parser processes input.','yosys_047'))

    def test_real_adapter_provider_pack_and_u2_success(self):
        prepared=prepare_course_context('latch',db_path=self.small,candidate_limit=20)
        bundle=SQLiteCourseProvider(self.small).prepare('latch',())
        self.assertEqual(prepared.selection_diagnostics,bundle.selection_diagnostics)
        state=self.run_loop('latch',self.small)
        self.assertEqual(state.status,Status.WAITING_STUDENT)
        self.assertEqual(state.course_status,'ok')
        self.assertEqual(state.course_diagnostics['retrieval_status'],'ok')
        self.assertEqual(state.course_diagnostics['packing_status'],'ok')
        self.assertEqual(state.course_diagnostics['answerability'],'not_assessed')

    def test_no_candidate_and_unsupported_reach_u2(self):
        for concept in ['unknown','unsupported_fixture']:
            state=self.run_loop('zxqvnbjkl',self.small,concepts=concept)
            self.assertEqual(state.status,Status.WAITING_STUDENT)
            self.assertEqual(state.course_status,'no_match')
            self.assertEqual(state.course_diagnostics['retrieval_status'],'no_match')
            if concept!='unknown':self.assertIn(concept,state.course_diagnostics['unsupported_concept_ids'])

    def test_missing_named_source_and_partial_multi_source_reach_u2(self):
        state=self.run_loop('lowRISC Yosys latch',self.small)
        self.assertEqual(state.status,Status.WAITING_STUDENT)
        self.assertEqual(state.course_status,'partial')
        self.assertEqual(state.course_diagnostics['missing_sources'],['yosys_047'])
        self.assertEqual(state.course_diagnostics['missing_required_sources'],('yosys_047',))
        coverage={r['source_id']:r['status'] for r in state.course_diagnostics['source_coverage']}
        self.assertEqual(coverage,{'lowrisc_style':'covered','yosys_047':'no_relevant_candidate'})
        state=self.run_loop('Yosys zxqvnbjkl',self.small)
        self.assertEqual(state.course_status,'no_match')
        self.assertIn('yosys_047',state.course_diagnostics['missing_sources'])

    def test_budget_omission_preserves_engineering_and_citation_allowlist(self):
        bundle=self.provider.prepare('Verilator WIDTHTRUNC',())
        before=self.evidence(7500);after,refs,status,omitted=pack_courses(before,bundle)
        self.assertEqual(before.context_items,after.context_items)
        self.assertEqual(status,'omitted');self.assertFalse(refs);self.assertTrue(omitted)
        child=bundle.materials[0].reference
        with self.assertRaises(CitationValidationError):
            resolve_evidence_citations([child.passage_id],[],index_version=child.index_version,db_path=self.db)
        state=self.run_loop('Verilator WIDTHTRUNC',size=7500)
        self.assertEqual(state.status,Status.WAITING_STUDENT)
        self.assertEqual(state.course_status,'omitted')
        self.assertEqual(state.course_diagnostics['retrieval_status'],'ok')
        self.assertTrue(state.course_diagnostics['omission_counts'])

    def test_malformed_and_forged_citation_ids_are_rejected(self):
        for ids in [[['bad']],[4],['invented'],['same','same']]:
            with self.subTest(ids=ids),self.assertRaises(CitationValidationError):
                resolve_evidence_citations(ids,[],index_version='sha256:'+'a'*64,db_path=self.db)
            with self.subTest(ids=ids),self.assertRaises(CitationValidationError):validate_citations(ids,[])

    def test_partial_budget_omission_is_visible_with_sources_covered(self):
        b=self.provider.prepare('Verilator WIDTHTRUNC WIDTHEXPAND WIDTHXZEXPAND',())
        ev,refs,status,omitted=pack_courses(self.evidence(),b)
        self.assertTrue(refs);self.assertEqual(status,'partial')
        self.assertTrue(omitted or ev.course_diagnostics['candidate_not_selected_count'])
        self.assertEqual(ev.course_diagnostics['answerability'],'not_assessed')
        self.assertFalse(ev.course_diagnostics['missing_required_sources'])


if __name__=='__main__':unittest.main()
