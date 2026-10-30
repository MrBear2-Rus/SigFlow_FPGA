"""FTS5 + actual BGE + course graph, RRF; trust/time before ALL candidates."""
from .util import *
from .store import utc,EvidenceViolation
from .embedding import source_ref
import re,time

CONCEPTS={
 'latch':['latch','锁存','赋值不完整','所有赋值路径','分支没给值','always_comb','记住上一次','保持旧值','默认分支','case'],
 'reset':['reset','复位','清零','初始值','初值','上电'],
 'blocking_nonblocking':['非阻塞','nonblocking','更新顺序','always_ff','阻塞赋值'],
 'bit_width':['width','位宽','截断','高位','truncat','narrow','extend']}
V2_ADDITIONS={'latch':['retains','previous value','path leaves','沿用','前一次输出','分支遗漏','路径遗漏','组合运算','组合逻辑'],
 'reset':['counter starts','initial value','回到零'], 'blocking_nonblocking':['读到旧数','old value after the clock'],
 'bit_width':['most significant bits','bits vanished','装不下']}
LABELS={'latch':'latch 锁存器 组合逻辑 完整赋值 分支 默认值', 'reset':'reset 复位 计数器 清零 极性',
 'blocking_nonblocking':'nonblocking 阻塞 非阻塞 时序 更新顺序', 'bit_width':'bit width 位宽 截断 高位 扩展'}
def index_text(e,representations):
    return representations.get(e['event_id'],e['summary'])+' [SRC:'+source_ref(e)+']'
def intent(question,version,options):
    q=question.lower(); concepts=[]
    for c,words in CONCEPTS.items():
        if any(w in q for w in words+(V2_ADDITIONS[c] if version>=2 else [])): concepts.append(c)
    # A signal name is not a diagnosis: stronger width language wins lexical association.
    if 'bit_width' in concepts and any(t in q for t in ('latch_count','高位','most significant')): concepts=['bit_width']
    state=dict(concepts=concepts,kind='diagnostic',revision=None,current=options.get('current_only',False),reject=False,absence=False)
    r=re.search(r'\br\d+\b',q)
    if r: state['revision']=r.group()
    if options.get('revision'): state['revision']=options['revision']
    if options.get('require_mastery'): state['reject']=True
    if version>=2:
        if any(x in q for x in ('password','密码','wifi','pll','phase noise','完全掌握','完全独立','掌握所有')): state['reject']=True
        state['absence']=bool(re.search(r'没有推断|未检出|no latch|without.*latch|没有.*锁存',q))
        if state['absence']: state['kind']='absence'
        elif any(w in q for w in ('复测','通过','retest','验证结果','当前记录')): state['kind']='outcome'
        elif any(w in q for w in ('修改','改过')): state['kind']='modified'
        elif any(w in q for w in ('提示','hint','展示')): state['kind']='help'
        if any(w in q for w in ('最新','当前记录','现在又失败')): state['current']=True
    return state
def grams(text):
    words=re.findall(r'[a-zA-Z_]{3,}|[\u4e00-\u9fff]+',text.lower()); terms=[]
    for w in words:
        if re.search('[\u4e00-\u9fff]',w): terms.extend(w[i:i+3] for i in range(max(0,len(w)-2)))
        else: terms.append(w)
    return list(dict.fromkeys(terms))[:48]

