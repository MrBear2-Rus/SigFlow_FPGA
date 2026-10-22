"""Explicit, recoverable receipt publication; production HTTP L4 stays disabled.

Only metadata is journalled. The trusted material provider is queried again
before every publication, including replay. Unknown external outcomes retain
their original receipt/run/action/version; recovery never issues a new receipt.
"""
from dataclasses import asdict
import hashlib
import json
import re
import time
from uuid import uuid4

from .reference import POLICY, ReviewedReference
from ..runtime.gateway_session import SessionBinding


class DurableReferenceGate:
    def __init__(self, binding, run_id, provider, journal, *, metadata_only=False):
        if (not isinstance(binding, SessionBinding)
                or type(run_id) is not str
                or re.fullmatch(r'[A-Za-z0-9_-]{1,128}', run_id) is None
                or not callable(provider) or type(metadata_only) is not bool):
            raise ValueError('invalid_reference_config')
        self.binding = binding
        self.run_id = run_id
        self.provider = provider
        self.journal = journal
        self.metadata_only = metadata_only

    def _context(self, version):
        if type(version) is not int or not 0 <= version < 2**63:
            raise ValueError('invalid_state_version')
        row = self.journal.store.get(self.binding.session_id)
        if (row['binding'] != asdict(self.binding)
                or row['state'].get('run_id') != self.run_id):
            raise ValueError('reference_scope_mismatch')
        if (row['cancel_requested'] or row['state']['status'] != 'WaitingStudent'
                or row['state'].get('level') != 3):
            raise ValueError('reference_context_changed')
        if int(row['state']['state_version']) != version:
            raise ValueError('stale_state')

    def _material(self, challenge=None):
        try:
            item = self.provider(self.binding)
            if not isinstance(item, ReviewedReference):
                raise ValueError()
            item.__post_init__()
            review_hash = hashlib.sha256(json.dumps(
                sorted(item.reviewers), separators=(',', ':')).encode('utf-8')).hexdigest()
            if challenge is not None and (
                    item.reference_id != challenge['reference_id']
                    or item.sha256 != challenge['reference_sha256']
                    or review_hash != challenge['review_sha256']):
                raise ValueError()
            return item, review_hash
        except Exception:
            # A provider exception can contain source text, private paths or keys.
            raise ValueError('reference_material_unavailable') from None

    def request(self, state_version):
        self._context(state_version)
        challenge = self._draft(state_version)
        self.journal.prepare(self.binding, self.run_id, challenge)
        return dict(challenge)

    def draft(self, state_version, published_version):
        """Build metadata for the host's next atomic action commit; writes nothing."""
        self._context(state_version)
        if (type(published_version) is not int or published_version != state_version + 1
                or published_version >= 2**63 - 1):
            raise ValueError('invalid_state_version')
        return self._draft(published_version)

    def _draft(self, state_version):
        previous = self.journal.get(self.binding, self.run_id)
        if previous is not None and (previous['phase'] == 'consuming'
                or (previous['phase'] == 'invalid' and previous['receipt_id'] is not None)):
            raise ValueError('reference_pending')
        item, review_hash = self._material()
        challenge = dict(
            challenge_id=uuid4().hex, action_id='reference-' + uuid4().hex,
            session_id=self.binding.session_id, project_id=self.binding.project_id,
            revision=self.binding.revision, learner_id=self.binding.learner_id,
            issue_id=self.binding.issue_id, run_id=self.run_id,
            reference_id=item.reference_id, reference_sha256=item.sha256,
            review_sha256=review_hash, level='l4', policy=POLICY,
            state_version=str(state_version), expires_at_epoch=str(int(time.time()) + 300))
        return challenge

    def confirm(self, receipt_id, challenge_id, state_version, port):
        self._context(state_version)
        record = self.journal.get(self.binding, self.run_id)
        if (record is None or record['phase'] == 'invalid'
                or record['challenge']['challenge_id'] != challenge_id
                or record['challenge']['state_version'] != str(state_version)):
            raise ValueError('reference_challenge_stale')
        if record['phase'] == 'consuming':
            raise ValueError('reference_pending')
        if record['phase'] == 'released':
            if receipt_id != record['receipt_id']:
                raise ValueError('reference_receipt_conflict')
            return self._publish(record, state_version)
        self._live_challenge(record['challenge'])
        return self._consume(record, receipt_id, state_version, port, recovery=False)

    def reconcile(self, state_version, port):
        """Host-authenticated explicit recovery after current evidence revalidation."""
        self._context(state_version)
        record = self.journal.get(self.binding, self.run_id)
        if record is None or record['phase'] not in ('consuming', 'released'):
            raise ValueError('reference_not_pending')
        if record['phase'] == 'released':
            return self._publish(record, state_version)
        return self._consume(record, record['receipt_id'], state_version, port, recovery=True)

    @staticmethod
    def _live_challenge(challenge):
        if time.time() >= int(challenge['expires_at_epoch']):
            raise ValueError('reference_challenge_stale')

    def _consume(self, record, receipt_id, current_version, port, *, recovery):
        challenge = record['challenge']
        self._material(challenge)
        receipt = port.lookup(receipt_id)
        expected = {key: challenge[key] for key in (
            'session_id', 'project_id', 'revision', 'challenge_id', 'action_id',
            'policy', 'state_version', 'level')}
        allowed_status = ('active', 'consumed') if recovery else ('active',)
        if (type(receipt) is not dict or receipt.get('receipt_id') != receipt_id
                or any(receipt.get(key) != value for key, value in expected.items())
                or receipt.get('status') not in allowed_status):
            raise ValueError('receipt_scope_mismatch')
        if receipt['status'] == 'active':
            self._live_challenge(challenge)
        # This durable reservation precedes the irreversible external consume.
        # A timeout, crash or later local failure leaves the same operation pending.
        self.journal.claim(self.binding, self.run_id, challenge['challenge_id'], receipt_id,
                           expected_version=current_version)
        if receipt['status'] == 'active':
            # Waiting for a SQLite writer can outlive the approval deadline.
            self._live_challenge(challenge)
        acknowledged = port.consume(receipt_id, challenge['project_id'], self.run_id,
                                    challenge['action_id'], int(challenge['state_version']))
        if (type(acknowledged) is not dict
                or acknowledged.get('receipt_id') != receipt_id
                or acknowledged.get('action_id') != challenge['action_id']
                or acknowledged.get('status') != 'consumed'
                or acknowledged.get('consumed') is not True
                or type(acknowledged.get('replayed')) is not bool):
            raise ValueError('receipt_scope_mismatch')
        # Re-read both review context and body after the blocking external call.
        return self._publish(record, current_version)

    def _publish(self, record, current_version):
        challenge = record['challenge']
        item, _ = self._material(challenge)
        self.journal.release(self.binding, self.run_id, challenge['challenge_id'],
                             expected_version=current_version)
        result = dict(reference_id=item.reference_id, sha256=item.sha256,
                      policy=POLICY, reviewers=list(item.reviewers))
        if not self.metadata_only:
            result['content'] = item.content
        return result

    def invalidate(self):
        self.journal.invalidate(self.binding, self.run_id)
