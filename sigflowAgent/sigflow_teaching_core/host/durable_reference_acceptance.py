"""Real Core, synthetic teaching context, deliberately lost consume response."""
from dataclasses import asdict
import hashlib

from .acceptance_support import require
from .durable_reference import DurableReferenceGate
from .reference_journal import ReferenceJournal
from .store import DurableStore
from ..clients.gateway import GatewayError


def exercise_durable_reference(directory, binding, item, port, ui, route):
    path = directory / 'reference-host.sqlite'
    store = DurableStore(path, binding.instance_id)

    def state(version):
        return dict(state_version=str(version), status='WaitingStudent', level=3,
                    run_id='probe-run', stage='E5', cards=[])

    class LossyPort:
        def __init__(self):
            self.attempts = 0
            self.acknowledgements = []

        def lookup(self, receipt_id):
            return port.lookup(receipt_id)

        def consume(self, *args):
            self.attempts += 1
            result = port.consume(*args)
            self.acknowledgements.append(result['replayed'])
            if self.attempts == 1:
                # Core actually committed; the caller deliberately sees a timeout.
                raise GatewayError('TIMEOUT')
            return result

    fault = LossyPort()
    try:
        store.create(binding.session_id, asdict(binding), hashlib.sha256(b'fixture').hexdigest(),
                     'rule', 'diagnose', state(7))
        journal = ReferenceJournal(store)
        gate = DurableReferenceGate(binding, 'probe-run', lambda _: item, journal)
        challenge = gate.request(7)
        body = {key: challenge[key] for key in ('session_id', 'revision', 'level', 'policy',
                                               'challenge_id', 'action_id', 'state_version')}
        body.update(issue=binding.issue_id, ttl_seconds=300)
        receipt_id = ui.request('POST', route + '/ui-receipts', body, status=201)['receipt_id']
        try:
            gate.confirm(receipt_id, challenge['challenge_id'], 7, fault)
        except GatewayError as error:
            require(error.code == 'TIMEOUT', 'durable_wrong_failure')
        else:
            require(False, 'durable_fault_not_injected')
        require(journal.get(binding, 'probe-run')['phase'] == 'consuming', 'durable_intent_missing')
        require(port.lookup(receipt_id)['status'] == 'consumed', 'durable_core_did_not_commit')
        store.close()
        store = DurableStore(path, binding.instance_id)
        journal = ReferenceJournal(store)
        gate = DurableReferenceGate(binding, 'probe-run', lambda _: item, journal)
        row = store.get(binding.session_id)
        require(row['state']['status'] == 'RecoveryRequired', 'durable_restart_not_quarantined')
        version = int(row['state']['state_version'])
        try:
            gate.reconcile(version, fault)
        except ValueError:
            pass
        else:
            require(False, 'durable_unvalidated_recovery_allowed')
        require(fault.attempts == 1, 'durable_recovery_executed_automatically')
        # This probe context is synthetic, not a real teaching revalidation.
        store.claim(binding.session_id, 'fixture-revalidation', 'fixture', version)
        store.commit(binding.session_id, 'fixture-revalidation', state(version + 1), None, {})
        result = gate.reconcile(version + 1, fault)
        require(result['sha256'] == item.sha256 and result['content'] == item.content,
                'durable_publication_invalid')
        require(fault.acknowledgements == [False, True], 'durable_core_replay_invalid')
        require(journal.get(binding, 'probe-run')['phase'] == 'released', 'durable_release_missing')
        gate.reconcile(version + 1, fault)
        require(fault.attempts == 2, 'durable_released_replay_consumed_again')
        require(item.content.encode('utf-8') not in path.read_bytes(), 'durable_content_persisted')
        return dict(status='passed', real_core=True, teaching_context='synthetic',
                    fault_injection='discard_successful_consume_response',
                    agent_database_reopened=True, restart_quarantine='verified',
                    same_receipt_run_action_version='verified', consume_attempts=2,
                    acknowledged_replays=[False, True], receipt_issuances=1,
                    final_phase='released', released_replay_additional_calls=0,
                    material_body_in_journal=False, teaching_HTTP_L4='disabled')
    finally:
        store.close()