class Hybrid:
    def __init__(self,store,encoder,representations,backend=None):
        self.store=store; self.encoder=encoder; self.representations=representations; self.backend=backend
        db=store.db
        db.executescript('''CREATE TABLE IF NOT EXISTS representations(event_id TEXT PRIMARY KEY REFERENCES events(id) ON DELETE CASCADE,
          text TEXT,content_hash TEXT,model_id TEXT,preprocessing TEXT);
          CREATE VIRTUAL TABLE IF NOT EXISTS memory_fts USING fts5(event_id UNINDEXED,text,tokenize='trigram');
          CREATE TRIGGER IF NOT EXISTS remove_fts AFTER DELETE ON representations BEGIN DELETE FROM memory_fts WHERE event_id=OLD.event_id; END;''')
        for raw in db.execute('SELECT data FROM events').fetchall():
            e=json.loads(raw[0]); text=representations.get(e['event_id'],e['summary']); h=digest(text)
            old=db.execute('SELECT content_hash,model_id,preprocessing FROM representations WHERE event_id=?',(e['event_id'],)).fetchone()
            if old and tuple(old)==(h,encoder.weights,encoder.spec.preprocessing_version): continue
            if self.encoder.available: self.encoder.encode(index_text(e,representations))
            with db:
                db.execute('INSERT OR REPLACE INTO representations VALUES (?,?,?,?,?)',(e['event_id'],text,h,encoder.weights,encoder.spec.preprocessing_version))
                db.execute('DELETE FROM memory_fts WHERE event_id=?',(e['event_id'],))
                db.execute('INSERT INTO memory_fts VALUES (?,?)',(e['event_id'],text+' '+LABELS.get(e['concept_ids'][0] if e['concept_ids'] else None,'')))
    def search(self,scope,question,as_of,*,version=2,k=6,options=None,source_kind='real'):
        started=time.perf_counter(); options=options or {}; state=intent(question,version,options)
        facts=self.store.events(scope,as_of,source_kind); byid={e['event_id']:e for e in facts}
        v=self.encoder.encode(question) if self.encoder.available else None
        if self.backend:
            dense=self.backend(scope,question,as_of)
            dense=[r for r in dense if r['event_id'] in byid]
        elif v is not None:
            dense=sorted([dict(event_id=e['event_id'],score=float(sum(a*b for a,b in zip(v,self.encoder.encode(index_text(e,self.representations)))))) for e in facts],key=lambda r:(-r['score'],r['event_id']))[:24]
        else: dense=[]
        query_words=grams(question+' '+' '.join(LABELS[c] for c in state['concepts']))
        lexical=[]
        if query_words:
            match=' OR '.join('"'+t.replace('"','""')+'"' for t in query_words)
            # FTS hard constraints are SQL joins on authoritative events, before rank/limit.
            rows=self.store.db.execute('''SELECT f.event_id,bm25(memory_fts) FROM memory_fts f JOIN events e ON e.id=f.event_id
               WHERE memory_fts MATCH ? AND e.scope=? AND e.kind=? AND e.occurred<=? AND e.available<=?
               ORDER BY bm25(memory_fts),f.event_id LIMIT 24''',(match,serial(list(scope)),source_kind,utc(as_of),utc(as_of))).fetchall()
            lexical=[dict(event_id=i,score=float(s)) for i,s in rows if i in byid]
        graph=[dict(event_id=e['event_id'],score=1.) for e in facts if set(e['concept_ids'])&set(state['concepts'])]
        fused={}
        for channel,items,weight in [('bge',dense,1.),('fts',lexical,.8),('course',graph,.7)]:
            for rank,row in enumerate(items):
                r=fused.setdefault(row['event_id'],dict(event_id=row['event_id'],rrf=0.,components={}))
                r['rrf']+=weight/(60+rank+1); r['components'][channel]=dict(rank=rank+1,score=row['score'])
        raw=sorted(fused.values(),key=lambda r:(-r['rrf'],r['event_id'])); post=[]
        latest={}
        for e in facts: latest[e['issue_id']]=max(latest.get(e['issue_id'],('','')),(e['created_at'],e['revision']))
        for r in raw:
            e=byid[r['event_id']]
            if state['revision'] and e['revision']!=state['revision']: continue
            if state['current'] and e['revision']!=latest[e['issue_id']][1]: continue
            if state['kind']=='diagnostic' and e['event_type'] not in ('issue_observed','u2_diagnostic_observed'): continue
            if state['kind']=='outcome' and e['event_type'] not in ('retry_passed','retry_failed'): continue
            if state['kind']=='absence' and e['diagnostic_present'] is not False: continue
            if state['kind']=='modified' and e['event_type']!='student_modified': continue
            if state['kind']=='help' and e['event_type'] not in ('hint_presented','u2_card_generated'): continue
            post.append(dict(r,fact=e))
        # Agreement is confidence, not a concept permission gate. Weak lexical
        # matches (e.g. a signal named latch_count) do not count as corroboration.
        uncertain=not state['concepts'] or len(state['concepts'])>1
        technical=any(w in question.lower() for words in CONCEPTS.values() for w in words)
        if version>=2:
            best=max([abs(r['components'].get('fts',{}).get('score',0)) for r in post] or [0])
            confident=[]
            for r in post:
                c=r['components']; votes=int('bge' in c)+int('course' in c)+int('fts' in c and abs(c['fts']['score'])>=best*.25 and best>0)
                r['confidence_channels']=votes
                if votes>=2: confident.append(r)
            uncertain=uncertain or not technical or not confident
            selected=[] if state['reject'] or uncertain else confident[:k]
        else: selected=[] if state['reject'] else post[:k]
        return dict(question=question,scope=list(scope),as_of=as_of,intent=state,raw=raw,post=post,selected=selected,
          abstained=not bool(selected),needs_semantic=not state['reject'] and uncertain,
          latency_ms=(time.perf_counter()-started)*1000,version=version)

def rerank_messages(trace):
    candidates=[dict(event_id=r['event_id'],fact=dto_for_rank(r['fact'])) for r in trace['post'][:10]]
    return [dict(role='system',content=('判断合成教学问题与给定历史候选的相关性。只能选候选event_id，不得扩大scope或创造事实。'
      '结合问题语义判断，不要仅因同一关键词就相关。否定诊断不是诊断观察；当前/最新只取该issue最新版本。'
      '若无关或证据不足，related=false,selected_ids=[]。JSON: '
      '{"related":true,"selected_ids":["id"],"temporal_relation":"historical|latest_history|unknown",'
      '"time_conflict":false,"reason":"简短理由"}。最多6个ID。最新历史不代表当前工程。')),
      dict(role='user',content=serial(dict(question=trace['question'],candidates=candidates)))]
def dto_for_rank(e):
    return {k:e[k] for k in ('event_type','concept_ids','diagnostic_type','diagnostic_present','run_id','revision','created_at','summary')}
def apply_rerank(trace,value):
    if set(value)!= {'related','selected_ids','temporal_relation','time_conflict','reason'}: raise EvidenceViolation('rerank_schema')
    source={r['event_id']:r for r in trace['post'][:10]}; ids=value['selected_ids']
    if type(value['related']) is not bool or not isinstance(ids,list) or len(ids)>6 or len(set(ids))!=len(ids) or not set(ids)<=set(source): raise EvidenceViolation('rerank_identity')
    if not value['related'] and ids: raise EvidenceViolation('rerank_contradiction')
    if value['temporal_relation'] not in ('historical','latest_history','unknown'): raise EvidenceViolation('rerank_time')
    trace=dict(trace,selected=[source[i] for i in ids],abstained=not bool(ids),semantic_decision=value); return trace
