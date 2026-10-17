"""Explicit real Core/tool + teaching sidecar acceptance, in an owned temp project.

No fixtures or missing-tool skips. Report is curated and never includes child
output, credentials, raw questions, or filesystem locations.
"""
import argparse
import http.client
import json
import os
from pathlib import Path
from queue import Queue, Empty
import secrets
import subprocess
import sys
import tempfile
from threading import Thread
import time

from .http import strict_json
from .acceptance_support import RunFailure, require, wait_for
from .runtime import parse_plan
from ..contracts.plan import build_plan_card


def sidecar_environment(directory, mode, inherited=None):
    env = dict(os.environ if inherited is None else inherited)
    env['PYTHONPATH'] = str(Path(__file__).resolve().parents[2])
    env.pop('SIGFLOW_U2_ANCHORS', None)
    if mode == 'rule':
        for key in ('DEEPSEEK_API_KEY', 'DEEPSEEK_BASE_URL', 'DEEPSEEK_MODEL'):
            env.pop(key, None)
        env['SIGFLOW_DEEPSEEK_CONFIG'] = str(directory / 'missing-model.json')
    return env


class Child:
    """Continuously drain pipes without retaining/logging untrusted output."""
    def __init__(self, argv, env, *, cwd=None):
        self.process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=env, cwd=cwd,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        self.lines, self.overflow = Queue(maxsize=1), False
        self.threads = [Thread(target=self._stdout, daemon=True), Thread(target=self._stderr, daemon=True)]
        for thread in self.threads:
            thread.start()

    def _stdout(self):
        try:
            raw = self.process.stdout.readline(16385)
            self.lines.put(raw)
            # Additional stdout is a protocol violation, but still drain it.
            while self.process.stdout.read(4096):
                self.overflow = True
        finally:
            # Only the reader closes its buffered stream, after its read ends.
            self.process.stdout.close()

    def _stderr(self):
        try:
            while self.process.stderr.read(4096):
                pass
        finally:
            self.process.stderr.close()

    def send(self, value):
        self.process.stdin.write(json.dumps(value).encode('utf-8') + b'\n')
        self.process.stdin.flush()

    def ready(self, deadline):
        try:
            raw = self.lines.get(timeout=max(.001, deadline-time.monotonic()))
        except Empty:
            raise RunFailure('ready_timeout') from None
        require(bool(raw) and len(raw) <= 16384 and raw.endswith(b'\n'), 'ready_invalid')
        try:
            return strict_json(raw)
        except ValueError:
            raise RunFailure('ready_invalid') from None

    def close(self, grace=5):
        if self.process.stdin and not self.process.stdin.closed:
            try:
                self.process.stdin.close()
            except OSError:
                pass
        try:
            self.process.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)
        drain_deadline = time.monotonic() + 1
        for thread in self.threads:
            thread.join(timeout=max(0, drain_deadline-time.monotonic()))
        # A descendant may still own pipe handles. Never acquire the buffered
        # stream lock from this thread: its reader owns eventual close at EOF.
        require(not any(thread.is_alive() for thread in self.threads), 'child_drain_timeout')


def close_children(children):
    """Attempt every owned child even when an earlier shutdown/drain fails."""
    error = None
    for child, grace in children:
        if child is None:
            continue
        try:
            child.close(grace=grace)
            require(child.process.returncode == 0 and not child.overflow, 'child_shutdown_failed')
        except RunFailure as failure:
            error = error or str(failure)
        except Exception:
            error = error or 'child_shutdown_failed'
    if error:
        raise RunFailure(error)


def check_ready(ready, instance, nonce, *, gateway=False):
    required = {'type', 'protocol', 'nonce', 'port'}
    required |= {'instance_id', 'project_id'} if gateway else {'agent_version'}
    require(set(ready) == required and ready.get('type') == 'ready'
            and ready.get('protocol') == 'edu.api.v1' and ready.get('nonce') == nonce,
            'ready_identity_mismatch')
    require(type(ready['port']) is int and 1 <= ready['port'] <= 65535, 'ready_invalid')
    if gateway:
        require(ready['instance_id'] == instance, 'ready_identity_mismatch')
        from .runtime import identifier
        try:
            identifier(ready['project_id'])
        except ValueError:
            raise RunFailure('ready_invalid') from None


