"""Owned-project student edit acceptance; immutable teaching scopes are retained.

Only the test parent writes its temporary RTL and requests Core refresh through
its own stdin pipe. The teaching service has no write or refresh capability.
"""
import secrets

from .acceptance_support import require, wait_for
from .runtime import parse_plan
from ..contracts.plan import build_plan_card


def exercise_student_edit(core, project, route, gateway, ui, teaching,
                          scope, plan, create, deadline):
    """Return curated facts after a new revision receives independent approval."""
    def count():
        return len(gateway.request('GET', route + '/jobs')['jobs'])

    def issue_grant(binding, plan_hash, steps):
        return ui.request('POST', route + '/grants', dict(plan_hash=plan_hash,
            revision=binding['revision'], snapshot_id=binding['snapshot_id'],
            max_jobs=1, steps=steps), status=201)['grant_id']

    def wait_state(session, statuses, *, after=None):
        return wait_for(lambda: teaching.request('GET', session + '/state', learner=True),
            lambda s: s['status'] in statuses and (after is None or int(s['state_version']) > int(after)),
            deadline, 'revision_session_timeout')

    def start(body, key):
        sid = teaching.request('POST', '/api/v1/teaching/sessions', body,
                               status=202, key=key, learner=True)['session_id']
        session = '/api/v1/teaching/sessions/' + sid
        waiting = wait_state(session, {'WaitingApproval', 'WaitingEvidence', 'Failed'})
        require(waiting['status'] == 'WaitingApproval' and waiting['cards'], 'revision_evidence_unavailable')
        require(waiting['cards'][0]['producer'] == 'rule', 'revision_round_not_rule')
        return session, waiting

    old_create = create | {'mode': 'rule', 'gateway_plan_hash': 'pre-edit-' + secrets.token_hex(16)}
    old_session, old_waiting = start(old_create, 'pre-edit-create')
    old_grant = issue_grant(scope, old_create['gateway_plan_hash'], old_waiting['plan_card']['steps'])
    old_report = gateway.request('GET', '/api/v1/jobs/' + create['job_id'] + '/report')
    require(count() == 2, 'revision_unapproved_job_started')
    # This file belongs to the runner's TemporaryDirectory, not a user project.
    (project / 'rtl' / 'top.v').write_text(
        'module top(input wire clk, input wire en, input wire d, output reg q);\n'
        'always @(posedge clk) if(en) q <= ~d;\nendmodule\n', encoding='utf-8')
    core.send({'type': 'refresh-project'})
    current = wait_for(lambda: gateway.request('GET', route + '/context'),
        lambda c: c['revision'] != scope['revision'], deadline, 'revision_not_advanced')
    require(current.get('dirty') is False and current.get('synchronized') is True,
            'revision_not_saved')
    error = gateway.request('POST', route + '/snapshots',
        dict(expected_revision=scope['revision'], require_saved=True), status=409)
    require(error.get('code') == 'STALE_REVISION', 'old_snapshot_not_rejected')
    old_job = dict(snapshot_id=scope['snapshot_id'], expected_revision=scope['revision'],
        capability='eda.synth', plugin_id='eda-synth-yosys', grant_id=old_grant,
        plan_hash=old_create['gateway_plan_hash'], step_id='s1', params={'top_module': 'top'})
    error = gateway.request('POST', route + '/jobs', old_job, status=409, key='stale-revision-job')
    require(error.get('code') == 'STALE_REVISION', 'old_job_not_rejected')
    teaching.request('POST', old_session + '/actions', dict(kind='execute_plan',
        action_id='stale-execute', expected_version=old_waiting['state_version'], grant_ref=old_grant),
        status=202, learner=True)
    blocked = wait_state(old_session, {'Failed', 'RecoveryRequired', 'Completed'}, after=old_waiting['state_version'])
    require(blocked['status'] == 'Failed' and count() == 2 and
            gateway.request('GET', '/api/v1/grants/' + old_grant)['used_jobs'] == 0,
            'stale_approval_started_job')

    snapshot = gateway.request('POST', route + '/snapshots',
        dict(expected_revision=current['revision'], require_saved=True), status=201)
    fresh = dict(project_id=scope['project_id'], revision=current['revision'], snapshot_id=snapshot['snapshot_id'])
    require(fresh['snapshot_id'] != scope['snapshot_id'], 'snapshot_not_new')
    new_plan = plan | fresh
    package = build_plan_card(parse_plan(new_plan), 'plan-1', 0)
    base_hash = 'edited-baseline-' + secrets.token_hex(16)
    base_grant = issue_grant(fresh, base_hash, package.card['steps'])
    base_body = old_job | dict(expected_revision=fresh['revision'], snapshot_id=fresh['snapshot_id'],
                               grant_id=base_grant, plan_hash=base_hash)
    baseline = gateway.request('POST', route + '/jobs', base_body, status=202, key='edited-baseline')['job_id']

    def report(job):
        record = wait_for(lambda: gateway.request('GET', '/api/v1/jobs/' + job),
            lambda j: j['state'] in {'Succeeded', 'Failed', 'Cancelled', 'TimedOut', 'Rejected'},
            deadline, 'edited_job_timeout')
        require(record['state'] == 'Succeeded', 'edited_yosys_failed')
        result = gateway.request('GET', '/api/v1/jobs/' + job + '/report')
        expected = dict(**fresh, job_id=job, state='Succeeded', origin='core',
            schema_version='edu.jobreport.v1', completeness='complete', capability='eda.synth',
            plugin_id='eda-synth-yosys', exit_code=0)
        require(all(result.get(k) == v for k, v in expected.items()) and result.get('artifacts')
                and result.get('input_fingerprint'), 'edited_report_incomplete')
        return result

    baseline_report = report(baseline)
    require(baseline_report['input_fingerprint'] != old_report['input_fingerprint'], 'input_fingerprint_not_new')
    fresh_create = create | fresh | dict(mode='rule', job_id=baseline, plan=new_plan,
        gateway_plan_hash='edited-approved-' + secrets.token_hex(16))
    # A fresh card cannot reuse the still-unused old revision's grant either.
    wrong_session, wrong_waiting = start(fresh_create, 'edited-wrong-grant-create')
    teaching.request('POST', wrong_session + '/actions', dict(kind='execute_plan',
        action_id='wrong-grant-execute', expected_version=wrong_waiting['state_version'], grant_ref=old_grant),
        status=202, learner=True)
    wrong = wait_state(wrong_session, {'Failed', 'RecoveryRequired', 'Completed'}, after=wrong_waiting['state_version'])
    require(wrong['status'] == 'Failed' and count() == 3 and
            gateway.request('GET', '/api/v1/grants/' + old_grant)['used_jobs'] == 0,
            'old_grant_reused_on_new_revision')
    session, waiting = start(fresh_create, 'edited-create')
    require(count() == 3, 'edited_job_started_before_new_approval')
    approved = issue_grant(fresh, fresh_create['gateway_plan_hash'], waiting['plan_card']['steps'])
    action = dict(kind='execute_plan', action_id='edited-execute',
                  expected_version=waiting['state_version'], grant_ref=approved)
    teaching.request('POST', session + '/actions', action, status=202, learner=True)
    completed = wait_state(session, {'Completed', 'RecoveryRequired', 'WaitingEvidence', 'Failed'})
    while completed['status'] == 'RecoveryRequired' and completed.get('verification', {}).get('can_observe'):
        previous = completed['state_version']
        teaching.request('POST', session + '/actions', dict(kind='observe_verification',
            action_id='edited-observe-' + previous, expected_version=previous), status=202, learner=True)
        completed = wait_state(session, {'Completed', 'RecoveryRequired', 'WaitingEvidence', 'Failed'}, after=previous)
    require(completed['status'] == 'Completed' and completed['reason'] == 'outcome_indeterminate',
            'edited_feedback_incomplete')
    result_job = completed['verification']['job_id']
    require(result_job != baseline and completed['verification']['result_evidence_ready'] is True,
            'edited_result_not_new')
    report(result_job)
    before, after = waiting['cards'][0], completed['cards'][-1]
    require(before['evidence_ids'] and after['evidence_ids'] and after['producer'] == 'rule'
            and set(before['evidence_ids']).isdisjoint(after['evidence_ids'])
            and set(before['evidence_ids']).isdisjoint(old_waiting['cards'][0]['evidence_ids']),
            'edited_evidence_reused')
    teaching.request('POST', session + '/actions', action, status=202, learner=True)
    replay = teaching.request('GET', session + '/state', learner=True)
    require(replay['state_version'] == completed['state_version'] and count() == 4,
            'edited_replay_started_job')
    used = [gateway.request('GET', '/api/v1/grants/' + ref)['used_jobs']
            for ref in (old_grant, base_grant, approved)]
    require(used == [0, 1, 1], 'edited_grant_consumption_mismatch')
    return dict(old_revision=scope['revision'], new_revision=fresh['revision'],
        old_snapshot_id=scope['snapshot_id'], new_snapshot_id=fresh['snapshot_id'],
        old_input_fingerprint=old_report['input_fingerprint'], new_input_fingerprint=baseline_report['input_fingerprint'],
        baseline_job_id=baseline, result_job_id=result_job, baseline_evidence_ids=before['evidence_ids'],
        result_evidence_ids=after['evidence_ids'], evidence_origin=completed['evidence_origin'],
        completed_status=completed['status'], reason=completed['reason'],
        old_scope_job='denied', old_scope_teaching='denied', old_grant_on_new_revision='denied',
        new_approval='verified', action_replay='verified', grant_used_jobs=used,
        functional_correctness='not_evaluated')
