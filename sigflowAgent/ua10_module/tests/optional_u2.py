"""真实本地 U2 类型/事实捕获的契约测试；不接生产宿主，不调用网络。"""
import hashlib
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
from types import SimpleNamespace
import unittest
from dataclasses import asdict

from ua10 import MemoryConfig, TeachingMemory
from ua10.adapters.llm import HostPatternProvider
from ua10.errors import ConfigurationError


class U2ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = os.environ.get('UA10_U2_SOURCE')
        if not path:raise unittest.SkipTest('未指定真实 U2 源码目录 UA10_U2_SOURCE')
        sys.path.append(str(Path(path).resolve()))

    def test_actual_session_binding_and_committed_metadata(self):
        from dataclasses import asdict
        from sigflow_edu_agent.runtime.gateway_session import SessionBinding
        from sigflow_edu_agent.host.teaching_memory import project_facts
        bound=SessionBinding('board','session','learner','project','r1','snapshot','job','diag','issue')
        state=dict(run_id='run-one',state_version='0',status='Running',evidence_origin='fixture',teaching_concept_ids=[])
        fact=project_facts(asdict(bound),state,created=True)[0]
        with tempfile.TemporaryDirectory() as tmp:
            with TeachingMemory(MemoryConfig(Path(tmp)),trusted_binding=bound,concepts=('latch',),execution_mode='mock') as memory:
                result=memory.record_event(fact)
                self.assertEqual(result['event_id'],fact['event_id'])
                self.assertFalse(memory.record_event(fact)['recorded'])
                self.assertEqual(memory.get_teaching_context('历史锁存器','latch','current','r2').text,'')

    def test_actual_card_provider_requires_host_json_port(self):
        from sigflow_edu_agent.deepseek import DeepSeekConfig,DeepSeekProvider
        provider=DeepSeekProvider(DeepSeekConfig('synthetic-test-placeholder','u2-selected-model'))
        with self.assertRaises(ConfigurationError):HostPatternProvider(provider)

    def test_actual_json_service_reuses_transport_and_requires_budget(self):
        from unittest.mock import patch
        from sigflow_edu_agent import deepseek as module
        provider=module.DeepSeekProvider(module.DeepSeekConfig('synthetic-placeholder','host-selected-model'))
        calls=[]
        class Connection:
            def __init__(self,*a,**kw):calls.append(('connection',kw))
            def request(self,method,path,body,headers):calls.append(('body',json.loads(body)))
            def getresponse(self):
                raw=json.dumps({'choices':[{'finish_reason':'stop','message':{'content':'{"ok":true}'}}]}).encode()
                return SimpleNamespace(status=200,read=lambda *a:raw)
            def close(self):pass
        with patch.object(module.http.client,'HTTPSConnection',Connection):
            def deny(**kw):raise RuntimeError('budget_denied')
            with self.assertRaises(RuntimeError):provider.complete_json([],max_tokens=128,reserve=deny)
            self.assertEqual(calls,[])
            value=provider.complete_json([],max_tokens=128,reserve=lambda **kw:calls.append(('reserve',kw)))
        self.assertTrue(value['ok']);self.assertEqual(calls[0][0],'reserve')
        self.assertEqual(calls[-1][1]['model'],'host-selected-model')


if __name__=='__main__':
    socket.socket.connect=lambda *a,**kw:(_ for _ in ()).throw(PermissionError('禁止联网'))
    unittest.main(verbosity=2)