class HTTP:
    def __init__(self, port, token, deadline):
        self.port, self.token, self.deadline = port, token, deadline

    def request(self, method, path, body=None, *, status=200, key=None, auth=True, learner=False):
        remaining = self.deadline - time.monotonic()
        require(remaining > 0, 'run_timeout')
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=min(5, remaining))
        headers = {'Accept': 'application/json'}
        if auth:
            headers['Authorization'] = 'Bearer ' + self.token
        if learner:
            headers['X-Learner-ID'] = 'integration'
        if key:
            headers['Idempotency-Key'] = key
        if body is not None:
            headers['Content-Type'] = 'application/json'
        try:
            conn.request(method, path, None if body is None else json.dumps(body).encode('utf-8'), headers)
            reply = conn.getresponse()
            parts, size = [], 0
            while True:
                require(time.monotonic() < self.deadline, 'run_timeout')
                chunk = reply.read1(min(65536, 2*1024*1024+1-size))
                if not chunk:
                    break
                parts.append(chunk)
                size += len(chunk)
                require(size <= 2*1024*1024, 'response_too_large')
            raw = b''.join(parts)
            envelope = strict_json(raw)
            require(envelope.get('schema_version') == 'edu.api.v1'
                    and all(type(envelope.get(k)) is str and envelope[k] for k in ('request_id','trace_id')),
                    'response_protocol_mismatch')
            require(reply.status == status, 'http_status_' + str(reply.status))
            if status >= 400:
                require(type(envelope.get('error')) is dict, 'response_invalid')
                return envelope['error']
            require(type(envelope.get('data')) is dict and 'error' not in envelope, 'response_invalid')
            return envelope['data']
        except (OSError, http.client.HTTPException, ValueError):
            raise RunFailure('http_unavailable_or_invalid') from None
        finally:
            conn.close()


