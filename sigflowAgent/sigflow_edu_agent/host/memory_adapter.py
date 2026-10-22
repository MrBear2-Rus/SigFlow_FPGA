"""Trusted operation-scoped UA-10 receiver composition, without a learning DB.

The caller supplies a context manager factory, never a shared SQLite connection.
The receiver owns durable deduplication, deletion tombstones and transaction races.
Scope checks fence delivery; they cannot undo a receiver's committed side effect.
"""
from copy import deepcopy
from threading import Event, Lock

from .teaching_memory import CONCEPTS, _SCOPE, _id, validate_fact
from ..learning import MAX_HISTORY_BYTES, bounded_history


class MemoryAdapter:
    def __init__(self, receiver_factory, scope_allowed):
        if not callable(receiver_factory) or not callable(scope_allowed):
            raise ValueError('invalid_memory_config')
        self._factory, self._allowed = receiver_factory, scope_allowed
        self._closed, self._fence = Event(), Lock()

    def _check(self, scope):
        if self._closed.is_set():
            raise ValueError('memory_closed')
        try:
            allowed = self._allowed(deepcopy(scope)) is True
        except Exception:
            raise ValueError('memory_unavailable') from None
        if self._closed.is_set():
            raise ValueError('memory_closed')
        if not allowed:
            raise ValueError('memory_scope_denied')

    def _call(self, operation, scope, payload):
        self._check(scope)
        try:
            # All three context manager phases execute in this caller's thread.
            with self._factory() as receiver:
                self._check(scope)
                if operation == 'record_teaching_fact':
                    result = receiver.record_teaching_fact(deepcopy(payload))
                else:
                    result = receiver.get_teaching_context(**deepcopy(payload))
                result = deepcopy(result)
        except Exception:
            # Receiver, factory and context-manager exit messages are private.
            self._check(scope)
            raise ValueError('memory_unavailable') from None
        self._check(scope)
        return result

    def _deliver(self, scope, result):
        self._check(scope)
        with self._fence:
            if self._closed.is_set():
                raise ValueError('memory_closed')
            return deepcopy(result)

    def record_teaching_fact(self, fact):
        fact = deepcopy(fact)
        scope = {k: fact.get(k) for k in _SCOPE} if type(fact) is dict else {}
        validate_fact(fact, scope)
        result = self._call('record_teaching_fact', scope, fact)
        if (type(result) is not dict or set(result) != {'accepted','event_id'}
                or result['accepted'] is not True or result['event_id'] != fact['event_id']):
            raise ValueError('memory_unavailable')
        return self._deliver(scope,result)

    def get_teaching_context(self, *, learner_id, project_id, concept_ids, max_bytes):
        if (not _id(learner_id) or not _id(project_id) or type(concept_ids) is not list
                or not 1 <= len(concept_ids) <= 8
                or any(type(c) is not str or c not in CONCEPTS for c in concept_ids)
                or len(set(concept_ids)) != len(concept_ids)
                or type(max_bytes) is not int or not 1 <= max_bytes <= MAX_HISTORY_BYTES):
            raise ValueError('invalid_memory_scope')
        scope = dict(learner_id=learner_id,project_id=project_id,concept_ids=deepcopy(concept_ids))
        result = self._call('get_teaching_context',scope,dict(scope,max_bytes=max_bytes))
        if (type(result) is not dict or set(result) != {*scope,'summary'}
                or any(result[k] != v for k,v in scope.items())
                or type(result['summary']) is not str
                or bounded_history(result['summary']) != result['summary']
                or len(result['summary'].encode('utf-8')) > max_bytes):
            raise ValueError('memory_unavailable')
        return self._deliver(scope,result)

    def close(self, timeout=0):
        """Fence future and in-flight returns without waiting on receiver I/O."""
        with self._fence:
            self._closed.set()
