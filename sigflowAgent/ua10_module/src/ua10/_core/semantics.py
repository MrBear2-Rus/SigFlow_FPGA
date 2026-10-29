from .util import *
from .store import Store,utc,after,EvidenceViolation,synchronized
from datetime import datetime,timezone
import re,uuid
def now(): return datetime.now(timezone.utc).isoformat(timespec='microseconds')
def dto(e):
    keys=('event_id','run_id','revision','source_kind','created_at','event_type','concept_ids','diagnostic_type',
          'diagnostic_present','hint_level','validation_scope','complete','outcome','summary')
    return {k:e[k] for k in keys}
def boundary(text):
    forbidden=(r'已(?:经)?完全掌握',r'已证实.{0,8}不掌握',r'完全独立完成',r'同一个持续未修复')
    for pattern in forbidden:
        for m in re.finditer(pattern,text):
            if not re.search(r'不能|无法|不证明|不代表|未证实|没有证据',text[max(0,m.start()-12):m.start()]): raise EvidenceViolation('unsupported_ability_or_issue_claim')
def validate_extraction(value,facts):
    if set(value)!= {'memories'} or not isinstance(value['memories'],list): raise EvidenceViolation('extract_schema')
    source={e['event_id']:e for e in facts}; result={}
    for r in value['memories']:
        if set(r)!= {'event_id','run_id','revision','source_kind','summary'}: raise EvidenceViolation('extract_fields')
        eid=r['event_id']
        if eid not in source or eid in result: raise EvidenceViolation('extract_identity')
        for key in ('run_id','revision','source_kind'):
            if r[key]!=source[eid][key]: raise EvidenceViolation('extract_'+key)
        t=r['summary']; boundary(t)
        if not isinstance(t,str) or not 1<=len(t)<=240: raise EvidenceViolation('extract_length')
        if source[eid]['event_type']=='u2_card_generated' and '已展示' in t and not re.search('未|不|未知|确认',t): raise EvidenceViolation('generated_is_not_shown')
        result[eid]=t
    if set(result)!=set(source): raise EvidenceViolation('extract_missing_events')
    return result

PATTERN_SCHEMA={
 'observations':[{'kind':'recurrence|status_transition|assistance_history|pending_validation|mixed_outcomes|validation_history|cross_concept',
     'text':'bounded derived description','event_ids':['given IDs']}],
 'difficulties':[{'description':'hypothesis, not proven ability','event_ids':['given IDs'],'certainty':'hypothesis'}],
 'teaching_strategies':[{'suggestion':'recommended next teaching action','event_ids':['given IDs'],'kind':'recommendation',
     'course_links':['complete_assignment']}],
 'unsupported_conclusions':['mastery','independence','current_pass_from_history'],
 'uncertainty':['explicit missing evidence'],
 'evidence':[{'event_id':'given','run_id':'given','revision':'given','source_kind':'given','created_at':'given'}]}
COURSE_LINKS={'latch':{'complete_assignment'},'reset':{'reset_polarity'},
    'blocking_nonblocking':{'assignment_order'},'bit_width':{'bit_width'}}
def pattern_messages(facts,version):
    system=('你是长期教学记忆的派生规律整理器。所有输入都是synthetic教学fixture，非真实学生。原始事实不可改写。'
      '请输出严格JSON，区分 observations派生观察、difficulties假设、teaching_strategies建议；不输出掌握结论。'
      '不要只计次数，要解释不同练习反复出现的困难、状态变化、仍缺什么证据及下次可采用的分级教学方法。'
      '不同issue/run保持独立；旧版本通过不能覆盖当前状态；生成提示不等于展示，没帮助记录不等于独立。'
      '每个观察/假设/建议都引用输入event_id；evidence列出这些引用的精确原始身份和时间，禁止创造ID。'
      'observations kind从给定枚举中选，最多3项；difficulties最多2项；strategies最多2项。'
      'unsupported_conclusions必须包含mastery、independence、current_pass_from_history。'
      'Schema='+serial(PATTERN_SCHEMA))
    if version>=2:
        system+=(' 课程关联只能来自受信图谱，且必须匹配该条建议所引用事件的概念：'+serial({k:sorted(v) for k,v in COURSE_LINKS.items()})+
          'Schema中的complete_assignment仅为示例，不是所有概念的默认关联。')
        system+=(' 进一步约束：不要把无诊断的通过当成诊断观察。pending_validation只描述对应run当时缺少复测，'
          '不能否认其他run后来的验证。assistance_history区分生成和真实UI回执。建议是将来的建议，'
          '不能说学生已经需要L3或已经获得反馈。身份记录只列被引用事件；描述中的r1/r2等版本要与引用匹配。'
          '为保持预算，每项text/description/suggestion最多80汉字，引用可复用。')
        system+=(' kind契约：recurrence要求至少两个不同exercise的issue_observed，重复run不自动等于不同exercise；'
          'status_transition只描述同一可信issue跨revision的retry_passed/retry_failed相反结果，不能用不同exercise来拼接。'
          'mixed_outcomes必须有retry_passed和retry_failed各至少一个；issue_observed.outcome=failed不是retry_failed。'
          '只有通过而无复测失败、或不同练习的验证历史请用validation_history。pending_validation必须引用student_modified。'
          'evidence所有身份必须精确复制，字段created_at按原字符串复制。')
    return [dict(role='system',content=system),dict(role='user',content=serial(dict(synthetic=True,events=[dto(e) for e in facts])))]