def exercise(gateway_path, mode, timeout, directory, *, scenario='same-revision',
             courses=False, course_db=None, waves=False,predictions=False):
    deadline = time.monotonic() + timeout
    instance, nonce = 'inst-' + secrets.token_hex(12), secrets.token_hex(32)
    agent_token, gateway_ui, teaching_ui = (secrets.token_urlsafe(32) for _ in range(3))
    project = directory / 'project'
    (project / 'rtl').mkdir(parents=True)
    (project / 'rtl' / 'top.v').write_text('module top(input wire clk, input wire en, input wire d, output reg q);\nalways @(posedge clk) if(en) q <= d;\nendmodule\n')
    (project / 'sigflow.project').write_text(json.dumps(dict(build=dict(top_module=['top']),
        paths=dict(source_files=['rtl/top.v']), fpga=dict(target_profile='test-target',yosys_strategy='baseline'))))
    core = sidecar = None
    teaching = None
    execution = None
    try:
        core_env = os.environ.copy()
        bootstrap = dict(project_path=str(project), instance_id=instance, agent_token=agent_token,
                         ui_token=gateway_ui, nonce=nonce)
        if scenario == 'simulation':
            from .simulation_acceptance import simulation_environment
            core_env = simulation_environment(directory)
            bootstrap['simulation'] = True
        core = Child([str(gateway_path)], core_env, cwd=directory)
        core.send(bootstrap)
        ready = core.ready(min(deadline, time.monotonic()+20))
        check_ready(ready, instance, nonce, gateway=True)
        gateway = HTTP(ready['port'], agent_token, deadline)
        ui = HTTP(ready['port'], gateway_ui, deadline)
        health = gateway.request('GET','/api/v1/health',auth=False)
        require((health.get('instance_id'), health.get('protocol_version'), health.get('edition')) ==
                (instance, 'edu.api.v1', 'edu'), 'gateway_identity_mismatch')
        caps = gateway.request('GET','/api/v1/capabilities')
        require(any(c.get('id') == 'eda.synth' and c.get('ready') is True and
                    c.get('plugin_id') == 'eda-synth-yosys' for c in caps.get('capabilities', [])), 'yosys_unavailable')
        route = '/api/v1/projects/' + ready['project_id']
        revision = gateway.request('GET',route+'/context')['revision']
        snapshot = gateway.request('POST',route+'/snapshots',dict(expected_revision=revision,require_saved=True),status=201)['snapshot_id']
        scope = dict(project_id=ready['project_id'],revision=revision,snapshot_id=snapshot)
        plan = dict(**scope,goal='Observe synthesis task boundaries',max_jobs=1,
                    steps=[dict(step_id='synth',capability='eda.synth',provider='yosys',params={'top':'top'})],
                    expected_observations=['Read the scoped tool report; functional correctness is not evaluated'])
        package = build_plan_card(parse_plan(plan),'plan-1',0)
        base_hash = 'baseline-' + secrets.token_hex(16)
        def grant(plan_hash,steps):
            return ui.request('POST',route+'/grants',dict(plan_hash=plan_hash,revision=revision,
                snapshot_id=snapshot,max_jobs=1,steps=steps),status=201)['grant_id']
        base_grant = grant(base_hash,package.card['steps'])
        base_body = dict(snapshot_id=snapshot,expected_revision=revision,capability='eda.synth',
            plugin_id='eda-synth-yosys',grant_id=base_grant,plan_hash=base_hash,step_id='s1',params={'top_module':'top'})
        # Agent identity is never able to issue UI grants.
        gateway.request('POST',route+'/grants',dict(plan_hash=base_hash,revision=revision,snapshot_id=snapshot,max_jobs=1),status=403)
        gateway.request('POST',route+'/jobs',base_body | {'plugin_id':'eda-sim-verilator'},status=422,key='wrong-provider')
        gateway.request('POST',route+'/jobs',base_body | {'plugin_id':42},status=422,key='malformed-provider')
        require(gateway.request('GET','/api/v1/grants/'+base_grant)['used_jobs']==0 and
                not gateway.request('GET',route+'/jobs')['jobs'],'provider_denial_mutated_scope')
        baseline = gateway.request('POST',route+'/jobs',base_body,status=202,key='baseline')['job_id']
        require(gateway.request('POST',route+'/jobs',base_body,status=202,key='baseline')['job_id'] == baseline,'baseline_replay_changed_job')
        gateway.request('POST',route+'/jobs',base_body,status=403,key='baseline-duplicate')
        def job_report(job):
            record = wait_for(lambda: gateway.request('GET','/api/v1/jobs/'+job),
                lambda x:x.get('state') in {'Succeeded','Failed','Cancelled','TimedOut','Rejected'},deadline,'job_timeout')
            require(record['state'] == 'Succeeded','yosys_job_failed')
            report = gateway.request('GET','/api/v1/jobs/'+job+'/report')
            require(all(report.get(k) == v for k,v in dict(**scope,job_id=job,state='Succeeded',origin='core',
                schema_version='edu.jobreport.v1',completeness='complete',capability='eda.synth',plugin_id='eda-synth-yosys',exit_code=0).items())
                and bool(report.get('artifacts')) and bool(report.get('input_fingerprint')), 'real_report_incomplete')
            return report
        job_report(baseline)
        course_args = (['--course-db', str(course_db)] if course_db is not None else []) if courses else ['--no-course']
        sidecar = Child([sys.executable,'-P','-m','sigflow_edu_agent.host','--bootstrap-stdin','--gateway-evidence', *course_args],
            sidecar_environment(directory,mode),cwd=directory)
        sidecar.send(dict(type='sigflow-bootstrap',protocol='edu.api.v1',instance_id=instance,nonce=nonce,
            gateway_url=f'http://127.0.0.1:{ready["port"]}',gateway_token=agent_token,ui_token=teaching_ui,data_directory=str(directory/'data')))
        host_ready=sidecar.ready(min(deadline,time.monotonic()+20))
        check_ready(host_ready,instance,nonce)
        teaching=HTTP(host_ready['port'],teaching_ui,deadline)
        host_health=teaching.request('GET','/api/v1/health',auth=False)
        require(host_health.get('instance_id') == instance and host_health.get('protocol') == 'edu.api.v1'
                and host_health.get('ready') is True,'host_identity_mismatch')
        if mode == 'deepseek':
            require(host_health.get('model',{}).get('available') is True,'model_unconfigured')
        if scenario == 'simulation':
            from .simulation_acceptance import exercise_simulation
            options=dict(waves=True,instance_id=instance) if waves else {}
            if predictions:options['predictions']=True
            execution = exercise_simulation(gateway, ui, teaching, route, scope, baseline, deadline,**options)
            return execution
        approval_hash='approved-'+secrets.token_hex(16)
        create=dict(**scope,learner_id='integration',job_id=baseline,diagnostic_id='job-report',issue_id='task-observation',
            question='Explain what this verified synthesis report establishes and its functional limitations.',
            mode=mode,intent='verify',plan=plan,gateway_plan_hash=approval_hash)
        if courses:
            create['question'] += ' Use the Yosys read_verilog documentation as background.'
        def create_session(body,key):
            return teaching.request('POST','/api/v1/teaching/sessions',body,status=202,key=key,learner=True)['session_id']
        sid=create_session(create,'create')
        require(create_session(create,'create')==sid,'create_replay_changed_session')
        session='/api/v1/teaching/sessions/'+sid
        state=lambda:teaching.request('GET',session+'/state',learner=True)
        waiting=wait_for(state,lambda s:s['status'] in {'WaitingApproval','WaitingEvidence','Failed'},deadline,'approval_timeout')
        require(waiting['status']=='WaitingApproval' and waiting['cards'],'baseline_evidence_unavailable')
        initial=waiting['cards'][0]
        course_facts = None
        if courses:
            # Independently resolve only the service-admitted IDs against the
            # trusted local DB. The runner never includes their text in reports.
            from ..rag.database import configured_database
            from .course_acceptance import verify_course_background
            course_facts = verify_course_background(waiting.get('course', {}),
                course_db if course_db is not None else configured_database())
            course_facts['initial_card_citation_count'] = len(initial.get('course_refs', []))
        require(initial['producer']==('model' if mode=='deepseek' else 'rule'),'model_fallback_or_wrong_producer')
        require(waiting['plan_card']['steps']==package.card['steps'] and waiting['plan_card']['requires_approval'] is True,'plan_card_mismatch')
        require(len(gateway.request('GET',route+'/jobs')['jobs'])==1,'unapproved_job_started')
        approved_grant=grant(approval_hash,waiting['plan_card']['steps'])
        action=dict(kind='execute_plan',action_id='execute',expected_version=waiting['state_version'],grant_ref=approved_grant)
        teaching.request('POST',session+'/actions',action,status=202,learner=True)
        completed=wait_for(state,lambda s:s['status'] in {'Completed','RecoveryRequired','Failed','WaitingEvidence'},deadline,'feedback_timeout')
        while completed['status']=='RecoveryRequired' and completed.get('verification',{}).get('can_observe'):
            teaching.request('POST',session+'/actions',dict(kind='observe_verification',action_id='observe-'+completed['state_version'],
                expected_version=completed['state_version']),status=202,learner=True)
            previous=completed['state_version']
            completed=wait_for(state,lambda s:int(s['state_version'])>int(previous),deadline,'observation_timeout')
        require(completed['status']=='Completed' and completed['reason']=='outcome_indeterminate','feedback_incomplete')
        new_job=completed['verification']['job_id']
        require(new_job and new_job!=baseline and completed['verification']['result_evidence_ready'] is True,'result_not_new')
        job_report(new_job)
        feedback=completed['cards'][-1]
        require(feedback['producer']=='rule' and feedback['evidence_ids'] and
                set(feedback['evidence_ids']).isdisjoint(initial['evidence_ids']),'feedback_evidence_not_new')
        require(int(completed['state_version'])>int(waiting['state_version']),'state_version_not_advanced')
        for ref in (base_grant,approved_grant):
            require(gateway.request('GET','/api/v1/grants/'+ref)['used_jobs']==1,'grant_consumption_mismatch')
        teaching.request('POST',session+'/actions',action,status=202,learner=True)
        require(state()['state_version']==completed['state_version'],'action_replay_mutated_state')
        teaching.request('POST',session+'/actions',dict(kind='finish',action_id='stale',expected_version=waiting['state_version']),status=409,learner=True)
        # Cancel a separate waiting session before any approval: no Job may start.
        cancelled_sid=create_session(create | {'mode':'rule'},'cancel-create')
        cancel_route='/api/v1/teaching/sessions/'+cancelled_sid
        wait_for(lambda:teaching.request('GET',cancel_route+'/state',learner=True),lambda s:s['status']=='WaitingApproval',deadline,'cancel_wait_timeout')
        teaching.request('POST',cancel_route+'/cancel',{},status=202,learner=True)
        cancelled=wait_for(lambda:teaching.request('GET',cancel_route+'/state',learner=True),lambda s:s['status']=='Cancelled',deadline,'cancel_timeout')
        teaching.request('POST',cancel_route+'/cancel',{},status=202,learner=True)
        require(len(gateway.request('GET',route+'/jobs')['jobs'])==2,'cancel_or_replay_started_job')
        execution = dict(status='passed',real_core=True,real_yosys=True,model_mode=mode,
            initial_producer=initial['producer'],feedback_producer=feedback['producer'],model_calls=completed['model_calls'],
            evidence_origin=completed['evidence_origin'],baseline_job_id=baseline,result_job_id=new_job,
            baseline_evidence_ids=initial['evidence_ids'],result_evidence_ids=feedback['evidence_ids'],
            waiting_version=waiting['state_version'],completed_version=completed['state_version'],
            completed_status=completed['status'],reason=completed['reason'],job_count=2,grant_used_jobs=[1,1],
            action_replay='verified',stale_version='denied',cancel_before_approval=cancelled['status'],
            functional_correctness='not_evaluated',limits=['real_simulation','cross_revision_student_edit','GUI','teaching_correctness'])
        if scenario == 'student-edit':
            from .revision_acceptance import exercise_student_edit
            edited = exercise_student_edit(core, project, route, gateway, ui, teaching,
                                           scope, plan, create, deadline)
            execution['student_edit'] = edited
            execution['job_count'] = 4
            execution['grant_used_jobs'] += edited['grant_used_jobs']
            execution['limits'].remove('cross_revision_student_edit')
        if course_facts is not None:
            execution['course'] = course_facts
        return execution
    finally:
        if teaching:
            try:
                teaching.deadline=time.monotonic()+2
                teaching.request('POST','/api/v1/shutdown',{},status=202)
            except Exception:
                pass
        try:
            close_children(((sidecar, 3), (core, 10)))
        except RunFailure as failure:
            failure.execution = execution
            raise


