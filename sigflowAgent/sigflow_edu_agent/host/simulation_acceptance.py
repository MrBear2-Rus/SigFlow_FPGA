"""Explicit, owned-project Verilator pipeline acceptance; no waveform inference."""
import os
from pathlib import Path
import re
import secrets
import shutil
import sys

from .acceptance_support import RunFailure, require, wait_for
from .runtime import parse_plan
from ..contracts.plan import build_plan_card


def simulation_environment(directory, *, repo_root=None, inherited=None):
    """Prepare only this owned Core child's tools; never alter installed tools."""
    env = dict(os.environ if inherited is None else inherited)
    env['SIGFLOW_PYTHON3'] = str(Path(getattr(sys, '_base_executable', sys.executable)))
    if os.name != 'nt':
        return env
    repo = Path(repo_root) if repo_root is not None else Path(__file__).resolve().parents[3]
    compiler = repo/'3rd/mingw-msvcrt/mingw64/bin'
    verilator = repo/'tools/verilator/verilator-install'
    git = shutil.which('git', path=env.get('PATH', ''))
    shell_bin = Path(git).resolve().parents[1]/'usr/bin' if git else Path(directory)/'missing-shell'
    required = (compiler/'mingw32-make.exe', compiler/'g++.exe',
                verilator/'bin/verilator_bin_dbg.exe', shell_bin/'sh.exe', shell_bin/'perl.exe')
    require(all(p.is_file() and p.stat().st_size > 0 for p in required), 'simulation_tools_missing')
    private_bin = Path(directory)/'simulation-tools'
    private_bin.mkdir()
    shutil.copyfile(compiler/'mingw32-make.exe', private_bin/'make.exe')
    env.update(SIGFLOW_SH=str(shell_bin/'sh.exe'),
               SIGFLOW_VERILATOR=str(verilator/'bin/verilator_bin_dbg.exe'),
               VERILATOR_ROOT=verilator.as_posix())
    # Bundled verilated.mk contains the packager's absolute MSVC/Python paths.
    # GNU make command-variable overrides win over those file assignments.
    # Keep this opt-in acceptance child's configuration private and fixed.
    env['MAKEFLAGS'] = '-- CXX=g++ LINK=g++ AR=ar PYTHON3=python3'
    env.pop('MFLAGS', None)
    env.pop('GNUMAKEFLAGS', None)
    env['PATH'] = os.pathsep.join((str(private_bin), str(compiler), str(shell_bin),
                                  str(Path(env['SIGFLOW_PYTHON3']).parent), env.get('PATH', '')))
    return env


def verify_report(report, scope, job_id, capability, plugin_id):
    expected = dict(**scope, job_id=job_id, state='Succeeded', origin='core',
                    schema_version='edu.jobreport.v1', completeness='complete',
                    capability=capability, plugin_id=plugin_id)
    require(type(report) is dict and all(report.get(k) == v for k, v in expected.items())
            and type(report.get('exit_code')) is int and report['exit_code'] == 0
            and type(report.get('input_fingerprint')) is str and bool(report['input_fingerprint'].strip()),
            'simulation_report_invalid')
    rows = report.get('artifacts')
    require(type(rows) is list and 1 <= len(rows) <= 16
            and all(type(a) is dict and type(a.get('artifact_id')) is str
                    and re.fullmatch(r'[A-Za-z0-9_-]{1,256}', a['artifact_id'])
                    and type(a.get('sha256')) is str
                    and re.fullmatch(r'[a-fA-F0-9]{64}', a['sha256']) for a in rows)
            and len({a['artifact_id'] for a in rows}) == len(rows), 'simulation_report_invalid')


def verify_wave_artifact(report, metadata, scope, job_id):
    """Certify file metadata, never exact waveform values or design correctness."""
    verify_report(report, scope, job_id, 'eda.sim.run', 'eda-sim-verilator-run')
    waves = [a for a in report['artifacts'] if a.get('schema') == 'eda.wave.vcd.v1']
    require(len(waves) == 1 and type(metadata) is dict, 'simulation_waveform_invalid')
    artifact = waves[0]
    expected = dict(project_id=scope['project_id'], revision=scope['revision'], job_id=job_id,
                    artifact_id=artifact['artifact_id'], sha256=artifact['sha256'], schema='eda.wave.vcd.v1')
    size = metadata.get('size')
    require(all(metadata.get(k) == v for k, v in expected.items())
            and type(size) is str and re.fullmatch(r'[1-9][0-9]{0,19}', size)
            and int(size) <= 2**64-1, 'simulation_waveform_invalid')
    return dict(artifact_id=artifact['artifact_id'], sha256=artifact['sha256'],
                schema='eda.wave.vcd.v1', size_bytes=int(size), metadata_verified=True,
                exact_samples_verified=False)