def validate_pattern(value,facts):
    if set(value)!=set(PATTERN_SCHEMA): raise EvidenceViolation('pattern_schema')
    byid={e['event_id']:e for e in facts}; identities={}; refs=set()
    for r in value['evidence']:
        if set(r)!= {'event_id','run_id','revision','source_kind','created_at'} or r['event_id'] not in byid: raise EvidenceViolation('pattern_identity')
        e=byid[r['event_id']]
        for k in ('run_id','revision','source_kind'):
            if r[k]!=e[k]: raise EvidenceViolation('pattern_'+k)
        if utc(r['created_at'])!=utc(e['created_at']): raise EvidenceViolation('pattern_time')
        if r['event_id'] in identities: raise EvidenceViolation('duplicate_identity')
        identities[r['event_id']]=e
    for collection,textkey in (('observations','text'),('difficulties','description'),('teaching_strategies','suggestion')):
        if not isinstance(value[collection],list) or len(value[collection])>3: raise EvidenceViolation('unbounded_claims')
        for item in value[collection]:
            ids=item.get('event_ids'); text=item.get(textkey)
            if not isinstance(ids,list) or not ids or len(ids)!=len(set(ids)) or not set(ids)<=set(identities): raise EvidenceViolation('claim_references')
            if not isinstance(text,str) or not 1<=len(text)<=300: raise EvidenceViolation('claim_text')
            boundary(text); refs.update(ids); events=[byid[i] for i in ids]
            # No free identity/version invention inside prose.
            if any(i not in byid for i in re.findall(r'\b[A-Z]\d\d-[a-z]+\b',text)): raise EvidenceViolation('prose_identity')
            # Unicode \b treats adjacent Chinese characters as word characters;
            # it misses "对比r2通过". Only ASCII identifier boundaries apply.
            if not set(re.findall(r'(?<![A-Za-z0-9_])r\d+(?![A-Za-z0-9_])',text))<={e['revision'] for e in events}: raise EvidenceViolation('prose_revision')
            if collection=='observations':
                kind=item.get('kind')
                if kind=='recurrence' and len({e['exercise_id'] for e in events if e['event_type']=='issue_observed' and e['diagnostic_present']})<2: raise EvidenceViolation('recurrence_not_multiple_exercises')
                if kind=='status_transition':
                    outcomes=[e for e in events if e['event_type'] in ('retry_passed','retry_failed')]
                    if len({e['issue_id'] for e in outcomes})!=1 or len({e['revision'] for e in outcomes})<2 or len({e['outcome'] for e in outcomes})<2: raise EvidenceViolation('transition_not_supported')
                elif kind=='assistance_history' and not any(e['event_type'] in ('hint_presented','u2_card_generated') for e in events): raise EvidenceViolation('no_assistance_event')
                elif kind=='pending_validation' and not any(e['event_type']=='student_modified' for e in events): raise EvidenceViolation('no_modification')
                elif kind=='mixed_outcomes' and len({e['outcome'] for e in events if e['event_type'] in ('retry_passed','retry_failed')})<2: raise EvidenceViolation('no_mixed_outcomes')
                elif kind=='validation_history' and not any(e['event_type'] in ('retry_passed','retry_failed') for e in events): raise EvidenceViolation('no_validation_history')
                elif kind=='cross_concept' and len({c for e in events for c in e['concept_ids']})<2: raise EvidenceViolation('no_cross_concept_evidence')
                elif kind not in ('recurrence','status_transition','assistance_history','pending_validation','mixed_outcomes','validation_history','cross_concept'): raise EvidenceViolation('observation_kind')
            if collection=='difficulties' and item.get('certainty')!='hypothesis': raise EvidenceViolation('difficulty_not_hypothesis')
            if collection=='teaching_strategies':
                if item.get('kind')!='recommendation': raise EvidenceViolation('strategy_not_recommendation')
                links=item.get('course_links')
                allowed={link for e in events for c in e['concept_ids'] for link in COURSE_LINKS.get(c,set())}
                if not isinstance(links,list) or not set(links)<=allowed: raise EvidenceViolation('untrusted_course_relation')
    # Extra identity rows are individually verified above. They are not claims or
    # dependencies and must NOT force an otherwise valid response to fail. Only
    # actually cited evidence is committed to the dependency graph.
    if not value['observations'] or not refs or not refs<=set(identities): raise EvidenceViolation('missing_evidence')
    if not {'mastery','independence','current_pass_from_history'}<=set(value['unsupported_conclusions']): raise EvidenceViolation('missing_uncertainty_boundary')
    if len({tuple(e['scope']) for e in facts})!=1 or len({e['source_kind'] for e in facts})!=1: raise EvidenceViolation('mixed_trust_input')
    return sorted(refs)

