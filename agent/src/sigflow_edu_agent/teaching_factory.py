"""Trusted bootstrap composition, loaded only when U2 is explicitly enabled."""
import hashlib
from pathlib import Path
import threading

from .errors import AgentError
from .gateway_client import GatewayClient
from .teaching import TeachingService
from .teaching_store import BindingStore,identifier


def compose(bootstrap,*,teaching_memory=None,reference_provider=None,receipt_port=None):
    # Optional imports keep the original Python 3.9 U1 base independent of U2.
    from sigflow_u2.clients.gateway import GatewayClient as EvidenceClient,GatewayConfig
    from sigflow_u2.clients.waveform import GatewayWaveformReader
    from sigflow_u2.courses import SQLiteCourseProvider
    from sigflow_u2.host.factory import make_factory,make_report_resolver
    from sigflow_u2.host.gateway_evidence import GatewayEvidenceCatalog
    from sigflow_u2.host.runtime import Runtime
    from sigflow_u2.host.shared_adapter import SharedTeachingAdapter
    from sigflow_u2.host.store import DurableStore
    from sigflow_u2.host.verification import GatewayVerification
    from sigflow_u2.runtime.verification_checkpoint import decode_verification_checkpoint
    from urllib.parse import urlsplit

    identifier(bootstrap.instance_id)
    location=urlsplit(bootstrap.gateway_url)
    if location.scheme!='http' or location.hostname!='127.0.0.1' or location.port is None:
        raise ValueError('U2 requires the canonical IPv4 loopback Gateway')
    directory=Path(bootstrap.data_directory)
    if not directory.is_absolute():raise ValueError('U2 requires an absolute trusted data directory')
    directory=directory/'u1-u2'
    directory.mkdir(parents=True,exist_ok=True)
    filename=hashlib.sha256(bootstrap.instance_id.encode()).hexdigest()[:32]
    config=GatewayConfig(location.port,bootstrap.gateway_token,bootstrap.instance_id)
    gateway=GatewayClient(bootstrap.gateway_url,bootstrap.gateway_token,secrets=bootstrap.secrets)
    client=EvidenceClient(config)
    catalog=GatewayEvidenceCatalog(config)
    store=bindings=runtime=verification=None

    def unwrap(outcome):
        if not outcome.ok:
            code=outcome.code if outcome.code in ('NOT_FOUND','POLICY_DENIED','STALE_REVISION') else 'GATEWAY_UNAVAILABLE'
            status=404 if code=='NOT_FOUND' else 403 if code=='POLICY_DENIED' else 409 if code=='STALE_REVISION' else 503
            raise AgentError(code,'current engineering evidence unavailable',http_status=status)
        return outcome.report

    def current(session):
        try:client.health()
        except ValueError:raise AgentError('GATEWAY_UNAVAILABLE','Gateway identity unavailable',http_status=503) from None
        data=unwrap(gateway.project(session['project_id']))
        if (data.get('project_id')!=session['project_id'] or data.get('revision')!=session['revision']
                or data.get('dirty') is not False or data.get('synchronized') is not True):
            raise AgentError('STALE_REVISION','saved synchronized project revision required',http_status=409)

    def prepare(session,body):
        report=unwrap(gateway.fetch_job_report(body['job_id']))
        if (report.get('project_id')!=session['project_id'] or report.get('revision')!=session['revision']
                or (body.get('snapshot_id') is not None and body['snapshot_id']!=report.get('snapshot_id'))):
            raise AgentError('POLICY_DENIED','report scope rejected',http_status=403)
        scope=dict(job_id=body['job_id'],snapshot_id=report.get('snapshot_id'),
            diagnostic_id=body.get('diagnostic_id','job-report'),issue_id=body['issue_id'])
        try:
            catalog.collect(scope['job_id'],project_id=session['project_id'],revision=session['revision'],
                snapshot_id=scope['snapshot_id'],diagnostic_id=scope['diagnostic_id'])
        except ValueError:raise AgentError('GATEWAY_UNAVAILABLE','report evidence not certified',http_status=503) from None
        return scope

    def adapter(context,sid=None):
        # Fresh resolver per projection. Only committed verification results may
        # add Job candidates; no caller evidence ID expands the trusted scope.
        jobs=[context.job_id]
        if sid is not None:
            row=runtime.store.get(sid)
            if row['intent']=='verify' and row.get('checkpoint'):
                checkpoint,_=decode_verification_checkpoint(row['checkpoint'])
                jobs.extend(item['job_id'] for item in checkpoint['results'])
        cache={}
        def resolve(eid):
            for job in dict.fromkeys(jobs):
                if job not in cache:
                    cache[job]=catalog.collect(job,project_id=context.project_id,revision=context.revision,
                        snapshot_id=context.snapshot_id,diagnostic_id=context.diagnostic_id if job==context.job_id else 'job-report')
                if cache[job].evidence_id==eid:return cache[job]
            return None
        return SharedTeachingAdapter(runtime,resolve,secrets=bootstrap.secrets)

    def close_verification():
        def close():
            try:verification.close()
            except Exception:pass
        thread=threading.Thread(target=close,daemon=True)
        thread.start();thread.join(.1)

    try:
        store=DurableStore(directory/(filename+'-runtime.sqlite3'),bootstrap.instance_id)
        bindings=BindingStore(directory/(filename+'-bindings.sqlite3'),bootstrap.instance_id)
        verification=GatewayVerification(config,directory/(filename+'-verification.sqlite3'),
            make_report_resolver(config,catalog))
        factory=make_factory(config,anchors=catalog,verification=verification,
            course_provider=SQLiteCourseProvider(),waveform_reader=GatewayWaveformReader(config))
        runtime=Runtime(store,bootstrap.instance_id,factory,teaching_memory=teaching_memory,
            reference_provider=reference_provider,receipt_port=receipt_port)
        return TeachingService(runtime,bindings,prepare,adapter,current,
            resource_closers=(store.close,close_verification))
    except Exception:
        if runtime:runtime.close()
        if bindings:bindings.close()
        if store:store.close()
        if verification:close_verification()
        raise
