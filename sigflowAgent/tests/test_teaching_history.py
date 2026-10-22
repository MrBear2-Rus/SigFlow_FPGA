"""History affects bounded phrasing context, never evidence or authority."""
from dataclasses import replace
import json
import unittest

from sigflow_edu_agent.demo import FixtureSource
from sigflow_edu_agent.domain import RunRequest, ProviderError
from sigflow_edu_agent.loop import TeachingLoop
from sigflow_edu_agent.deepseek import serialize_request


class TeachingHistoryTests(unittest.TestCase):
    def setUp(self):
        self.assertIn('learning_history',__import__('sigflow_edu_agent.domain',fromlist=['CardContext']).CardContext.__dataclass_fields__,
                      'history_context_port_missing')
        self.contexts=[];self.reads=[]
        owner=self
        class Provider:
            def generate(self,context):
                owner.contexts.append(context)
                if owner.repair and not context.repair:raise ProviderError('invalid_output')
                return dict(level=context.level,hint='先核对设计目标。',question='需要记忆吗？',
                            evidence_ids=[context.evidence.evidence_id])
        self.provider=Provider();self.repair=False
        self.loop=TeachingLoop(RunRequest('p','r','issue','当前问题'),FixtureSource(),self.provider)

    def test_each_card_reads_history_once_and_model_repair_reuses_it(self):
        self.repair=True
        def history(concept_ids):
            self.reads.append(concept_ids)
            return '此前已接触锁存器；尚无独立修复证据。'
        self.loop.history_provider=history
        state=self.loop.run_until_pause()
        self.assertEqual((state.level,state.model_calls,len(self.contexts),len(self.reads)),(1,2,2,1))
        self.assertEqual(self.reads,[('latch',)])
        self.assertEqual(self.contexts[0].learning_history,self.contexts[1].learning_history)
        self.assertEqual(self.contexts[0].evidence,self.loop._evidence)
        payload=json.loads(serialize_request(self.contexts[0],model='test',max_tokens=1024))
        data=json.loads(payload['messages'][1]['content'])
        self.assertEqual(data['learning_history'],'此前已接触锁存器；尚无独立修复证据。')
        self.loop.act('hint_next','h',state.version)
        state=self.loop.run_until_pause()
        self.assertEqual((state.level,len(self.reads)),(2,2))

    def test_failed_oversized_or_invalid_history_never_blocks_card(self):
        def broken(_):raise OSError('private path and token')
        for provider in (broken,lambda _:'界'*1025,lambda _: {'summary':'unexpected'},lambda _:'bad\x00text'):
            with self.subTest(provider=provider):
                self.contexts.clear()
                loop=TeachingLoop(RunRequest('p','r','issue','当前问题'),FixtureSource(),self.provider)
                loop.history_provider=provider
                state=loop.run_until_pause()
                self.assertEqual((state.status,state.level,state.model_calls),('WaitingStudent',1,1))
                self.assertEqual(self.contexts[0].learning_history,'')

    def test_history_never_displaces_engineering_or_exceeds_provider_request_budget(self):
        from sigflow_edu_agent.context.models import ContextItem
        source=FixtureSource()
        evidence=source.collect('p','r')
        # Model request is already near its limit; preserve the engineering body.
        evidence=replace(evidence,context_items=(ContextItem('eng','rtl','p','r','x'*8192),))
        source.collect=lambda *_:evidence
        loop=TeachingLoop(RunRequest('p','r','issue','q'*4096),source,self.provider)
        loop.history_provider=lambda _:'h'*1024
        loop.run_until_pause()
        context=self.contexts[0]
        self.assertEqual(context.evidence,evidence)
        payload=serialize_request(context,model='m'*128,max_tokens=8192)
        self.assertLessEqual(len(payload),16384)