class PatternStore(Store):
    @synchronized
    def patterns(self,scope,as_of,source_kind='real',concept=None):
        result=[]
        for p in super().patterns(scope,as_of,source_kind,concept):
            if p.get('prompt_version','').startswith('llm-pattern-v'):
                events=[self.event(i) for i in p['supporting_event_ids']]
                raw={k:p[k] for k in ('observations','difficulties','teaching_strategies','unsupported_conclusions','uncertainty')}
                raw['evidence']=[{k:e[k] for k in ('event_id','run_id','revision','source_kind','created_at')} for e in events]
                try: validate_pattern(raw,events)
                except EvidenceViolation: continue  # Historical raw response remains in artifacts; cannot enter new context.
                p=dict(p,needs_teacher_review=True)
            result.append(p)
        return result
    def append(self,e):
        # Reuse the SAME authority schema and base append contract. Extra concept
        # invalidation runs in the same transaction via a SQLite trigger.
        with self.lock,self.db:
            self.db.executescript('''CREATE TRIGGER IF NOT EXISTS cross_concept_invalidation AFTER INSERT ON events BEGIN
              UPDATE patterns SET valid_until=NEW.available WHERE scope=NEW.scope AND kind=NEW.kind AND valid_until IS NULL
                AND EXISTS (SELECT 1 FROM json_each(patterns.data,'$.concept_ids') WHERE value=NEW.concept);
              END;''')
        return super().append(e)
    def commit_pattern(self,value,facts,version,attempt_id,*,model_id='unknown-injected-provider'):
        refs=validate_pattern(value,facts); created=now(); scope=facts[0]['scope']; source=facts[0]['source_kind']
        concepts=sorted({c for e in facts for c in e['concept_ids']}); pid='llmp-'+digest([value,created])[:18]
        summary=' '.join(o['text'] for o in value['observations'])
        p=dict(pattern_id=pid,trusted_scope=scope,concept_ids=concepts,diagnostic_types=sorted({e['diagnostic_type'] for e in facts}),
           supporting_event_ids=refs,supporting_run_ids=sorted({e['run_id'] for e in facts if e['event_id'] in refs}),
           time_range=[min(e['created_at'] for e in facts if e['event_id'] in refs),max(e['created_at'] for e in facts if e['event_id'] in refs)],
           derived_summary=summary,uncertainty=value['uncertainty'],unsupported_conclusions=value['unsupported_conclusions'],
           observations=value['observations'],difficulties=value['difficulties'],teaching_strategies=value['teaching_strategies'],
           model_id=model_id,prompt_version='llm-pattern-v'+str(version),source_kind=source,
           created_at=created,updated_at=created,expires_at=after(created,90*86400),synthetic=all(e.get('synthetic',False) for e in facts),api_attempt_id=attempt_id,
           available_at=created,valid_from=created,input_event_ids=sorted(e['event_id'] for e in facts))
        with self.lock:
            try:
                self._tx()
                present=self.events(scope,created,source)
                expected={e['event_id'] for e in present if set(e['concept_ids'])&set(concepts)}
                if expected!={e['event_id'] for e in facts}: raise EvidenceViolation('pattern_snapshot_changed_or_incomplete')
                current=[self.event(i) for i in refs]
                if any(e is None or tuple(e['scope'])!=tuple(scope) or e['source_kind']!=source for e in current): raise EvidenceViolation('evidence_deleted_or_changed_during_llm')
                if any(digest(self.event(e['event_id']))!=digest(e) for e in facts): raise EvidenceViolation('facts_changed_during_llm')
                for oldid,data in self.db.execute('SELECT id,data FROM patterns WHERE scope=? AND kind=? AND valid_until IS NULL',(serial(scope),source)).fetchall():
                    old=json.loads(data)
                    if old['concept_ids']==concepts: self.db.execute('UPDATE patterns SET valid_until=? WHERE id=?',(created,oldid))
                self.db.execute('INSERT INTO patterns VALUES (?,?,?,?,?,NULL,?,?)',(pid,serial(scope),source,concepts[0],created,p['expires_at'],serial(p)))
                self.db.executemany('INSERT INTO dependencies VALUES (?,?)',[(pid,eid) for eid in refs]); self.db.commit()
            except BaseException: self.db.rollback(); raise
        return p
