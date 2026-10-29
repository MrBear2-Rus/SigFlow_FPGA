"""已安装模块的调用示例：每个阶段使用新进程，不修改 sys.path。"""
from pathlib import Path
from dataclasses import asdict
import argparse,json
from ua10 import MemoryHost,MemoryConfig,LearningEvent,JobEvidence,Observation,TeachingTask,Scope
from datetime import datetime,timezone
from ua10.errors import DeletedScopeError

LEARNER='example-learner';PROJECT='example-fpga';INSTANCE='example-board'
def stamp():return datetime.now(timezone.utc).isoformat()
def job(name,rev,snapshot,*,outcome='failed'):
    return JobEvidence(name,PROJECT,INSTANCE,rev,snapshot,'report:'+name,'sha256:'+'0'*64,
        kind='real',validation='test_expectation',outcome=outcome,complete=True,scope_ref='expected:'+name)
def seed(host):
    host.create_profile(LEARNER);s1=host.create_session(LEARNER,PROJECT,INSTANCE,'session-one');api=host.bind(s1)
    api.create_run('r1','snapshot-one',run_id='run-one',execution_mode='real')
    observation=api.append_learning_event(LearningEvent('event-one','run-one','issue_observed',('latch',),job=job('job-one','r1','snapshot-one')),
        observation=Observation('LATCH_INFERRED','exercise-mux','top.mux'),synthetic=True)
    api.create_run('r2','snapshot-two',run_id='run-two',execution_mode='real')
    api.append_learning_event(LearningEvent('event-modified','run-two','student_modified',('latch',),observation.issue_id,observation.occurrence_id,student_modified=True),synthetic=True)
    api.append_learning_event(LearningEvent('event-passed','run-two','retry_passed',('latch',),observation.issue_id,observation.occurrence_id,
        job=job('job-two','r2','snapshot-two',outcome='passed')),synthetic=True)
    api.complete_run('run-one');api.complete_run('run-two')
    s2=host.create_session(LEARNER,PROJECT,INSTANCE,'session-two');other=host.bind(s2)
    other.create_run('r1','snapshot-three',run_id='run-three',execution_mode='real')
    second=other.append_learning_event(LearningEvent('event-three','run-three','issue_observed',('latch',),job=job('job-three','r1','snapshot-three')),
        observation=Observation('LATCH_INFERRED','exercise-decoder','top.decoder'),synthetic=True)
    other.complete_run('run-three')
    assert observation.issue_id!=second.issue_id
    return dict(written=4,independent_issue_ids=True,synthetic=True,paid_requests=0)
def read(host):
    api=host.bind(Scope(LEARNER,PROJECT,INSTANCE,'session-two'));cutoff=stamp();ids=[];offset=0
    while True:
        page=api.get_learning_timeline(as_of=cutoff,offset=offset)
        ids.extend(f.event_id for f in page.facts)
        if not page.has_more:break
        assert page.next_offset is not None;offset=page.next_offset
    search=api.search_memories('之前的锁存器诊断');assert search.facts
    bundle=api.get_teaching_memory_bundle(TeachingTask('如何检查锁存器相关路径？','latch','current-report','r3',cutoff))
    assert bundle.facts and not bundle.patterns and not bundle.recommendations
    related=api.get_related_attempts('latch');assert all(not x.fusion_allowed for x in related.attempts)
    return dict(timeline=ids,search=[f.event_id for f in search.facts],bundle=bundle.to_dict(),paid_requests=0)
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--data-dir',type=Path,required=True)
    parser.add_argument('--phase',choices=['seed','read','revoke','verify','delete','verify-deleted'],required=True);args=parser.parse_args()
    with MemoryHost(MemoryConfig(args.data_dir)) as host:
        if args.phase=='seed':result=seed(host)
        elif args.phase=='read':result=read(host);assert len(result['timeline'])==4
        elif args.phase=='revoke':
            api=host.bind(Scope(LEARNER,PROJECT,INSTANCE,'session-two'));result=asdict(api.revoke_learning_event('event-one'))
            api.rebuild_index()
        elif args.phase=='verify':
            result=read(host);assert 'event-one' not in result['timeline'];assert len(result['timeline'])==3
            result['reopen_after_revocation']=True
        elif args.phase=='delete':
            api=host.bind(Scope(LEARNER,PROJECT,INSTANCE,'session-two'));result=api.delete_session()
            assert result['status']=='completed'
        else:
            try:host.bind(Scope(LEARNER,PROJECT,INSTANCE,'session-two'))
            except DeletedScopeError:pass
            else:raise AssertionError('deleted session resurrected')
            api=host.bind(Scope(LEARNER,PROJECT,INSTANCE,'session-one'));api.rebuild_index()
            facts=api.get_learning_timeline().facts
            assert {f.event_id for f in facts}=={'event-modified','event-passed'}
            result=dict(reopen_after_physical_delete=True,remaining_events=2,paid_requests=0)
    result['phase']=args.phase
    print(json.dumps(result,ensure_ascii=True))
if __name__=='__main__':main()