def failed_report(report, code, execution):
    # Execution may already have succeeded when teardown or publication fails.
    # Keep those facts under their own field; acceptance always fails closed.
    failed = dict(status='failed', model_mode=report.get('model_mode', 'rule'),
                  functional_correctness='not_evaluated', error=code)
    if execution:
        failed['execution'] = {k:v for k,v in execution.items() if k not in ('status','cleanup','error')}
    else:
        failed.update(real_core=False, real_yosys=False)
    return failed


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gateway',required=True,type=Path)
    parser.add_argument('--mode',choices=('rule','deepseek'),default='rule')
    parser.add_argument('--scenario',choices=('same-revision','student-edit','simulation'),default='same-revision')
    parser.add_argument('--courses',action='store_true')
    parser.add_argument('--waves',action='store_true')
    parser.add_argument('--predictions',action='store_true')
    parser.add_argument('--course-db',type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--timeout',type=float,default=120)
    args=parser.parse_args(argv)
    if args.course_db is not None and not args.courses:
        parser.error('course_db_requires_courses')
    if args.scenario == 'simulation' and (args.mode != 'rule' or args.courses):
        parser.error('simulation_requires_rule_without_courses')
    if args.waves and args.scenario!='simulation':
        parser.error('waves_requires_simulation')
    if args.predictions and (not args.waves or args.scenario!='simulation'):
        parser.error('predictions_requires_simulation_and_waves')
    report=dict(status='failed',real_core=False,real_yosys=False,model_mode=args.mode,
                functional_correctness='not_evaluated')
    execution = None
    try:
        require(args.gateway.is_file(),'gateway_missing')
        require(0 < args.timeout <= 600,'invalid_timeout')
        with tempfile.TemporaryDirectory(prefix='sigflow-u2-closed-loop-') as owned:
            options = dict(scenario=args.scenario)
            if args.waves:options['waves']=True
            if args.predictions:options['predictions']=True
            if args.courses:
                options.update(courses=True, course_db=args.course_db.resolve() if args.course_db else None)
            execution=exercise(args.gateway.resolve(),args.mode,args.timeout,Path(owned),**options)
            report=execution.copy()
        report['cleanup']='completed'
    except RunFailure as failure:
        execution = failure.execution or execution
        report = failed_report(report, str(failure), execution)
    except Exception:
        report = failed_report(report, 'integration_failed', execution)
    raw=json.dumps(report,ensure_ascii=True,sort_keys=True,indent=2)+'\n'
    try:
        args.output.write_text(raw,encoding='utf-8')
    except Exception:
        report = failed_report(report, 'report_write_failed', execution)
        raw=json.dumps(report,ensure_ascii=True,sort_keys=True,indent=2)+'\n'
    print(raw,end='')
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':
    raise SystemExit(main())
