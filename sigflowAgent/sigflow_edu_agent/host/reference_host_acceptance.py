"""Real Core receipts plus a fixture teaching host; metadata only.

The caller owns the gateway, UI client and temporary directory. Materials and
reviewer identities are synthetic fixtures, with no model, Job or GUI signoff.
"""
import json
import time

from .acceptance_support import require, wait_for
from .runtime import Runtime
from .store import DurableStore
from ..clients.gateway import GatewayError
from ..demo import FixtureSource
from ..loop import TeachingLoop
from ..runtime.session import TeachingSession


def exercise_reference_host(directory, binding, item, port, ui, route):
    path = directory / 'reference-runtime.sqlite'
    learner = binding.learner_id
    body = dict(learner_id=learner, project_id=binding.project_id, revision=binding.revision,
                snapshot_id=binding.snapshot_id, job_id='fixture-job-1',
                diagnostic_id=binding.diagnostic_id, issue_id=binding.issue_id,
                question='Explain the synthetic latch diagnostic', mode='rule', intent='diagnose')

    class LossyPort:
        def __init__(self):
            self.lookups = []
            self.calls = []
            self.acknowledgements = []

        def lookup(self, receipt_id):
            self.lookups.append(receipt_id)
            return port.lookup(receipt_id)

        def consume(self, *args):
            self.calls.append(args)
            acknowledged = port.consume(*args)
            self.acknowledgements.append(acknowledged['replayed'])
            if len(self.calls) == 1:
                # Core has committed. Only the caller's successful reply is lost.
                raise GatewayError('TIMEOUT')
            return acknowledged

    fault = LossyPort()
    store = runtime = None
    receipt_id = None

    def factory(session_binding, request, mode, reserve):
        return TeachingSession(TeachingLoop(request, FixtureSource()))

    def open_host():
        nonlocal store, runtime
        store = DurableStore(path, binding.instance_id)
        runtime = Runtime(store, binding.instance_id, factory, evidence_origin='fixture',
                          reference_provider=lambda b: item, receipt_port=fault)

    def close_host():
        nonlocal store, runtime
        try:
            if runtime is not None:
                runtime.close()
        finally:
            runtime = None
            if store is not None:
                store.close()
                store = None

    def assert_private(sid):
        # Read every public persistence surface, including raw action fingerprints
        # and checkpoints. Only reference_records may retain the receipt identity.
        with store._transaction() as db:
            public = {table: [dict(row) for row in db.execute('SELECT * FROM ' + table)]
                      for table in ('sessions', 'actions', 'events')}
        serialized = json.dumps([public, runtime.snapshot(sid, learner),
                                 runtime.events(sid, learner, 0)], ensure_ascii=False)
        require(item.content not in serialized, 'host_reference_body_published')
        require(receipt_id is None or receipt_id not in serialized, 'host_receipt_published')
        require(item.content.encode('utf-8') not in path.read_bytes(), 'host_reference_body_persisted')

    def wait(sid, aid):
        result = wait_for(lambda: store.action(sid, aid),
                          lambda value: value['status'] in ('committed', 'interrupted'),
                          time.monotonic() + 4, 'host_reference_worker_timeout')
        require(result['status'] == 'committed', 'host_reference_action_interrupted')
        assert_private(sid)
        return runtime.snapshot(sid, learner)

    def action(sid, state, kind, aid, payload=None):
        options = dict(reference={} if payload is None else payload) if kind.startswith('reference_') else {}
        runtime.action(sid, learner, kind, aid, int(state['state_version']), **options)
        return wait(sid, aid)

    try:
        open_host()
        sid = runtime.create(body, 'host-fixture-create')['session_id']
        state = wait(sid, 'create')
        for index in (1, 2):
            state = action(sid, state, 'hint_next', 'host-hint-' + str(index))
        require((state['status'], state['level'], len(state['cards']), state['model_calls']) ==
                ('WaitingStudent', 3, 3, 0), 'host_fixture_not_l3')
        state = action(sid, state, 'reference_request', 'host-reference-request')
        require(state['reference']['status'] == 'awaiting_confirmation', 'host_challenge_missing')
        challenge = state['reference']['challenge']
        require(challenge['state_version'] == state['state_version'], 'host_challenge_version_mismatch')
        require(not fault.lookups and not fault.calls, 'host_request_receipt_io')
        issuance = {key: challenge[key] for key in ('session_id', 'revision', 'level', 'policy',
                                                    'challenge_id', 'action_id', 'state_version')}
        issuance.update(issue=binding.issue_id, ttl_seconds=300)
        receipt_id = ui.request('POST', route + '/ui-receipts', issuance, status=201)['receipt_id']
        state = action(sid, state, 'reference_confirm', 'host-reference-confirm',
                       dict(challenge_id=challenge['challenge_id'], receipt_id=receipt_id))
        require((state['status'], state['reference']['status']) == ('WaitingStudent', 'pending'),
                'host_lost_reply_not_pending')
        original = (receipt_id, binding.project_id, challenge['run_id'],
                    challenge['action_id'], int(challenge['state_version']))
        require(fault.calls == [original] and fault.acknowledgements == [False],
                'host_consume_fault_invalid')
        before_recovery = (len(fault.lookups), len(fault.calls))
        close_host()
        open_host()
        state = runtime.snapshot(sid, learner)
        require(state['status'] == 'RecoveryRequired' and state['cards'] == [] and
                state['reference'] == dict(status='revalidation_required'), 'host_restart_not_quarantined')
        require((len(fault.lookups), len(fault.calls)) == before_recovery, 'host_restart_receipt_io')
        assert_private(sid)
        runtime.recover(sid, learner, 'host-recover', int(state['state_version']), body['question'])
        state = wait(sid, 'host-recover')
        require((len(fault.lookups), len(fault.calls)) == before_recovery, 'host_revalidation_receipt_io')
        require(state['reference']['status'] == 'reconcile_required', 'host_reconcile_not_required')
        state = action(sid, state, 'reference_reconcile', 'host-reference-reconcile')
        require(fault.calls == [original, original] and fault.acknowledgements == [False, True],
                'host_original_receipt_replay_invalid')
        require(state['reference']['status'] == 'confirmed' and
                state['reference'].get('body_available') is False and
                state['reference'].get('sha256') == item.sha256, 'host_metadata_publication_invalid')
        before_released_replay = (len(fault.lookups), len(fault.calls))
        state = action(sid, state, 'reference_reconcile', 'host-reference-reconcile-released')
        require((len(fault.lookups), len(fault.calls)) == before_released_replay,
                'host_released_replay_receipt_io')
        require((state['status'], state['level'], len(state['cards']), state['model_calls']) ==
                ('WaitingStudent', 3, 3, 0) and state['reference']['status'] == 'confirmed' and
                state['reference'].get('body_available') is False, 'host_teaching_state_changed')
        require(store.model_used(sid) == 0, 'host_model_reserved')
        assert_private(sid)
        return dict(status='passed', real_core=True, teaching_evidence='fixture',
                    reviewed_material='synthetic_fixture', reviewers='synthetic_fixture_reviewers',
                    real_reviewed_reference='not_available', GUI_signoff='not_run',
                    fault_injection='discard_successful_consume_response', receipt_issuances=1,
                    consume_attempts=2, acknowledged_replays=[False, True],
                    challenge_committed_version='verified', agent_database_reopened=True,
                    restart_quarantine='verified', cards_hidden_before_revalidation=True,
                    restart_receipt_calls=0, revalidation_receipt_calls=0,
                    same_receipt_run_action_version='verified', final_reference_status='confirmed',
                    released_replay_additional_calls=0, body_available=False,
                    material_body_in_database=False, receipt_in_public_persistence=False,
                    level=3, card_count=3, model_calls=0, job_count=0, teaching_HTTP_L4='disabled')
    finally:
        close_host()
