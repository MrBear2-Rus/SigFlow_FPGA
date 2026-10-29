"""Read only trusted DTOs; render facts deterministically, never LLM conclusions."""
from __future__ import annotations
import hashlib
import json
from datetime import datetime

VERSION = 'ua10-trusted-view.v2.1'
LABELS = {
    'latch': '锁存器 组合逻辑 赋值不完整 分支 保持旧值',
    'complete_assignment': '完整赋值 默认值 所有路径',
    'blocking_nonblocking': '阻塞赋值 非阻塞赋值 时序 更新顺序',
    'counter': '计数器 计数', 'reset': '复位 清零 初始值 reset',
}

def encode(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'))

def digest(value):
    return hashlib.sha256(encode(value).encode()).hexdigest()

def scope_key(scope):
    return (scope.profile_id, scope.project_id, scope.instance_id)

def read_facts(service, scope):
    service.repo.check_scope(scope)
    rows = service.repo.events(scope.profile_id, project_id=scope.project_id,
                               instance_id=scope.instance_id)
    result = []
    for row in rows:
        p = row['payload']; u = p.get('u2') or {}; job = p.get('job') or {}
        run = service.db.connection.execute('SELECT * FROM runs WHERE run_id=?', (row['run_id'],)).fetchone()
        issue = service.db.connection.execute('SELECT diagnostic_type FROM issues WHERE issue_id=?',
                                              (row.get('issue_id'),)).fetchone()
        result.append(dict(event_id=row['event_id'], run_id=row['run_id'],
            session_id=row['session_id'], scope=list(scope_key(scope)),
            event_type=row['event_type'], concept_ids=p['concept_ids'],
            diagnostic_type=u.get('diagnostic_type') or (issue[0] if issue else None),
            revision=run['revision'], snapshot_id=job.get('snapshot_id') or u.get('snapshot_id') or run['snapshot_id'],
            source_kind=p['evidence_kind'], source_origin=u.get('evidence_origin') or job.get('kind'),
            integrity=u.get('evidence_integrity') or 'trusted-service-dto',
            created_at=row['created_at'], issue_id=row.get('issue_id'), occurrence_id=row.get('occurrence_id'),
            job_id=job.get('job_id') or u.get('job_id'), validation=job.get('validation'),
            validation_scope=job.get('scope_ref'), complete=job.get('complete', False),
            outcome=job.get('outcome'), hint_level=u.get('card_level') or p.get('hint_level', 0),
            action_kind=u.get('action_kind'), evidence_refs=p.get('evidence_refs', []),
            policy_version=run['policy_version'], workflow_version=run['workflow_version'],
            model_version=run['model_version'], generation_version=VERSION))
    return result

def fact_text(f):
    # Every sentence is computed from the trusted event, even if a model summary contradicts it.
    kind = f['event_type']; desc = kind
    if kind in ('issue_observed', 'u2_diagnostic_observed'):
        desc = '诊断观察:' + str(f['diagnostic_type'] or '未分类')
    elif kind == 'student_modified':
        desc = '已提交修改；此事件不包含复测结论'
    elif kind == 'retry_passed':
        if (f['source_kind'] == 'real' and f['complete'] and
            f['validation'] in ('test_expectation', 'diagnostic_absence') and f['validation_scope']):
            desc = '可信复测通过，仅限:' + f['validation_scope'] + '；不代表掌握'
        else:
            desc = '模拟或范围不足的复测通过记录；不代表真实修复或掌握'
    elif kind == 'retry_failed':
        desc = '复测未通过，范围:' + str(f['validation_scope'])
    elif kind == 'hint_presented':
        desc = f"UI 回执记录提示 L{f['hint_level']} 已展示；不代表掌握"
    elif kind == 'u2_card_generated':
        desc = f"提示 L{f['hint_level']} 已生成；展示尚未确认"
    elif kind == 'u2_hint_accepted':
        desc = '已接受进一步提示请求；展示尚未确认'
    return f"[{f['event_id']}|{f['source_kind']}|{f['revision']}] {desc}"

def external_view(facts):
    # No names, learner/project IDs, source bodies, card prose, paths or tool logs leave the host.
    # Local event references are replaced by ordinal source IDs.
    return [dict(source_id=i, event_type=f['event_type'], concept_ids=f['concept_ids'],
                 diagnostic_type=f['diagnostic_type'], hint_level=f['hint_level'],
                 source_kind=f['source_kind'], validation=f['validation'],
                 complete=f['complete'], outcome=f['outcome']) for i, f in enumerate(facts)]

def search_text(facts):
    concepts = sorted({c for f in facts for c in f['concept_ids']})
    return ' '.join(concepts + [LABELS.get(c, c) for c in concepts] + [fact_text(f) for f in facts])

def iso_seconds(value):
    return datetime.fromisoformat(value.replace('Z', '+00:00')).timestamp()
