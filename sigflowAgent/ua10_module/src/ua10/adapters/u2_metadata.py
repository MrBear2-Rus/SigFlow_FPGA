"""映射 U2 已提交元数据；不推断诊断、代码修改、展示回执或验证通过。"""
import hashlib
import json
from dataclasses import replace

from memory import LearningEvent, U2TeachingFacts
from memory.models import identifier
from ..errors import ScopeError, ConflictError
from .._core.util import digest
from ..events import decode_event


def record_metadata(memory, fact):
    scope = memory._scope
    memory._api._binding()
    names = ('learner_id','project_id','instance_id','session_id')
    if tuple(fact.get(k) for k in names) != (scope.profile_id,scope.project_id,scope.instance_id,scope.session_id):
        raise ScopeError('教学元数据身份不匹配')
    kind = fact.get('event_type')
    extras = {'run_created':set(), 'card_generated':{'card_index','level','producer','evidence_ids'},
              'student_action_accepted':{'action_id','action_kind'}, 'run_finished':{'level'}}
    common = set(names)|{'schema_version','revision','snapshot_id','job_id','diagnostic_id','issue_id',
        'policy','run_id','state_version','status','evidence_origin','concept_ids','event_id','event_type'}
    if kind not in extras or set(fact) != common|extras[kind]: raise ValueError('未知教学元数据结构')
    for key in ('revision','snapshot_id','job_id','diagnostic_id','issue_id','policy','run_id','event_id'):
        identifier(fact[key])
    concepts = fact['concept_ids']
    if type(concepts) is not list or len(set(concepts))!=len(concepts) or not set(concepts)<=memory._concepts:
        raise ScopeError('元数据概念超出绑定范围')
    known=memory._host.authority.v1.repo.events(scope.profile_id,session_id=scope.session_id)
    if any(not set(row['payload']['concept_ids'])<=memory._concepts for row in known if row['run_id']==fact['run_id']):
        raise ScopeError('既有运行包含当前绑定之外的概念')
    if kind=='card_generated' and (type(fact['evidence_ids']) is not list or not 1<=len(fact['evidence_ids'])<=4
            or not all(type(value) is str and value.strip() and len(value.encode('utf-8'))<=8192 for value in fact['evidence_ids'])):
        raise ValueError('无效原始证据引用')
    origin = 'real' if fact['evidence_origin']=='tool' else 'mock'
    if fact['evidence_origin'] not in ('tool','fixture') or origin != memory._mode:
        raise ScopeError('来源与宿主执行模式不一致')
    subject = str(fact['card_index']) if kind=='card_generated' else fact['action_id'] if kind=='student_action_accepted' else 'run'
    identity=[fact['instance_id'],fact['session_id'],fact['run_id'],kind,subject]
    expected='u2fact-'+hashlib.sha256(json.dumps(identity,separators=(',',':'),ensure_ascii=True).encode('ascii')).hexdigest()
    if fact['event_id']!=expected: raise ValueError('元数据事件标识不匹配')
    if type(fact['state_version']) is not str or not fact['state_version'].isdigit():raise ValueError('无效版本')
    version=int(fact['state_version']); service=memory._host.authority.v1
    prior=memory._host.authority.control.execute('SELECT event FROM intents WHERE id=?',(fact['event_id'],)).fetchone()
    marker='u2metadata:'+digest(fact)
    if prior:
        stored=decode_event(json.loads(prior[0]))
        if stored.followup_ref!=marker: raise ConflictError('同一事件标识不能替换原始元数据')
        event=replace(stored,created_at=None)
    else:
        row=service.db.connection.execute('SELECT * FROM runs WHERE run_id=?',(fact['run_id'],)).fetchone()
        if row is None:
            if kind!='run_created':raise ValueError('须先重放 run_created')
            service.create_run(scope,fact['revision'],fact['snapshot_id'],run_id=fact['run_id'],execution_mode=memory._mode,
                policy_version=fact['policy'],workflow_version='u2_teaching_fact_v1',model_version='not_reported')
        run=service.repo.check_run(scope,fact['run_id'])
        if (run['revision'],run['snapshot_id'],run['execution_mode'])!=(fact['revision'],fact['snapshot_id'],memory._mode):
            raise ScopeError('运行身份或来源发生变化')
        fields=dict(external_issue_id=fact['issue_id'], diagnostic_id=fact['diagnostic_id'], job_id=fact['job_id'],
            snapshot_id=fact['snapshot_id'], evidence_origin=fact['evidence_origin'], evidence_integrity='u2_metadata_only',
            current_version=version, status=fact['status'])
        refs=()
        if kind=='run_created':
            fields['stage']='not_reported'; event_type='u2_run_started'
        elif kind=='card_generated':
            fields.update(card_id='card:'+fact['event_id'],card_level=fact['level'],card_producer=fact['producer'])
            # 原始不透明引用仍在宿主 outbox，可通过原始 event_id 回查；此处不是报告摘要哈希。
            refs=tuple('u2-evidence:'+digest(value) for value in fact['evidence_ids'])
            event_type='u2_card_generated'
        elif kind=='student_action_accepted':
            rows=service.repo.events(scope.profile_id,session_id=scope.session_id)
            versions=[r['payload']['u2']['current_version'] for r in rows if r['run_id']==fact['run_id'] and r['payload'].get('u2')]
            # 这里只声明前一条已持久化元数据的版本，不假造 hint_next 的等级转换。
            previous=max(versions) if versions else None
            if previous is None or previous>=version:raise ValueError('缺少先行的教学元数据版本')
            fields.update(action_kind=fact['action_kind'],action_sha256=digest(fact),previous_version=previous)
            event_type='u2_action_accepted'
        else:event_type='u2_run_ended'
        event=LearningEvent(fact['event_id'],fact['run_id'],event_type,tuple(concepts),
            hint_level=fact.get('level',0),followup_ref=marker,evidence_refs=refs,u2=U2TeachingFacts(**fields))
    result=memory._api.append_learning_event(event)
    if kind=='run_finished':
        status=fact['status'] if fact['status'] in ('Completed','Cancelled','Failed') else 'Failed'
        run=service.repo.check_run(scope,fact['run_id'])
        if run['status']!=status:service.set_run_status(scope,fact['run_id'],status)
    return dict(event_id=result.event_id,accepted=True,recorded=result.recorded)
