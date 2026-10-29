"""三档合成预算验证；不是远程模型 Token 实测。"""
import json,os,unittest
from pathlib import Path
import test_module as helper
from ua10 import TeachingTask, MemoryConfig, TeachingMemory
from ua10.errors import ConfigurationError

class BudgetTests(unittest.TestCase):
    setUp=helper.ModuleTests.setUp
    tearDown=helper.ModuleTests.tearDown
    def test_three_budgets(self):
        self.api.build_learning_patterns(('latch',),provider=helper.FakePatternProvider())
        rows=[]
        for budget in (512,1024,1536):
            pack=self.api.get_teaching_memory_bundle(TeachingTask('锁存器','latch','current','r3',helper.example.stamp()),token_budget=budget)
            text=json.dumps(pack.to_dict(),ensure_ascii=False,sort_keys=True,separators=(',',':'))
            self.assertLessEqual(len(text.encode()),budget)
            self.assertTrue(pack.unknown)
            rows.append(dict(budget=budget,chars=len(text),utf8_bytes=len(text.encode()),measurement='utf8_bytes_token_upper_bound',truncated=pack.dto['truncated'],omitted=pack.dto['omitted_fact_count'],facts=[f.event_id for f in pack.facts],sources=[f.source_kind for f in pack.facts],patterns=len(pack.patterns),recommendations=len(pack.recommendations),passed_retained=any(f.event_id=='event-passed' for f in pack.facts)))
        if os.environ.get('UA10_BUDGET_REPORT'):Path(os.environ['UA10_BUDGET_REPORT']).write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
    def test_normal_entry_cap(self):
        self.assertEqual(MemoryConfig(self.path).default_token_budget,1024)
        with self.assertRaises(ConfigurationError):TeachingMemory(MemoryConfig(self.path,default_token_budget=2048),trusted_binding=dict(learner_id='l',project_id='p',instance_id='i',session_id='s'),concepts=('latch',))
