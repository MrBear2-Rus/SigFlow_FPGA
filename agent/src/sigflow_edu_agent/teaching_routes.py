"""Opt-in U2 extension on U1's existing authenticated HTTP server."""
import re
from urllib.parse import parse_qs,urlsplit

from .errors import AgentError
from .teaching_store import identifier
from .teaching import SCHEMA

PREFIX='/api/v1/u2'


def capabilities(service):
    return dict(schema_version=SCHEMA,available=True,base_path=PREFIX,waiting_is_terminal=False,
        starts_jobs_without_approval=False,http_l4_available=False,
        owner_identity='UI-asserted X-Learner-ID; not independent learner authentication',
        teaching_memory_configured=service.runtime.teaching_memory_configured,
        model_available=bool(getattr(service.runtime.factory,'model_available',False)),
        course_configured=bool(getattr(service.runtime.factory,'course_enabled',False)),
        prediction_configured=bool(getattr(service.runtime.factory,'prediction_available',False)))


def _one(handler,name):
    values=handler.headers.get_all(name,[])
    if len(values)!=1:
        raise AgentError('INVALID_ARGUMENT','required teaching header missing or repeated',http_status=400)
    return identifier(values[0])


def _cursor(value):
    if type(value) is not str or re.fullmatch(r'0|[1-9][0-9]{0,18}',value) is None or int(value)>=2**63:
        raise AgentError('INVALID_ARGUMENT','invalid teaching cursor',http_status=400)
    return int(value)


def dispatch(handler,method,path):
    service=handler.state.config.teaching
    if not path.startswith(PREFIX+'/') or service is None:return False
    learner=_one(handler,'X-Learner-ID')
    parsed=urlsplit(handler.path)
    if path==PREFIX+'/sessions' and method=='POST' and not parsed.query:
        handler._respond_data(201,service.create_session(learner,dict(handler._read_json_body()),_one(handler,'Idempotency-Key')))
        return True
    match=re.fullmatch(PREFIX+r'/sessions/(sess-[1-9][0-9]{0,18})(/runs)?',path)
    if match and not parsed.query:
        sid,route=match.groups()
        if method=='GET' and not route:
            handler._respond_data(200,service.session(sid,learner));return True
        if method=='POST' and route:
            handler._respond_data(202,service.start(sid,learner,dict(handler._read_json_body()),_one(handler,'Idempotency-Key')))
            return True
    match=re.fullmatch(PREFIX+r'/runs/(run-[1-9][0-9]{0,18})(?:/(actions|cancel|events))?',path)
    if match:
        rid,route=match.groups()
        if method=='GET' and route=='events':
            try:query=parse_qs(parsed.query,keep_blank_values=True,strict_parsing=True)
            except ValueError:raise AgentError('INVALID_ARGUMENT','invalid teaching cursor',http_status=400) from None
            if set(query)-{'after','limit'} or any(len(v)!=1 for v in query.values()):
                raise AgentError('INVALID_ARGUMENT','invalid teaching cursor',http_status=400)
            after,limit=(_cursor(query.get(k,[default])[0]) for k,default in (('after','0'),('limit','100')))
            if not 1<=limit<=100:raise AgentError('INVALID_ARGUMENT','invalid teaching limit',http_status=400)
            handler._respond_data(200,service.events(rid,learner,after,limit));return True
        if not parsed.query:
            if method=='GET' and not route:
                handler._respond_data(200,service.project(rid,learner));return True
            if method=='POST' and route=='actions':
                handler._respond_data(202,service.action(rid,learner,dict(handler._read_json_body())));return True
            if method=='POST' and route=='cancel':
                if handler._read_json_body():
                    raise AgentError('INVALID_ARGUMENT','cancel body must be empty',http_status=400)
                handler._respond_data(202,service.cancel(rid,learner));return True
    return False