def exercise_simulation(gateway, ui, teaching, route, scope, baseline, deadline, *, waves=False,instance_id=None,predictions=False,dynamic=False):
    """Run the existing two-step teaching plan via real Gateway and sidecar HTTP."""
    require(not predictions or (waves and instance_id is not None),'prediction_requires_wave_mode')
    require(not dynamic or (waves and predictions),'dynamic_requires_prediction')
    caps = gateway.request('GET', '/api/v1/capabilities')['capabilities']
    for capability, plugin in (('eda.sim.build','eda-sim-verilator'),
                               ('eda.sim.run','eda-sim-verilator-run')):
        require(any(c.get('id') == capability and c.get('plugin_id') == plugin
                    and c.get('ready') is True and c.get('disabled') is False for c in caps),
                'simulation_capability_unavailable')
    plan = dict(**scope, goal='Observe the bounded Verilator execution pipeline', max_jobs=2,
        steps=[dict(step_id='build', capability='eda.sim.build', provider='verilator',
                    params=dict(top='top', testbench='tb' if dynamic else 'top')),
               dict(step_id='run', capability='eda.sim.run', provider='verilator',
                    params=(dict(build_step_id='build',duration_ticks='40') if dynamic else
                            dict(build_step_id='build')), dependencies=['build'])],
        expected_observations=['Read the scoped reports and VCD metadata; no dynamic functional verdict'])
    package = build_plan_card(parse_plan(plan), 'plan-1', 0)
    plan_hash = 'simulation-'+secrets.token_hex(16)
    create = dict(**scope, learner_id='integration', job_id=baseline, diagnostic_id='job-report',
                  issue_id='simulation-observation', mode='rule', intent='verify', plan=plan,
                  gateway_plan_hash=plan_hash,
                  question=('Compare an explicit expectation under the controlled reset/clock/input timeline.' if dynamic else
                            'Explain the limits of this Verilator build/run and its VCD artifact.'))
    sid = teaching.request('POST', '/api/v1/teaching/sessions', create, status=202,
                           key='simulation-create', learner=True)['session_id']
    session = '/api/v1/teaching/sessions/'+sid
    state = lambda: teaching.request('GET', session+'/state', learner=True)
    paused = {'WaitingApproval','RecoveryRequired','Completed','WaitingEvidence','Failed','BudgetExceeded'}
    waiting = wait_for(state, lambda s:s['status'] in paused, deadline, 'simulation_approval_timeout')
    require(waiting['status'] == 'WaitingApproval' and waiting.get('cards')
            and waiting['plan_card']['steps'] == package.card['steps'], 'simulation_plan_unavailable')
    require(len(gateway.request('GET', route+'/jobs')['jobs']) == 1, 'unapproved_simulation_started')
    grant = ui.request('POST', route+'/grants', dict(plan_hash=plan_hash, revision=scope['revision'],
        snapshot_id=scope['snapshot_id'], max_jobs=2, steps=waiting['plan_card']['steps']), status=201)['grant_id']
    # A synth artifact cannot stand in for this approved run's build dependency.
    gateway.request('POST', route+'/jobs', dict(snapshot_id=scope['snapshot_id'],
        expected_revision=scope['revision'], capability='eda.sim.run', plugin_id='eda-sim-verilator-run',
        grant_id=grant, plan_hash=plan_hash, step_id='s2', params=dict(build_job_id=baseline)),
        key='invalid-build-dependency', status=422)
    require(gateway.request('GET','/api/v1/grants/'+grant)['used_jobs'] == 0
            and len(gateway.request('GET',route+'/jobs')['jobs']) == 1, 'invalid_dependency_consumed_grant')

    def dispatch(current, kind):
        body = dict(kind=kind, action_id=kind+'-'+current['state_version'],
                    expected_version=current['state_version'])
        if kind == 'execute_plan':
            body['grant_ref'] = grant
        teaching.request('POST', session+'/actions', body, status=202, learner=True)
        updated = wait_for(state, lambda s:int(s['state_version']) > int(current['state_version'])
                           and s['status'] in paused, deadline, 'simulation_action_timeout')
        return updated, body

    current, first_action = dispatch(waiting, 'execute_plan')
    observed = successor_confirmations = 0
    while current['status'] in {'RecoveryRequired', 'WaitingApproval'}:
        if current['status'] == 'RecoveryRequired':
            progress = current.get('verification', {})
            completed_steps = progress.get('completed_steps')
            require(type(completed_steps) is int and completed_steps in (0,1),
                    'simulation_progress_invalid')
            require(progress.get('can_observe') is True and progress.get('job_id'),
                    'simulation_not_observable')
            # Wait via GET without spending teaching stage budget on busy polling.
            job = progress['job_id']
            terminal = wait_for(lambda:gateway.request('GET','/api/v1/jobs/'+job),
                lambda j:j['state'] in {'Succeeded','Failed','Cancelled','TimedOut','Rejected'},
                deadline, 'simulation_job_timeout')
            require(terminal['state'] == 'Succeeded', 'simulation_job_failed')
            current, _ = dispatch(current, 'observe_verification')
            observed += 1
            if completed_steps == 0:
                require(current['status'] == 'WaitingApproval'
                        and current.get('reason') == 'successor_approval_required'
                        and current['verification']['completed_steps'] == 1
                        and len(gateway.request('GET',route+'/jobs')['jobs']) == 2,
                        'simulation_successor_started_automatically')
        else:
            require(current.get('reason') == 'successor_approval_required'
                    and current['verification']['completed_steps'] == 1
                    and len(gateway.request('GET',route+'/jobs')['jobs']) == 2,
                    'simulation_successor_started_automatically')
            current, _ = dispatch(current, 'execute_plan')
            successor_confirmations += 1
    require(current['status'] == 'Completed' and current['reason'] == 'outcome_indeterminate'
            and current['verification']['completed_steps'] == 2 and current['model_calls'] == 0,
            'simulation_feedback_incomplete')
    jobs = gateway.request('GET',route+'/jobs')['jobs']
    require(len(jobs) == 3, 'simulation_job_count_invalid')
    reports = {}
    for record in jobs:
        if record['job_id'] == baseline:
            continue
        report = gateway.request('GET','/api/v1/jobs/'+record['job_id']+'/report')
        capability = report.get('capability')
        require(capability in {'eda.sim.build','eda.sim.run'} and capability not in reports,
                'simulation_report_invalid')
        verify_report(report, scope, record['job_id'], capability,
                      'eda-sim-verilator' if capability == 'eda.sim.build' else 'eda-sim-verilator-run')
        reports[capability] = report
    require(len(reports) == 2, 'simulation_report_missing')
    run = reports['eda.sim.run']
    vcd_artifacts = [a for a in run['artifacts'] if a.get('schema') == 'eda.wave.vcd.v1']
    require(len(vcd_artifacts) == 1, 'simulation_waveform_invalid')
    metadata = gateway.request('GET','/api/v1/artifacts/'+vcd_artifacts[0]['artifact_id'])
    wave = verify_wave_artifact(run, metadata, scope, run['job_id'])
    initial, feedback = waiting['cards'][0], current['cards'][-1]
    require(feedback['producer'] == 'rule' and len(feedback['evidence_ids']) == 2
            and len(set(feedback['evidence_ids'])) == 2
            and set(feedback['evidence_ids']).isdisjoint(initial['evidence_ids']),
            'simulation_feedback_evidence_invalid')
    require(gateway.request('GET','/api/v1/grants/'+grant)['used_jobs'] == 2,
            'simulation_grant_consumption_invalid')
    teaching.request('POST',session+'/actions',first_action,status=202,learner=True)
    require(state()['state_version'] == current['state_version']
            and len(gateway.request('GET',route+'/jobs')['jobs']) == 3, 'simulation_replay_mutated_state')
    teaching.request('POST',session+'/actions',dict(kind='finish',action_id='simulation-stale',
        expected_version=waiting['state_version']),status=409,learner=True)
    result=dict(status='passed', real_core=True, real_yosys=True, real_verilator=True,
        model_mode='rule', model_calls=0, job_count=3, baseline_job_id=baseline,
        build_job_id=reports['eda.sim.build']['job_id'], run_job_id=run['job_id'],
        completed_status=current['status'], reason=current['reason'], evidence_origin=current['evidence_origin'],
        baseline_evidence_ids=initial['evidence_ids'], result_evidence_ids=feedback['evidence_ids'],
        grant_used_jobs=[1,2], invalid_build_dependency='denied', action_replay='verified',
        stale_version='denied', observation_actions=observed,
        successor_confirmations=successor_confirmations, waveform=wave,
        harness='controlled_timeline' if dynamic else 'unverified_timeline', time_advance_verified=False,
        functional_correctness='not_evaluated',
        limits=['dynamic_stimulus','trusted_exact_waveform','student_prediction','GUI','teaching_correctness'])
    if waves:
        from ..clients.waveform import GatewayWaveformReader
        from ..clients.gateway import GatewayConfig
        from ..runtime.gateway_session import SessionBinding
        binding=SessionBinding(instance_id,'session-wave-acceptance','integration',scope['project_id'],
                               scope['revision'],scope['snapshot_id'],run['job_id'],'job-report','wave-observation')
        reader=GatewayWaveformReader(GatewayConfig(gateway.port,gateway.token,instance_id))
        description=reader.describe(binding,wave['artifact_id'])
        signal=next((s for s in description.signals if s.full_name.endswith('.q')),description.signals[0])
        window=reader.read(binding,wave['artifact_id'],signal_ids=(signal.signal_id,),
                           start_tick=str(description.start_tick),end_tick=str(description.end_tick))
        result['waveform']['exact_samples_verified']=True
        result['time_advance_verified']=description.end_tick>description.start_tick
        if dynamic:
            require(description.start_tick==0 and description.end_tick>=40
                    and sum(len(s.transitions) for s in window.signals)>=2,'dynamic_timeline_missing')
            result['limits'].remove('dynamic_stimulus')
            result['stimulus']=dict(clock_half_period_ticks='5',reset_release_tick='12',
                data_change_tick='30',duration_ticks='40',expected_sample_tick='15',expected='1',
                contract_origin='owned_test_input', teacher_review='not_assessed')
        result['wave_observation']=dict(exact_window_verified=True,origin=window.origin,
            evidence_id=window.anchor.evidence_id,job_id=window.anchor.job_id,
            artifact_id=window.anchor.artifact_id,artifact_hash=window.anchor.artifact_hash,
            revision=window.anchor.revision,snapshot_id=window.anchor.snapshot_id,
            start_tick=str(window.start_tick),end_tick=str(window.end_tick),timescale=window.timescale,
            signal_id=signal.signal_id,full_name=signal.full_name,width=signal.width,
            transitions=sum(len(s.transitions) for s in window.signals))
        result['limits'].remove('trusted_exact_waveform')
        if predictions:
            # Literal expectations follow the owned RTL and declared input; no
            # queried observation chooses the student's expected answer.
            require(signal.width==1,'prediction_signal_width_invalid')
            sample_tick='15' if dynamic else str(description.start_tick)
            expected='1' if dynamic else '0'
            create=dict(**scope,learner_id='integration',job_id=run['job_id'],diagnostic_id='job-report',
                issue_id='wave-prediction',question=f'Compare the explicit expectation {expected} at tick {sample_tick}.',
                mode='rule',intent='diagnose')
            psid=teaching.request('POST','/api/v1/teaching/sessions',create,status=202,key='prediction-create',learner=True)['session_id']
            path='/api/v1/teaching/sessions/'+psid
            get=lambda:teaching.request('GET',path+'/state',learner=True)
            pstate=wait_for(get,lambda s:s['status'] in {'WaitingStudent','Failed','WaitingEvidence'},deadline,'prediction_create_timeout')
            require(pstate['status']=='WaitingStudent','prediction_evidence_unavailable')
            def act(previous,kind,aid,payload):
                action=dict(kind=kind,action_id=aid,expected_version=previous['state_version'],prediction=payload)
                teaching.request('POST',path+'/actions',action,status=202,learner=True)
                updated=wait_for(get,lambda s:int(s['state_version'])>int(previous['state_version']),deadline,'prediction_timeout')
                return updated,action
            pending,_=act(pstate,'prediction_request','predict',dict(artifact_id=wave['artifact_id'],signal_id=signal.signal_id,
                start_tick=str(description.start_tick),end_tick=str(description.end_tick),sample_tick=sample_tick))
            require(pending['prediction']['status']=='awaiting_prediction' and 'observed' not in pending['prediction'],
                    'prediction_leaked_observation')
            compared,action=act(pending,'prediction_submit','compare',dict(challenge_id=pending['prediction']['challenge_id'],expected=expected))
            require(compared['prediction']['status']=='observed' and compared['prediction']['outcome']=='matches'
                    and compared['prediction']['observed']==expected and compared['level']==pstate['level']
                    and compared['model_calls']==0 and compared['status']=='WaitingStudent','prediction_comparison_failed')
            teaching.request('POST',path+'/actions',action,status=202,learner=True)
            require(get()['state_version']==compared['state_version'] and len(gateway.request('GET',route+'/jobs')['jobs'])==3,
                    'prediction_replay_mutated_state')
            result['prediction']=dict(status='passed',outcome='matches',expected=expected,observed=compared['prediction']['observed'],
                sample_tick=sample_tick,level_unchanged=True,model_calls=0,additional_jobs=0,
                challenge_before_observation=True,replay='verified',evidence_id=compared['prediction']['evidence_id'],
                limitation='single fixed sample; no whole-design verdict')
            result['limits'].remove('student_prediction')
    return result
