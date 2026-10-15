"""Source integrity / packing contracts. No network, model, or production writes."""
from dataclasses import asdict,replace
from contextlib import closing
from pathlib import Path
import hashlib,json,sqlite3,tempfile,unittest
from sigflow_edu_agent.rag.chunks import markdown_headings,semantic_ranges,digest
from sigflow_edu_agent.rag.external_materials import _markdown_sections
from sigflow_edu_agent.rag.importer import build_index,DEFAULT_RESOURCES_DIR
from sigflow_edu_agent.rag.retriever import retrieve_course_passages,retrieve_course_candidates
from sigflow_edu_agent.rag.citations import resolve_evidence_citations,CitationValidationError
from sigflow_edu_agent.courses import SQLiteCourseProvider,pack_courses
from sigflow_edu_agent.domain import Evidence,CardContext
from sigflow_edu_agent.context.models import ContextItem
from sigflow_edu_agent.deepseek import serialize_request
from sigflow_edu_agent.pedagogy import checked_card,valid_evidence
from sigflow_edu_agent.domain import RunRequest


class SemanticBoundaryTests(unittest.TestCase):
    def test_parent_intro_and_fenced_fake_heading(self):
        lines=['# Root','Rule with condition.','',chr(96)*3+'sv','# fake',chr(96)*3,'## Child','Child rule.']
        ranges=_markdown_sections(lines)
        self.assertEqual([(a,b) for _,a,b,_ in ranges],[(1,6),(7,8)])
        self.assertEqual(ranges[1][0],['Root','Child'])

    def test_code_and_preceding_conditions_remain_whole(self):
        lines=['Only if enabled:','',chr(96)*3+'sv','if (en) begin','  q=d;','end',chr(96)*3,'','Otherwise the prior value remains.']
        ranges=semantic_ranges(lines)
        self.assertEqual(ranges,[(0,len(lines)-1)])

    def test_list_continuation_not_severed(self):
        lines=['Requirements:','','  - default must be included','  even for complete cases.','','  - assign every output','  unless common defaults exist.']
        self.assertEqual(semantic_ranges(lines),[(0,len(lines)-1)])

    def test_rst_underline_is_not_a_markdown_fence(self):
        lines=['Topic','~~~~~','','First independent paragraph.','','.. code:: sv','','   assign y=a;','','Second independent section.']+['']+['x'*800]+['']+['y'*800]
        ranges=semantic_ranges(lines,format_hint='rst')
        self.assertGreater(len(ranges),1)
        self.assertEqual([i for a,b in ranges for i in range(a,b+1)],list(range(len(lines))))
        self.assertTrue(any('assign y=a;' in '\n'.join(lines[a:b+1]) for a,b in ranges))


class EvidenceUnitIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        snapshot=Path('D:/EDA/doc/sources/ysyx/2607')
        if not snapshot.is_dir():raise unittest.SkipTest('Pinned original snapshot unavailable')
        temporary_root=Path(__file__).parent/'.rag-tmp'
        temporary_root.mkdir(exist_ok=True)
        cls.temp=tempfile.TemporaryDirectory(dir=temporary_root)
        cls.db=Path(cls.temp.name)/'course.sqlite'
        build_index(DEFAULT_RESOURCES_DIR,cls.db,snapshot_dir=snapshot)
        cls.provider=SQLiteCourseProvider(cls.db)
    @classmethod
    def tearDownClass(cls):
        if hasattr(cls,'temp'):cls.temp.cleanup()
    def evidence(self,bytes_=0):
        return Evidence('ev','p','r','j','unknown',True,'Synthetic budget test',origin='fixture',
            context_items=(ContextItem('report','report','p','r','x'*bytes_),) if bytes_ else ())
    def test_parent_identity_registry_and_new_case_rules(self):
        with closing(sqlite3.connect(self.db)) as c:
            old=c.execute("SELECT content FROM passages WHERE passage_id='lowrisc_style:verilog:constants'").fetchone()[0]
            restored=c.execute("SELECT content FROM passages WHERE passage_id='lowrisc_style:verilog:case-statements'").fetchone()[0]
            self.assertIn('symbolically named constants',old)
            self.assertIn('Always define a default case',restored)
            self.assertIn('empty default permissible',restored)
            self.assertEqual(c.execute('select count(*) from passage_fts').fetchone()[0],
                             c.execute('select count(*) from passages').fetchone()[0])
    def test_parent_candidates_legacy_view_and_child_storage(self):
        q='WIDTHTRUNC WIDTHEXPAND WIDTHXZEXPAND Verilator'
        short=retrieve_course_passages(q,db_path=self.db)
        pool=retrieve_course_candidates(q,db_path=self.db)
        self.assertEqual(short['retrieved_passage_ids'],pool['retrieved_passage_ids'][:3])
        self.assertGreater(len(pool['passages']),3)
        self.assertEqual(pool['agent_context'],'')
        self.assertEqual(pool['context_status'],'not_packed')
        with self.assertRaises(ValueError):retrieve_course_passages(q,top_k=4,db_path=self.db)
    def test_four_complementary_definitions_and_recommendations(self):
        bundle=self.provider.prepare('区分 Verilator WIDTHTRUNC、WIDTHEXPAND、WIDTHXZEXPAND 并给修正建议。',())
        packed,refs,status,_=pack_courses(self.evidence(),bundle)
        parents={r.parent_id for r in refs}
        self.assertTrue({'verilator_5052:warnings:'+s for s in ['width','widthtrunc','widthexpand','widthxzexpand']}<=parents)
        self.assertLessEqual(len(refs),8)
        self.assertTrue(valid_evidence(packed,RunRequest('p','r','i',bundle.question)))
        resolved=resolve_evidence_citations([r.passage_id for r in refs],[r.passage_id for r in refs],
            index_version=refs[0].index_version,db_path=self.db)
        self.assertEqual(len(resolved),len(refs))
        for r in resolved:self.assertEqual(digest(r['content']),r['content_hash'])
    def test_engineering_priority_and_explicit_budget_gap(self):
        bundle=self.provider.prepare('Verilator WIDTHTRUNC',())
        before=self.evidence(7500);after,refs,status,omitted=pack_courses(before,bundle)
        self.assertEqual(before.context_items,after.context_items)
        self.assertEqual(refs,());self.assertEqual(status,'omitted')
        self.assertIn('verilator_5052',after.course_diagnostics['missing_required_sources'])
        self.assertTrue(omitted)
        self.assertTrue(all(o.reason=='engineering_over_budget' for o in omitted))
    def test_complete_request_shape_and_compact_document_registry(self):
        bundle=self.provider.prepare('Verilator WIDTHTRUNC WIDTHEXPAND WIDTHXZEXPAND',())
        ev,refs,status,_=pack_courses(self.evidence(),bundle)
        body=serialize_request(CardContext(bundle.question,3,ev,True,refs,status),model='m'*128,max_tokens=8192)
        self.assertLessEqual(len(body),16384)
        data=json.loads(json.loads(body)['messages'][1]['content'])
        self.assertIn('course_documents',data)
        self.assertNotIn('debug',data['evidence']['course_diagnostics'])
        self.assertNotIn('local_path',body.decode())
        self.assertLessEqual(sum(len(i.text.encode('utf-8')) for i in ev.context_items),4096)
    def test_citation_forgery_and_version_rejected(self):
        b=self.provider.prepare('Verilator LATCH',());_,refs,_,_=pack_courses(self.evidence(),b)
        ids=[r.passage_id for r in refs]
        with self.assertRaises(CitationValidationError):
            resolve_evidence_citations(['invented'],ids,index_version=refs[0].index_version,db_path=self.db)
        with self.assertRaises(CitationValidationError):
            resolve_evidence_citations(ids,ids,index_version='sha256:'+'0'*64,db_path=self.db)
    def test_authored_notes_have_only_local_line_locators(self):
        b=self.provider.prepare('位宽有符号数',('bit_width',))
        notes=[m.reference for m in b.materials if m.reference.material_kind=='authored_note']
        self.assertTrue(notes)
        for ref in notes:
            self.assertTrue(ref.source_url.startswith('sigflow-note:///team-note:'))
            self.assertTrue(all(s.locator_kind=='local_authored_note' for s in ref.source_spans))
    def test_eight_reference_checker_allows_only_admitted_ids(self):
        b=self.provider.prepare('Verilator WIDTHTRUNC WIDTHEXPAND WIDTHXZEXPAND',())
        ev,refs,status,_=pack_courses(self.evidence(),b)
        context=CardContext(b.question,1,ev,False,refs,status)
        value={'level':1,'hint':'比较两侧宽度。','question':'目标宽度是什么？','evidence_ids':['ev'],
               'course_passage_ids':[r.passage_id for r in refs]}
        self.assertIsNotNone(checked_card(value,context,'offline-structure-check'))
        value['course_passage_ids']=['verilator_5052:warnings:width']
        self.assertIsNone(checked_card(value,context,'offline-structure-check'))


if __name__=='__main__':unittest.main()
