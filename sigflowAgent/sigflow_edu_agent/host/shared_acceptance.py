"""Explicit real Core -> formal U1 -> U2 acceptance in an owned project.

No model calls, fixture success, raw child output or credentials in the report.
The existing U1 server is restarted with the same trusted instance and data.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import sys
import tempfile
import time

from .acceptance_support import RunFailure,require,wait_for
from .closed_loop import Child,HTTP,check_ready,close_children,sidecar_environment
from .runtime import parse_plan
from ..contracts.plan import build_plan_card

PREFIX='/api/v1/u2'


def u1_environment(directory,u1_source=None):
    root=Path(__file__).resolve().parents[3]
    source=Path(u1_source) if u1_source is not None else root/'agent/src'
    require((source/'sigflow_edu_agent/__main__.py').is_file(),'u1_source_missing')
    env=sidecar_environment(directory,'rule')
    for key in tuple(env):
        if key.startswith(('DEEPSEEK_','SIGFLOW_')):env.pop(key)
    env['SIGFLOW_DEEPSEEK_CONFIG']=str(directory/'absent-model.json')
    env['PYTHONPATH']=os.pathsep.join((str(source.resolve()),str(Path(__file__).resolve().parents[2])))
    env['TEMP']=env['TMP']=str(directory)
    return env


def _project(directory,simulation):
    project=directory/'project'
    (project/'rtl').mkdir(parents=True)
    (project/'rtl/top.v').write_text(
        'module top(input wire clk, input wire rst, input wire en, input wire d, output reg q);\n'
        'always @(posedge clk) if(rst) q <= 0; else if(en) q <= d;\nendmodule\n',encoding='utf-8')
    sources=['rtl/top.v']
    if simulation:
        (project/'rtl/tb.v').write_text('`ifndef SYNTHESIS\nmodule tb;\n'
            'reg clk; reg rst; reg en; reg d; wire q;\n'
            'top dut(.clk(clk), .rst(rst), .en(en), .d(d), .q(q));\n'
            'initial begin clk = 0; forever #5 clk = ~clk; end\n'
            'initial begin\nrst = 1;\nen = 1;\nd = 1;\n#12;\nrst = 0;\n#18;\nd = 0;\n#10;\nd = 0;\nend\n'
            'endmodule\n`endif\n',encoding='utf-8')
        sources.append('rtl/tb.v')
    (project/'sigflow.project').write_text(json.dumps(dict(build=dict(top_module=['top']),
        paths=dict(source_files=sources),fpga=dict(target_profile='test-target',yosys_strategy='baseline'))),encoding='utf-8')
    return project


def exercise(gateway_path,directory,timeout,*,scenario='simulation',u1_source=None):
    deadline=time.monotonic()+timeout
    simulation=scenario=='simulation'
    instance,nonce='inst-'+secrets.token_hex(12),secrets.token_hex(32)
    agent_token,gateway_ui,teaching_ui=(secrets.token_urlsafe(32) for _ in range(3))
    env=u1_environment(directory,u1_source)
    project=_project(directory,simulation)
    core=host=None
    gateway=ui=teaching=None
    scope={}
    try:
        core_env=os.environ.copy()
        boot=dict(project_path=str(project),instance_id=instance,agent_token=agent_token,
                  ui_token=gateway_ui,nonce=nonce)
        if simulation:
            from .simulation_acceptance import simulation_environment
            core_env=simulation_environment(directory)
            boot['simulation']=True
        core=Child([str(gateway_path)],core_env,cwd=directory);core.send(boot)
        ready=core.ready(min(deadline,time.monotonic()+20))
        check_ready(ready,instance,nonce,gateway=True)
        gateway=HTTP(ready['port'],agent_token,deadline)
        ui=HTTP(ready['port'],gateway_ui,deadline)
        health=gateway.request('GET','/api/v1/health',auth=False)
        require(health.get('instance_id')==instance and health.get('protocol_version')=='edu.api.v1',
                'gateway_identity_mismatch')
        route='/api/v1/projects/'+ready['project_id']
        context=gateway.request('GET',route+'/context')
        snapshot=gateway.request('POST',route+'/snapshots',dict(expected_revision=context['revision'],
            require_saved=True),status=201)['snapshot_id']
        scope=dict(project_id=ready['project_id'],revision=context['revision'],snapshot_id=snapshot)
        synth=dict(**scope,goal='Observe bounded synthesis',max_jobs=1,
            steps=[dict(step_id='synth',capability='eda.synth',provider='yosys',params=dict(top='top'))],
            expected_observations=['Read the scoped report without a functional verdict'])

        def count():return len(gateway.request('GET',route+'/jobs')['jobs'])
        def grant(plan_hash,plan):
            package=build_plan_card(parse_plan(plan),'plan-1',0)
            # This UI channel can be idle across a full teaching/restart round.
            # Open a fresh connection before the one grant mutation; never
            # retry a POST whose result may already have committed.
            ui.close()
            return ui.request('POST',route+'/grants',dict(plan_hash=plan_hash,revision=scope['revision'],
                snapshot_id=scope['snapshot_id'],max_jobs=plan['max_jobs'],steps=package.card['steps']),status=201)['grant_id']
        base_hash='baseline-'+secrets.token_hex(12)
        base_grant=grant(base_hash,synth)
        baseline=gateway.request('POST',route+'/jobs',dict(snapshot_id=snapshot,
            expected_revision=scope['revision'],capability='eda.synth',plugin_id='eda-synth-yosys',
            grant_id=base_grant,plan_hash=base_hash,step_id='s1',params=dict(top_module='top')),
            status=202,key='baseline')['job_id']

        def completed_job(job):
            state=wait_for(lambda:gateway.request('GET','/api/v1/jobs/'+job),
                lambda j:j.get('state') in {'Succeeded','Failed','Cancelled','TimedOut','Rejected'},
                deadline,'job_timeout')
            require(state['state']=='Succeeded','real_tool_job_failed')
            report=wait_for(lambda:gateway.request('GET','/api/v1/jobs/'+job+'/report'),
                lambda r:r.get('completeness')=='complete' and bool(r.get('artifacts')),
                deadline,'report_timeout')
            require(all(report.get(k)==v for k,v in dict(**scope,job_id=job,origin='core',
                state='Succeeded',exit_code=0).items()) and bool(report.get('input_fingerprint')),'report_invalid')
            return report
        completed_job(baseline)

        host_boot=dict(type='sigflow-bootstrap',protocol='edu.api.v1',instance_id=instance,nonce=nonce,
            gateway_url=f'http://127.0.0.1:{ready["port"]}',gateway_token=agent_token,
            ui_token=teaching_ui,data_directory=str(directory/'data'))
        def launch():
            child=Child([sys.executable,'-P','-m','sigflow_edu_agent','--bootstrap-stdin','--u2-teaching'],env,cwd=directory)
            try:
                child.send(host_boot)
                announcement=child.ready(min(deadline,time.monotonic()+20))
                check_ready(announcement,instance,nonce)
                connection=HTTP(announcement['port'],teaching_ui,deadline)
                caps=connection.request('GET','/api/v1/capabilities')
                require(caps.get('teaching',{}).get('schema_version')=='u1.u2-teaching.v1'
                    and caps['teaching']['http_l4_available'] is False,'u2_extension_unavailable')
                return child,connection
            except Exception:
                child.close()
                raise
        host,teaching=launch()
        session_body={k:scope[k] for k in ('project_id','revision')}
        session=teaching.request('POST',PREFIX+'/sessions',session_body,status=201,key='session',learner=True)['session_id']
        require(teaching.request('POST',PREFIX+'/sessions',session_body,status=201,key='session',learner=True)
            ['session_id']==session,'session_replay_changed_id')
        session_route=PREFIX+'/sessions/'+session
        teaching.request('POST',session_route+'/runs',{},status=401,auth=False,learner=True)

        def start(issue,intent='diagnose',**extra):
            body=dict(question='Explain what this scoped tool report establishes and its limitations.',
                job_id=baseline,snapshot_id=snapshot,issue_id=issue,intent=intent,mode='rule',**extra)
            response=teaching.request('POST',session_route+'/runs',body,status=202,key=issue,learner=True)
            return response['run_id'],body
        def get(rid):
            try:return teaching.request('GET',PREFIX+'/runs/'+rid,learner=True)
            except RunFailure as failure:
                # A checkpoint projection can detect a concurrent commit. A
                # fresh GET is safe; never retry an uncertain mutation.
                if str(failure)=='http_status_409':return dict(teaching_status='ProjectionRetry',state_version='0')
                raise
        def wait(rid,statuses,after=None):
            return wait_for(lambda:get(rid),lambda v:v['teaching_status'] in statuses
                and (after is None or int(v['state_version'])>int(after)),deadline,'teaching_state_timeout')
        def action(rid,state,kind,aid,**extra):
            body=dict(kind=kind,action_id=aid,expected_state_version=state['state_version'],**extra)
            teaching.request('POST',PREFIX+'/runs/'+rid+'/actions',body,status=202,learner=True)
            return body

        rid,body=start('teaching')
        waiting=wait(rid,{'WaitingStudent','Failed','WaitingEvidence'})
        require(waiting['teaching_status']=='WaitingStudent' and waiting['run'] is None
            and waiting['cards'][-1]['level']=='L1','initial_card_unavailable')
        initial_ids={e['ref']['evidence_id'] for e in waiting['cards'][0]['evidence']}
        require(all(e['ref']['job_id']==baseline for e in waiting['cards'][0]['evidence']),'initial_evidence_invalid')
        require(teaching.request('POST',session_route+'/runs',body,status=202,key='teaching',learner=True)
            ['run_id']==rid,'run_replay_changed_id')
        hint=action(rid,waiting,'hint_next','hint')
        second=wait(rid,{'WaitingStudent'},waiting['state_version'])
        require(second['cards'][-1]['level']=='L2' and count()==1,'hint_started_job_or_wrong_level')
        teaching.request('POST',PREFIX+'/runs/'+rid+'/actions',hint,status=202,learner=True)
        require(get(rid)['state_version']==second['state_version'],'hint_replay_advanced')

        teaching.request('POST','/api/v1/shutdown',status=202)
        teaching.close();teaching=None
        close_children(((host,8),));host=None
        host,teaching=launch()
        recovered=get(rid)
        require(recovered['teaching_status']=='RecoveryRequired' and not recovered['cards']
            and recovered['run'] is None and count()==1,'restart_replayed_or_published_old_cards')
        action(rid,recovered,'recover','recover',question=body['question'])
        restored=wait(rid,{'WaitingStudent'})
        require(restored['cards'][-1]['level']=='L2','recovery_changed_level')
        action(rid,restored,'hint_next','hint-l3')
        third=wait(rid,{'WaitingStudent'},restored['state_version'])
        require(third['cards'][-1]['level']=='L3','l3_unavailable')
        teaching.request('POST',PREFIX+'/runs/'+rid+'/actions',dict(kind='reference_confirm',
            action_id='forbidden-l4',expected_state_version=third['state_version']),status=403,learner=True)
        action(rid,third,'finish','finish')
        require(wait(rid,{'Completed'})['run']['state']=='degraded','teaching_not_completed')

        plan=synth
        if simulation:
            plan=dict(**scope,goal='Observe bounded controlled simulation',max_jobs=2,
                steps=[dict(step_id='build',capability='eda.sim.build',provider='verilator',
                    params=dict(top='top',testbench='tb')),
                    dict(step_id='run',capability='eda.sim.run',provider='verilator',
                        dependencies=['build'],params=dict(build_step_id='build',duration_ticks='40'))],
                expected_observations=['Observe the specified timeline without a whole-design verdict'])
        plan_hash='approved-'+secrets.token_hex(12)
        verify_id,_=start('verification','verify',plan=plan,gateway_plan_hash=plan_hash)
        current=wait(verify_id,{'WaitingApproval','Failed','WaitingEvidence'})
        require(current['teaching_status']=='WaitingApproval' and current['plan_card']['requires_approval']
            and count()==1,'job_started_without_approval')
        approved=grant(plan_hash,plan)
        action(verify_id,current,'execute_plan','execute',grant_ref=approved)
        current=wait(verify_id,{'Completed','RecoveryRequired','WaitingApproval','Failed'},current['state_version'])
        observations=successors=0
        while current['teaching_status'] in ('RecoveryRequired','WaitingApproval'):
            progress=current.get('verification',{})
            if current['teaching_status']=='RecoveryRequired':
                require(progress.get('can_observe') and progress.get('job_id'),'verification_unobservable')
                completed_job(progress['job_id'])
                action(verify_id,current,'observe_verification','observe-'+current['state_version'])
                observations+=1
            else:
                require(simulation and progress.get('completed_steps')==1 and count()==2,
                        'unexpected_successor')
                action(verify_id,current,'execute_plan','successor-'+current['state_version'],grant_ref=approved)
                successors+=1
            current=wait(verify_id,{'Completed','RecoveryRequired','WaitingApproval','Failed'},current['state_version'])
        require(current['teaching_status']=='Completed' and current['run']['model_used'] is False
            and current['run']['state']=='degraded','verification_feedback_incomplete')
        expected_jobs=3 if simulation else 2
        require(count()==expected_jobs and gateway.request('GET','/api/v1/grants/'+approved)['used_jobs']
            ==(2 if simulation else 1),'unexpected_job_or_grant_count')
        result_jobs={e['ref']['job_id'] for e in current['cards'][-1]['evidence']}
        require(result_jobs and baseline not in result_jobs and initial_ids.isdisjoint(
            {e['ref']['evidence_id'] for e in current['cards'][-1]['evidence']}),'old_evidence_reused')
        reports=[completed_job(job) for job in result_jobs]
        wave_result=None
        if simulation:
            from ..clients.gateway import GatewayConfig
            from ..clients.waveform import GatewayWaveformReader
            from ..runtime.gateway_session import SessionBinding
            report=next(r for r in reports if r['capability']=='eda.sim.run')
            wave=next(a for a in report['artifacts'] if a['schema']=='eda.wave.vcd.v1')
            reader=GatewayWaveformReader(GatewayConfig(ready['port'],agent_token,instance))
            binding=SessionBinding(instance,'session-acceptance','integration',scope['project_id'],
                scope['revision'],snapshot,report['job_id'],'job-report','prediction')
            description=reader.describe(binding,wave['artifact_id'])
            signal=next(s for s in description.signals if s.full_name.endswith('.q'))
            prediction_body=dict(question='Compare the declared expectation 1 at tick 15.',
                job_id=report['job_id'],snapshot_id=snapshot,issue_id='prediction',intent='diagnose',mode='rule')
            prediction_id=teaching.request('POST',session_route+'/runs',prediction_body,
                status=202,key='prediction',learner=True)['run_id']
            prediction=wait(prediction_id,{'WaitingStudent'})
            action(prediction_id,prediction,'prediction_request','challenge',prediction=dict(
                artifact_id=wave['artifact_id'],signal_id=signal.signal_id,start_tick='0',end_tick='40',sample_tick='15'))
            challenge=wait(prediction_id,{'WaitingStudent'},prediction['state_version'])
            require(challenge['prediction']['status']=='awaiting_prediction' and
                'observed' not in challenge['prediction'],'prediction_leaked_observation')
            action(prediction_id,challenge,'prediction_submit','compare',prediction=dict(
                challenge_id=challenge['prediction']['challenge_id'],expected='1'))
            observed=wait(prediction_id,{'WaitingStudent'},challenge['state_version'])
            comparison=observed['prediction']
            require(comparison.get('outcome')=='matches' and comparison.get('observed')=='1'
                and count()==3,'prediction_comparison_failed')
            action(prediction_id,observed,'finish','finish-prediction')
            wait(prediction_id,{'Completed'})
            wave_result=dict(status='passed',exact_window=['0','40'],sample_tick='15',expected='1',observed='1',
                outcome='matches',challenge_before_observation=True,additional_jobs=0,model_calls=0,
                artifact_hash=wave['sha256'],teacher_review='not_assessed')

        history=teaching.request('GET',PREFIX+'/runs/'+rid+'/events?after=0&limit=100',learner=True)
        require(history.get('run_id')==rid and history.get('session_id')==session and history.get('events')
            and all(set(row)=={'sequence','action_id','state_version','teaching_status'}
                for row in history['events']),'public_event_metadata_invalid')

        cancel_id,_=start('cancel')
        cancel_wait=wait(cancel_id,{'WaitingStudent'})
        # Only the runner edits its own temporary source, then asks Core to
        # refresh. U1/U2 have no write or project-refresh route.
        with (project/'rtl/top.v').open('a',encoding='utf-8') as stream:stream.write('// owned acceptance revision\n')
        core.send(dict(type='refresh-project'))
        fresh=wait_for(lambda:gateway.request('GET',route+'/context'),
            lambda c:c['revision']!=scope['revision'],deadline,'revision_not_advanced')
        stale=teaching.request('GET',PREFIX+'/runs/'+cancel_id,status=409,learner=True)
        require(stale.get('code')=='STALE_REVISION','old_scope_not_rejected')
        teaching.request('POST',PREFIX+'/runs/'+cancel_id+'/actions',dict(kind='hint_next',
            action_id='stale',expected_state_version=cancel_wait['state_version']),status=409,learner=True)
        ack=teaching.request('POST',PREFIX+'/runs/'+cancel_id+'/cancel',{},status=202,learner=True)
        require(ack.get('accepted') is True and count()==expected_jobs,'cancel_or_stale_started_job')
        events=teaching.request('GET',PREFIX+'/runs/'+rid+'/events?after=0&limit=100',status=409,learner=True)
        require(events.get('code')=='STALE_REVISION','events_not_scope_guarded')
        return dict(status='passed',schema_version='edu.u1-u2-acceptance.v1',real_core=True,real_yosys=True,
            real_verilator=simulation,model_calls=0,shared_port=True,host='formal_u1',
            extension='u1.u2-teaching.v1',scenario=scenario,job_count=expected_jobs,
            l1_l2_l3='verified',hint_replay='verified',session_run_replay='verified',
            event_metadata='verified',
            restart=dict(external_ids='preserved',old_cards='quarantined',explicit_recovery='verified',additional_jobs=0),
            verification=dict(status='Completed',result_jobs=len(result_jobs),observations=observations,
                explicit_successor_confirmations=successors,model_used=False),
            prediction=wave_result,stale_revision='denied',cancel_after_revision_change='accepted',
            new_revision=fresh['revision'],http_l4='denied',functional_correctness='not_evaluated',
            limitations=['GUI not exercised','actual UA-10 and reviewed materials not configured',
                         'teacher and target-platform acceptance pending'])
    finally:
        if teaching:
            try:teaching.deadline=time.monotonic()+2;teaching.request('POST','/api/v1/shutdown',status=202)
            except Exception:pass
        for connection in (teaching,gateway,ui):
            if connection is not None:connection.close()
        close_children(((host,8),(core,10)))


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gateway',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--work-dir',type=Path)
    parser.add_argument('--u1-source',type=Path)
    parser.add_argument('--scenario',choices=('synthesis','simulation'),default='simulation')
    parser.add_argument('--timeout',type=float,default=300)
    args=parser.parse_args(argv)
    report=dict(status='failed',model_calls=0,functional_correctness='not_evaluated')
    try:
        require(args.gateway.is_file(),'gateway_missing')
        require(0<args.timeout<=600,'invalid_timeout')
        with tempfile.TemporaryDirectory(prefix='sigflow-u1-u2-',dir=args.work_dir) as owned:
            report=exercise(args.gateway.resolve(),Path(owned),args.timeout,
                scenario=args.scenario,u1_source=args.u1_source)
        report['cleanup']='completed'
    except RunFailure as failure:
        report=dict(status='failed',model_calls=0,functional_correctness='not_evaluated',error=str(failure))
    except Exception:
        report=dict(status='failed',model_calls=0,functional_correctness='not_evaluated',error='integration_failed')
    raw=json.dumps(report,sort_keys=True,ensure_ascii=True,indent=2)+'\n'
    try:args.output.write_text(raw,encoding='utf-8')
    except Exception:
        report=dict(status='failed',model_calls=0,functional_correctness='not_evaluated',error='report_write_failed')
        raw=json.dumps(report,sort_keys=True,ensure_ascii=True,indent=2)+'\n'
    print(raw,end='')
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':raise SystemExit(main())
