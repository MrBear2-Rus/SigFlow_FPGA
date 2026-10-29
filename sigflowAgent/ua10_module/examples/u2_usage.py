"""已安装 U2 与 UA-10 的真实本地 Runtime 联调；只有合成源与 Fake 模型。"""
import argparse
import json
from pathlib import Path
import time
from ua10 import MemoryConfig
from sigflow_u2.context.models import EvidenceAnchor
from sigflow_u2.domain import Evidence
from sigflow_u2.loop import TeachingLoop
from sigflow_u2.runtime.session import TeachingSession
from sigflow_u2.host.runtime import Runtime
from sigflow_u2.host.store import DurableStore
from sigflow_u2.host.ua10 import U2TeachingMemoryPort


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--data-dir',type=Path,required=True)
    args=parser.parse_args();args.data_dir.mkdir(parents=True,exist_ok=True)
    contexts=[]
    class SyntheticToolSource:
        def collect(self,project,revision):
            anchor=EvidenceAnchor('synthetic-evidence',project,revision,'synthetic-job','snap','synthetic-artifact','a'*64)
            return Evidence('synthetic-evidence',project,revision,'synthetic-job','latch',True,
                '合成工具协议测试，非真实工程或学生',True,'tool',anchor=anchor)
    class FakeModel:
        def generate(self,context):
            contexts.append(context)
            return dict(level=context.level,hint='核对设计目标',question='需要保持旧值吗？',evidence_ids=[context.evidence.evidence_id])
    def factory(binding,request,mode,reserve,**options):
        return TeachingSession(TeachingLoop(request,SyntheticToolSource(),FakeModel()))
    port=U2TeachingMemoryPort(MemoryConfig(args.data_dir/'memory'),instance_id='synthetic-inst',concepts=('latch',),evidence_origin='tool')
    store=DurableStore(args.data_dir/'host.sqlite','synthetic-inst')
    runtime=Runtime(store,'synthetic-inst',factory,evidence_origin='tool',teaching_memory=port)
    try:
        body=dict(learner_id='synthetic-learner',project_id='synthetic-project',revision='r1',snapshot_id='snap',
            job_id='synthetic-job',diagnostic_id='synthetic-diag',issue_id='synthetic-issue',question='锁存器历史提示',mode='deepseek')
        for key in ('synthetic-session-one','synthetic-session-two'):
            sid=runtime.create(body,key)['session_id'];deadline=time.monotonic()+5
            while time.monotonic()<deadline:
                if store.action(sid,'create')['status']=='committed' and not store.pending_teaching_facts(32):break
                time.sleep(.03)
            else:raise RuntimeError('合成工作流或 outbox 未完成')
            if key=='synthetic-session-one':
                state=runtime.snapshot(sid,'synthetic-learner')
                runtime.action(sid,'synthetic-learner','finish','finish',int(state['state_version']))
                deadline=time.monotonic()+5
                while time.monotonic()<deadline:
                    if store.action(sid,'finish')['status']=='committed' and not store.pending_teaching_facts(32):break
                    time.sleep(.03)
                else:raise RuntimeError('合成首会话未完成')
        assert any(c.learning_history for c in contexts), '跨会话上下文未进入模型'
        print(json.dumps(dict(跨会话上下文已进入Fake模型=True,已提交待重放数量=len(store.pending_teaching_facts(32)),
            合成工具协议测试=True,真实付费请求数=0),ensure_ascii=False))
    finally:
        runtime.close();port.close();store.close()


if __name__=='__main__':main()
