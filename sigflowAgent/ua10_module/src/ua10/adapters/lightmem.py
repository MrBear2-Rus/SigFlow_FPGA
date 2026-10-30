"""Optional connector to the existing reviewed RecoverableIndex projection.

The application supplies a per-scope safe index factory. No framework import,
installation, native LLM call or unreviewed raw Qdrant writer is implicit.
"""
from datetime import datetime
from typing import Callable,Any
from .._core.embedding import source_ref,representation
from .._core.util import digest
from ..errors import ConfigurationError,ProjectionError

class LightMemBackend:
    experimental=True
    def __init__(self,index_factory:Callable[...,Any]):
        self.factory=index_factory;self.indexes={};self.events={}
    def sync(self,scope,facts,encoder):
        # One native index per scope AND source kind. Its safe policy must enforce
        # exact event identity on fusion and current host event liveness.
        import uuid
        for kind in {f['source_kind'] for f in facts}|{k for s,k in self.indexes if s==scope}:
            selected=[e for e in facts if e['source_kind']==kind];live={e['event_id']:e for e in selected}
            registry={source_ref(e):dict(facts=[e],text=e['summary']) for e in selected};key=(scope,kind)
            if key not in self.indexes:self.indexes[key]=self.factory(scope=scope,source_kind=kind,registry=registry,encoder=encoder)
            index=self.indexes[key]
            if not all(hasattr(index,name) for name in ('policy','recover','update','delete','get_all','search')):
                raise ConfigurationError('LightMem factory must return the reviewed recoverable index adapter')
            if tuple(index.policy.scope)!=scope or index.policy.kind!=kind:raise ConfigurationError('native index partition mismatch')
            index.policy.registry.update(registry);index.policy.is_live=lambda eid,live=live:eid in live
            index.recover();old=index.get_all()
            for row in old:
                if not set(row['payload']['evidence_ids'])<=set(live):index.delete(row['id'])
            for event in selected:
                eid=str(uuid.uuid5(uuid.NAMESPACE_URL,'ua10-module:'+event['event_id']))
                index.update(eid,payload=dict(memory=representation(event),float_time_stamp=datetime.fromisoformat(event['created_at']).timestamp(),
                    available_at_float=datetime.fromisoformat(event['available_at']).timestamp()))
            index.recover();self.events[key]=live
    def search(self,scope,question,as_of):
        index=self.indexes.get((tuple(scope),'real'))
        if index is None:return []
        # Encoder must be provided by the safe factory; model instance can be shared.
        cutoff=datetime.fromisoformat(as_of).timestamp();vector=index.embedder.embed(question)
        hits=index.search(vector,limit=24,filters={'float_time_stamp':{'lte':cutoff},'available_at_float':{'lte':cutoff}})
        return [dict(event_id=eid,score=h['score']) for h in hits for eid in h['payload']['evidence_ids']]
    def purge_event(self,event_id):
        for key,live in self.events.items():
            if event_id in live:
                live.pop(event_id)
                # The policy's liveness callback captures this same dict.
                self.indexes[key].get_all()
