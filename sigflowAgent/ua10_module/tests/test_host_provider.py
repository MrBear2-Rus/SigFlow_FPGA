"""无额外密钥的宿主模型注入；只使用假服务。"""
import unittest
import test_module as helper

from ua10.adapters.llm import HostPatternProvider
from ua10.errors import ConfigurationError, PatternGenerationError


class FakeLLM:
    def __init__(self):self.calls = []
    def complete_json(self, messages, *, max_tokens):
        self.calls.append((messages, max_tokens))
        return helper.FakePatternProvider().generate(messages, max_tokens=max_tokens)


class HostProviderTests(unittest.TestCase):
    setUp = helper.ModuleTests.setUp
    tearDown = helper.ModuleTests.tearDown

    def test_uses_host_service_and_records_actual_model_without_extra_key(self):
        llm = FakeLLM(); provider = HostPatternProvider(llm, model_id='u2-test-model', is_network=False)
        p = self.api.build_learning_patterns(('latch',), provider=provider)
        self.assertEqual(len(llm.calls), 1)
        row = self.host.projection.patterns(
            [helper.example.LEARNER,helper.example.PROJECT,helper.example.INSTANCE],helper.example.stamp())
        self.assertEqual(row[0]['model_id'], 'u2-test-model')
        self.assertTrue(self.api.get_learning_patterns().patterns)
        self.assertTrue(p.needs_teacher_review)

    def test_network_disabled_before_delegate_and_host_errors_not_retried(self):
        llm = FakeLLM(); provider = HostPatternProvider(llm)
        with self.assertRaises(PermissionError):self.api.build_learning_patterns(('latch',), provider=provider)
        self.assertEqual(llm.calls, [])
        class Failed:
            calls = 0
            def complete_json(self,*args,**kwargs):self.calls+=1;raise TimeoutError('synthetic-timeout')
        failed = Failed(); provider = HostPatternProvider(failed, is_network=False)
        with self.assertRaises(TimeoutError):self.api.build_learning_patterns(('latch',), provider=provider)
        self.assertEqual(failed.calls,1);self.assertFalse(self.api.get_learning_patterns().patterns)

    def test_card_only_provider_and_empty_or_forged_result_rejected(self):
        class CardOnly:
            def generate(self, context):raise AssertionError('不应调用卡片接口')
        with self.assertRaises(ConfigurationError):HostPatternProvider(CardOnly())
        class Empty:
            def complete_json(self,*args,**kwargs):return {}
        with self.assertRaises(PatternGenerationError):HostPatternProvider(Empty()).generate([],max_tokens=128)
        class Forged(FakeLLM):
            def complete_json(self,*args,**kwargs):
                result=super().complete_json(*args,**kwargs);result['evidence'][0]['run_id']='foreign';return result
        with self.assertRaises(helper.EvidenceViolation):self.api.build_learning_patterns(('latch',),
            provider=HostPatternProvider(Forged(),is_network=False))


if __name__=='__main__':unittest.main(verbosity=2)
