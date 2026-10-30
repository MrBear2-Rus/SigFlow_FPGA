"""Restricted Gateway execution with a durable, credential-free submission ledger.

Approval registration and resolver callbacks belong to the trusted host, never
to model output. Gateway's approval hash is opaque UI material: neither the
internal fingerprint nor ApprovalPackage.digest is its wire replacement.
"""
from collections.abc import Mapping
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import http.client
import json
import math
import os
import re
from pathlib import Path
import sqlite3
from threading import RLock
import time

from ..checkers.plan import PlanChecker
from ..clients.gateway import (GatewayClient, GatewayConfig, GatewayError, PROTOCOL,
                               _json_object, _json_constant)
from ..contracts.plan import ApprovalPackage, build_plan_card
from ..domain import Evidence, RunRequest, VerificationResult
from ..pedagogy import valid_evidence

_PROVIDERS = {('eda.synth', 'yosys'): 'eda-synth-yosys',
              ('eda.sim.build', 'verilator'): 'eda-sim-verilator',
              ('eda.sim.run', 'verilator'): 'eda-sim-verilator-run'}
_JOB_TYPES = {'eda.synth': 'synth', 'eda.sim.build': 'sim.build', 'eda.sim.run': 'sim.run'}
_ACTIVE = {'Created', 'Validating', 'Queued', 'Running', 'ValidatingArtifact', 'Cancelling'}
_TERMINAL = {'Succeeded', 'Failed', 'Cancelled', 'TimedOut', 'Rejected', 'Unknown'}


def _digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'),
        ensure_ascii=True, allow_nan=False).encode('utf-8')).hexdigest()


@dataclass(frozen=True)
class ApprovedPlan:
    package: ApprovalPackage
    gateway_plan_hash: str


@dataclass(frozen=True)
class _BoundVerification:
    adapter: object
    fingerprint: str
    approval: ApprovedPlan
    run_id: str | None = None

    def bind_run(self, run_id):
        self.adapter._run_keys(self.approval.package.binding['internal_fingerprint'], run_id,
                               len(self.approval.package.card['steps']))
        return _BoundVerification(self.adapter, self.fingerprint, self.approval, run_id)

    def _key(self, plan, index, key):
        if self.run_id is not None:
            keys = self.adapter._run_keys(plan.fingerprint, self.run_id, len(plan.steps))
            if type(index) is not int or not 0 <= index < len(keys) or _digest(key) != keys[index]:
                raise GatewayError('POLICY_DENIED')
        return key

    def _plan(self, plan):
        if plan.fingerprint != self.fingerprint:
            raise GatewayError('POLICY_DENIED')
        return plan

    def authorized(self, plan, grant_ref):
        return self.adapter.authorized(self._plan(plan), grant_ref, _approved=self.approval)

    def execute(self, plan, step_index, grant_ref, idempotency_key):
        return self.adapter.execute(self._plan(plan), step_index, grant_ref,
                                    self._key(plan, step_index, idempotency_key),
                                    _approved=self.approval, _run_id=self.run_id)

    def observe(self, plan, step_index, idempotency_key):
        return self.adapter.observe(self._plan(plan), step_index,
                                   self._key(plan, step_index, idempotency_key), _approved=self.approval)

    def request_cancel(self, plan):
        return self.adapter.request_cancel(self._plan(plan), _approved=self.approval, _run_id=self.run_id)

    def cancel_owned(self, plan):
        return self.adapter.cancel_owned(self._plan(plan), _approved=self.approval, _run_id=self.run_id)

    def reconcile(self, plan, step_index, grant_ref, idempotency_key, job_binding):
        return self.adapter.reconcile(self._plan(plan), step_index, grant_ref,
                                      self._key(plan, step_index, idempotency_key),
                                      job_binding, _approved=self.approval)


class GatewayVerification:
    """Synchronous VerificationPort; bounded polling may return a nonterminal state.

    report_resolver(job_id, *, project_id, revision, snapshot_id) returns verified
    Evidence or None. expected_resolver(plan, step_index, evidence) optionally
    certifies an expected observation; process success alone is never proof.
    """
    def __init__(self, config: GatewayConfig, journal_path, report_resolver, *,
                 approval_resolver=None, expected_resolver=None,
                 wait_timeout=5.0, poll_interval=0.05):
        if not isinstance(config, GatewayConfig) or not callable(report_resolver):
            raise ValueError('invalid_verification_config')
        for callback in (approval_resolver, expected_resolver):
            if callback is not None and not callable(callback):
                raise ValueError('invalid_verification_config')
        for value, upper in ((wait_timeout, 60), (poll_interval, 5)):
            if type(value) not in (int, float) or not math.isfinite(value) or not 0 < value <= upper:
                raise ValueError('invalid_verification_wait')
        journal_path = os.fspath(journal_path)
        if (type(journal_path) is not str or not journal_path.strip()
                or journal_path == ':memory:' or journal_path.startswith('file:')):
            raise ValueError('durable_journal_required')
        journal_path = str(Path(journal_path).resolve())
        self.journal_path = journal_path
        self.config, self.report_resolver = config, report_resolver
        self.approval_resolver, self.expected_resolver = approval_resolver, expected_resolver
        self.wait_timeout, self.poll_interval = wait_timeout, poll_interval
        self._client, self._lock, self._approvals = GatewayClient(config), RLock(), {}
        self._closed = False
        self._db = sqlite3.connect(journal_path, timeout=3, check_same_thread=False)
        self._db.row_factory = sqlite3.Row
        self._db.execute('PRAGMA synchronous=FULL')
        self._db.execute('CREATE TABLE IF NOT EXISTS cancelled_plans (scope_digest TEXT PRIMARY KEY)')
        self._db.execute('''CREATE TABLE IF NOT EXISTS verification_requests (
            key_digest TEXT PRIMARY KEY, request_digest TEXT NOT NULL,
            scope_digest TEXT NOT NULL, instance_id TEXT NOT NULL,
            project_id TEXT NOT NULL, revision TEXT NOT NULL, snapshot_id TEXT NOT NULL,
            plan_hash TEXT NOT NULL, step_id TEXT NOT NULL, capability TEXT NOT NULL,
            plugin_id TEXT NOT NULL, job_id TEXT, state TEXT NOT NULL,
            plan_digest TEXT NOT NULL DEFAULT '', cancel_requested INTEGER NOT NULL DEFAULT 0,
            approval_digest TEXT NOT NULL DEFAULT '',
            UNIQUE(scope_digest, step_id))''')
        columns = {r['name'] for r in self._db.execute('PRAGMA table_info(verification_requests)')}
        for name, definition in (('plan_digest', "TEXT NOT NULL DEFAULT ''"),
                                 ('cancel_requested', 'INTEGER NOT NULL DEFAULT 0'),
                                 ('approval_digest', "TEXT NOT NULL DEFAULT ''")):
            if name not in columns:
                self._db.execute(f'ALTER TABLE verification_requests ADD COLUMN {name} {definition}')
        self._db.commit()

    def close(self):
        with self._lock:
            if not self._closed:
                self._db.close()
                self._closed = True

    @staticmethod
    def _checked(plan):
        try:
            plan = PlanChecker().check(plan)
            for value in (plan.project_id, plan.revision, plan.snapshot_id):
                GatewayClient._identifier(value)
            for step in plan.steps:
                if (step.capability, step.provider) not in _PROVIDERS or 'timeout_ms' in step.params:
                    raise GatewayError('UNSUPPORTED_MAPPING')
            return plan
        except (ValueError, AttributeError, TypeError):
            raise GatewayError('UNSUPPORTED_MAPPING') from None

    def register_approval(self, plan, package: ApprovalPackage, gateway_plan_hash: str):
        """Register exact trusted UI handoff material in memory; never grants access."""
        plan = self._checked(plan)
        if (type(package) is not ApprovalPackage or type(gateway_plan_hash) is not str
                or not gateway_plan_hash.strip() or len(gateway_plan_hash) > 4096):
            raise ValueError('invalid_approval_binding')
        try:
            binding = package.binding
            expected = build_plan_card(plan, binding['plan_id'], int(binding['state_version']))
            # Compare full decoded material, including format/card/plan/version.
            if _digest(json.loads(package._material_json)) != _digest(json.loads(expected._material_json)):
                raise ValueError('approval_plan_mismatch')
        except (KeyError, TypeError, ValueError, OverflowError):
            raise ValueError('approval_plan_mismatch') from None
        with self._lock:
            self._approvals[plan.fingerprint] = ApprovedPlan(expected, gateway_plan_hash)

    def _approval(self, plan):
        approved = self._approvals.get(plan.fingerprint)
        if approved is None and self.approval_resolver is not None:
            approved = self.approval_resolver(plan)
            if type(approved) is not ApprovedPlan:
                raise GatewayError('POLICY_DENIED')
            self.register_approval(plan, approved.package, approved.gateway_plan_hash)
            approved = self._approvals[plan.fingerprint]
        if approved is None:
            raise GatewayError('POLICY_DENIED')
        return approved

    def bind(self, plan, gateway_plan_hash):
        """Freeze this session's approval; later registrations cannot retarget it."""
        with self._lock:
            plan = self._checked(plan)
            approved = self._approval(plan)
            if approved.gateway_plan_hash != gateway_plan_hash:
                raise GatewayError('POLICY_DENIED')
            return _BoundVerification(self, plan.fingerprint, approved)

    def _request(self, method, path, body=None, key=None, *, timeout=None):
        # Paths are constructed only by validated identifier route factories.
        connection = http.client.HTTPConnection('127.0.0.1', self.config.port,
                                                timeout=timeout or self.config.timeout)
        headers = {'Accept': 'application/json', 'Authorization': 'Bearer ' + self.config.token}
        if body is not None:
            headers['Content-Type'] = 'application/json'
        if key is not None:
            headers['Idempotency-Key'] = key
        try:
            connection.request(method, path, body=None if body is None else json.dumps(body), headers=headers)
            response = connection.getresponse()
            status, raw = response.status, response.read(GatewayClient.MAX_RESPONSE_BYTES + 1)
            if 300 <= status < 400:
                raise GatewayError('REDIRECT_DENIED')
            if len(raw) > GatewayClient.MAX_RESPONSE_BYTES:
                raise GatewayError('RESOURCE_EXHAUSTED')
        except TimeoutError:
            raise GatewayError('TIMEOUT') from None
        except (OSError, http.client.HTTPException):
            raise GatewayError('SERVICE_UNAVAILABLE') from None
        finally:
            connection.close()
        try:
            envelope = json.loads(raw, object_pairs_hook=_json_object, parse_constant=_json_constant)
        except (ValueError, RecursionError):
            raise GatewayError('INVALID_RESPONSE') from None
        if type(envelope) is not dict or envelope.get('schema_version') != PROTOCOL:
            raise GatewayError('PROTOCOL_MISMATCH')
        if any(type(envelope.get(k)) is not str or not envelope[k] for k in ('request_id', 'trace_id')):
            raise GatewayError('INVALID_RESPONSE')
        if status not in ((200, 202) if method == 'POST' else (200,)):
            error = envelope.get('error')
            if (type(error) is not dict or type(error.get('code')) is not str
                    or type(error.get('message')) is not str or type(error.get('retryable')) is not bool
                    or 'data' in envelope):
                raise GatewayError('INVALID_RESPONSE')
            raise GatewayError(error['code'])
        if 'error' in envelope or type(envelope.get('data')) is not dict:
            raise GatewayError('INVALID_RESPONSE')
        return envelope['data']

    def _validate(self, plan, grant_ref, _approved=None):
        plan = self._checked(plan)
        grant_ref = GatewayClient._identifier(grant_ref)
        approved = _approved or self._approval(plan)
        self._client.health()  # unauthenticated instance/protocol/edition handshake first
        context = self._request('GET', '/api/v1/projects/' + plan.project_id + '/context')
        if (context.get('project_id') != plan.project_id
                or type(context.get('revision')) is not str or not context['revision']
                or type(context.get('dirty')) is not bool
                or type(context.get('synchronized')) is not bool):
            raise GatewayError('INVALID_RESPONSE')
        if context['revision'] != plan.revision or context['dirty'] or not context['synchronized']:
            raise GatewayError('STALE_REVISION')
        capabilities = self._client.capabilities()
        for step in plan.steps:
            plugin = _PROVIDERS[(step.capability, step.provider)]
            if not any(c.id == step.capability and c.ready and not c.disabled and c.plugin_id == plugin
                       for c in capabilities.items):
                raise GatewayError('CAPABILITY_UNAVAILABLE')
        grant = self._request('GET', '/api/v1/grants/' + grant_ref)
        if (any(grant.get(k) != expected for k, expected in (
                ('grant_id', grant_ref), ('project_id', plan.project_id), ('revision', plan.revision),
                ('snapshot_id', plan.snapshot_id), ('plan_hash', approved.gateway_plan_hash)))
                or grant.get('status') != 'active' or type(grant.get('steps')) is not list
                or _digest(grant['steps']) != _digest(approved.package.card['steps'])
                or type(grant.get('max_jobs')) is not int or grant['max_jobs'] != plan.max_jobs
                or type(grant.get('used_jobs')) is not int or not 0 <= grant['used_jobs'] <= grant['max_jobs']):
            raise GatewayError('POLICY_DENIED')
        try:
            expiry = datetime.strptime(grant['expires_at'], '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=timezone.utc)
            if expiry <= datetime.now(timezone.utc):
                raise ValueError('expired')
        except (KeyError, ValueError, TypeError):
            raise GatewayError('POLICY_DENIED') from None
        return plan, approved, grant

    def authorized(self, plan, grant_ref, *, _approved=None):
        with self._lock:
            try:
                _, _, grant = self._validate(plan, grant_ref, _approved)
                return grant['used_jobs'] < grant['max_jobs']
            except (ValueError, TypeError, AttributeError):
                return False

    def _material(self, plan, index, grant_ref, approved, key):
        if type(index) is not int or not 0 <= index < len(plan.steps):
            raise GatewayError('INVALID_ARGUMENT')
        if (type(key) is not str or not 1 <= len(key) <= 256
                or any(ord(c) < 33 or ord(c) > 126 for c in key)):
            raise GatewayError('INVALID_ARGUMENT')
        step = approved.package.card['steps'][index]
        scope = _digest([self.config.instance_id, plan.fingerprint, approved.package.digest,
                         approved.gateway_plan_hash, grant_ref])
        # Symbolic request is stable before/after dependencies are resolved.
        symbolic = dict(snapshot_id=plan.snapshot_id, expected_revision=plan.revision,
            capability=step['capability'], grant_id=grant_ref, plugin_id=_PROVIDERS[(step['capability'], step['provider'])],
            params=step['params'], plan_hash=approved.gateway_plan_hash, step_id=step['step_id'])
        digest = _digest([self.config.instance_id, plan.project_id, plan.fingerprint,
                          approved.package.digest, symbolic])
        return step, scope, symbolic, digest, _digest(key)

    def _row(self, key_digest):
        return self._db.execute('SELECT * FROM verification_requests WHERE key_digest=?', (key_digest,)).fetchone()

    @staticmethod
    def _run_keys(fingerprint, run_id, step_count):
        if type(run_id) is not str or re.fullmatch('[a-f0-9]{32}', run_id) is None:
            raise GatewayError('POLICY_DENIED')
        return tuple(_digest(f'{run_id}:{fingerprint}:{index}') for index in range(step_count))

    def _cancel_scope(self, plan, approved, run_id=None):
        material = [self.config.instance_id, plan.fingerprint, approved.gateway_plan_hash,
                    approved.package.digest]
        if run_id is not None:
            self._run_keys(plan.fingerprint, run_id, len(plan.steps))
            material += ['run', run_id]
        return _digest(material)

    def _legacy_cancel_scope(self, plan, approved):
        return _digest([self.config.instance_id, plan.fingerprint, approved.gateway_plan_hash])

    def _cancelled(self, plan, approved, run_id=None):
        return self._db.execute('SELECT 1 FROM cancelled_plans WHERE scope_digest IN (?,?,?)',
            (self._cancel_scope(plan, approved), self._legacy_cancel_scope(plan, approved),
             self._cancel_scope(plan, approved, run_id))).fetchone() is not None

    def request_cancel(self, plan, *, _approved=None, _run_id=None):
        """Commit a plan or exact host-run fence without the network lock."""
        if self._closed:
            return
        plan = self._checked(plan)
        approved = _approved or self._approvals.get(plan.fingerprint)
        if approved is None:
            raise GatewayError('POLICY_DENIED')
        connection = sqlite3.connect(self.journal_path, timeout=3)
        try:
            connection.execute('PRAGMA synchronous=FULL')
            with connection:
                connection.execute('INSERT OR IGNORE INTO cancelled_plans VALUES (?)',
                                   (self._cancel_scope(plan, approved, _run_id),))
        finally:
            connection.close()

    def _dependencies(self, plan, step, scope):
        rows = self._db.execute('SELECT * FROM verification_requests WHERE scope_digest=?', (scope,)).fetchall()
        if any(r['cancel_requested'] != 0 for r in rows):
            raise GatewayError('POLICY_DENIED')
        if any(r['step_id'] != step['step_id'] and r['state'] in (_TERMINAL - {'Succeeded'}) | {'ObservationMismatch'}
               for r in rows):
            raise GatewayError('POLICY_DENIED')
        for dependency in step['depends_on']:
            if not any(r['step_id'] == dependency and r['state'] == 'Succeeded' and r['job_id'] for r in rows):
                raise GatewayError('POLICY_DENIED')
        params = dict(step['params'])
        if 'build_step_id' in params:
            build_step = params.pop('build_step_id')
            build = next((r for r in rows if r['step_id'] == build_step and r['capability'] == 'eda.sim.build'
                          and r['plugin_id'] == _PROVIDERS[('eda.sim.build', step['provider'])] and r['state'] == 'Succeeded'), None)
            if build is None:
                raise GatewayError('POLICY_DENIED')
            params['build_job_id'] = build['job_id']
        return params

    def execute(self, plan, step_index, grant_ref, idempotency_key, *, _approved=None, _run_id=None):
        with self._lock:
            plan, approved, grant = self._validate(plan, grant_ref, _approved)
            step, scope, body, digest, key_digest = self._material(plan, step_index, grant_ref, approved, idempotency_key)
            if _run_id is not None and key_digest != self._run_keys(plan.fingerprint, _run_id, len(plan.steps))[step_index]:
                raise GatewayError('POLICY_DENIED')
            row = self._row(key_digest)
            if row is not None:
                if row['request_digest'] != digest:
                    raise GatewayError('IDEMPOTENCY_CONFLICT')
                if not row['approval_digest']:
                    # Legacy records require the original full request/grant
                    # digest before they can acquire a read-only approval scope.
                    with self._db:
                        self._db.execute('UPDATE verification_requests SET approval_digest=? WHERE key_digest=?',
                                         (approved.package.digest, key_digest))
                    row = self._row(key_digest)
                if row['job_id'] is None:
                    return VerificationResult('Unknown', None, None)
                return self._observe(plan, step_index, row)
            if self._cancelled(plan, approved, _run_id):
                raise GatewayError('POLICY_DENIED')
            body['params'] = self._dependencies(plan, step, scope)
            if grant['used_jobs'] >= grant['max_jobs']:
                raise GatewayError('POLICY_DENIED')
            # Commit before touching POST. Any subsequent exception preserves
            # an Unknown reservation; no retry can create another submission.
            try:
                with self._db:
                    inserted = self._db.execute('''INSERT INTO verification_requests
                        SELECT ?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?
                        WHERE NOT EXISTS (SELECT 1 FROM cancelled_plans WHERE scope_digest IN (?,?,?))''',
                        (key_digest, digest, scope, self.config.instance_id,
                        plan.project_id, plan.revision, plan.snapshot_id, approved.gateway_plan_hash,
                        step['step_id'], step['capability'], body['plugin_id'], None, 'Unknown', plan.fingerprint, 0,
                        approved.package.digest, self._cancel_scope(plan, approved),
                        self._legacy_cancel_scope(plan, approved),
                        self._cancel_scope(plan, approved, _run_id))).rowcount
                    if not inserted:
                        raise GatewayError('POLICY_DENIED')
            except sqlite3.IntegrityError:
                row = self._row(key_digest)
                if row is not None and row['request_digest'] == digest:
                    return VerificationResult('Unknown', None, None)
                raise GatewayError('IDEMPOTENCY_CONFLICT') from None
            try:
                data = self._request('POST', '/api/v1/projects/' + plan.project_id + '/jobs', body, idempotency_key)
                job_id = GatewayClient._identifier(data.get('job_id'))
                if (type(data.get('state')) is not str or data['state'] not in _ACTIVE | _TERMINAL
                        or data.get('revision') != plan.revision):
                    raise GatewayError('INVALID_RESPONSE')
                with self._db:
                    self._db.execute('UPDATE verification_requests SET job_id=?, state=? WHERE key_digest=?',
                                     (job_id, data['state'], key_digest))
            except GatewayError:
                return VerificationResult('Unknown', None, None)
            if self._cancelled(plan, approved, _run_id):
                self.cancel_owned(plan, _approved=approved, _run_id=_run_id)
            return self._observe(plan, step_index, self._row(key_digest))

    def _job(self, job_id, plan, capability, *, timeout=None):
        data = self._request('GET', '/api/v1/jobs/' + GatewayClient._identifier(job_id), timeout=timeout)
        if (data.get('job_id') != job_id or data.get('project_id') != plan.project_id
                or data.get('job_type') != _JOB_TYPES[capability]
                or type(data.get('state')) is not str or data['state'] not in _ACTIVE | _TERMINAL
                or type(data.get('exit_code')) is not int
                or any(type(data.get(k)) is not str for k in ('created_at', 'updated_at'))):
            raise GatewayError('INVALID_RESPONSE')
        return data

    def observe(self, plan, step_index, idempotency_key, *, _approved=None):
        """GET-only continuation bound to an existing exact approval/step record.

        No grant is accepted: a revoked or exhausted grant cannot prevent
        reading a known Job, and a read never authorizes another submission.
        """
        with self._lock:
            plan = self._checked(plan)
            approved = _approved or self._approval(plan)
            step, _, _, _, key_digest = self._material(plan, step_index, 'readonly-observation',
                                                      approved, idempotency_key)
            row = self._row(key_digest)
            expected = dict(instance_id=self.config.instance_id, project_id=plan.project_id,
                revision=plan.revision, snapshot_id=plan.snapshot_id, plan_digest=plan.fingerprint,
                plan_hash=approved.gateway_plan_hash, approval_digest=approved.package.digest,
                step_id=step['step_id'], capability=step['capability'],
                plugin_id=_PROVIDERS[(step['capability'], step['provider'])])
            if row is None or any(row[k] != v for k, v in expected.items()):
                raise GatewayError('POLICY_DENIED')
            if row['job_id'] is None:
                return VerificationResult('Unknown', None, None)
            if row['state'] in {'Failed', 'Cancelled', 'TimedOut', 'Rejected'}:
                return self._journal_result(row, row['state'])
            try:
                self._client.health()
            except GatewayError as error:
                if error.code not in ('TIMEOUT', 'SERVICE_UNAVAILABLE'):
                    raise
                self._observed_state(row['key_digest'], 'Unknown')
                return self._journal_result(row, 'Unknown')
            return self._observe(plan, step_index, row)

    def _report(self, row, plan):
        report = self._client.job_report(row['job_id'], project_id=plan.project_id,
            revision=plan.revision, snapshot_id=plan.snapshot_id)
        if (report['state'] != 'Succeeded' or report['capability'] != row['capability']
                or report.get('plugin_id') != row['plugin_id']):
            raise GatewayError('INVALID_RESPONSE')
        return report

    def _observed_state(self, key_digest, state):
        # Observed failures and policy stops survive both refreshes and writes
        # from another adapter that began before the terminal observation.
        with self._db:
            self._db.execute('''UPDATE verification_requests SET state=
                CASE WHEN state IN ('ObservationMismatch','Failed','Cancelled','TimedOut','Rejected')
                THEN state ELSE ? END
                WHERE key_digest=?''', (state, key_digest))

    def _journal_result(self, row, state, evidence=None, match=None):
        persisted = self._row(row['key_digest'])['state']
        if persisted in {'Failed', 'Cancelled', 'TimedOut', 'Rejected'}:
            state, evidence, match = persisted, None, None
        elif persisted == 'ObservationMismatch' and evidence is not None:
            match = False
        return VerificationResult(state, evidence, match, row['job_id'])

    def _observe(self, plan, index, row):
        if row['state'] in {'Failed', 'Cancelled', 'TimedOut', 'Rejected'}:
            return self._journal_result(row, row['state'])
        deadline, state = time.monotonic() + self.wait_timeout, row['state']
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return self._journal_result(row, state)
            try:
                state = self._job(row['job_id'], plan, row['capability'],
                                  timeout=min(self.config.timeout, remaining))['state']
                if state == 'Succeeded':
                    report = self._report(row, plan)
            except GatewayError:
                # A known Job remains known, but cannot certify success or dependencies.
                self._observed_state(row['key_digest'], 'Unknown')
                return self._journal_result(row, 'Unknown')
            if state != 'Succeeded':
                self._observed_state(row['key_digest'], state)
            if state in _TERMINAL:
                break
            time.sleep(min(self.poll_interval, max(0, deadline - time.monotonic())))
        if state != 'Succeeded':
            return self._journal_result(row, state)
        if report['completeness'] != 'complete':
            self._observed_state(row['key_digest'], state)
            return self._journal_result(row, state)
        try:
            evidence = self.report_resolver(row['job_id'], project_id=plan.project_id,
                                           revision=plan.revision, snapshot_id=plan.snapshot_id)
        except (ValueError, TypeError):
            self._observed_state(row['key_digest'], state)
            return self._journal_result(row, state)
        request = RunRequest(plan.project_id, plan.revision, 'verification', 'verify', 'verify')
        if (not isinstance(evidence, Evidence) or not valid_evidence(evidence, request)
                or evidence.job_id != row['job_id'] or evidence.anchor is None
                or evidence.anchor.snapshot_id != plan.snapshot_id):
            self._observed_state(row['key_digest'], state)
            return self._journal_result(row, state)
        if not any(type(a) is dict and a.get('artifact_id') == evidence.anchor.artifact_id
                   and a.get('sha256') == evidence.anchor.artifact_hash for a in report.get('artifacts', [])):
            self._observed_state(row['key_digest'], state)
            return self._journal_result(row, state)
        match = self.expected_resolver(plan, index, evidence) if self.expected_resolver else None
        if match is not None and type(match) is not bool:
            raise GatewayError('INVALID_RESPONSE')
        if ((match is False and 'observation_mismatch' in plan.stop_conditions)
                or self._row(row['key_digest'])['state'] == 'ObservationMismatch'):
            match = False
            self._observed_state(row['key_digest'], 'ObservationMismatch')
        else:
            self._observed_state(row['key_digest'], state)
        return self._journal_result(row, state, evidence, match)

    def pending(self):
        """Safe metadata view for reconciliation; contains no grant or token."""
        with self._lock:
            return tuple(dict(r) for r in self._db.execute('''SELECT * FROM verification_requests
                WHERE instance_id=? AND state NOT IN
                ('Succeeded','Failed','Cancelled','TimedOut','Rejected','ObservationMismatch')''',
                (self.config.instance_id,)))

    def cancel_owned(self, plan, *, _approved=None, _run_id=None):
        """Cancel only this exact plan's journal-known Jobs; ACK is not a terminal fact.

        Returns safe {job_id, status, cancel_requested} records. A request whose
        outcome is unknown has cancel_requested=False (no verified ACK). No Job
        ID is accepted from the caller and no unknown submission is guessed.
        """
        self.request_cancel(plan, _approved=_approved, _run_id=_run_id)
        with self._lock:
            if self._closed:
                return ()
            plan = self._checked(plan)
            approved = _approved or self._approval(plan)
            owned_keys = self._run_keys(plan.fingerprint, _run_id, len(plan.steps)) if _run_id is not None else ()
            key_filter = (' AND key_digest IN (' + ','.join('?' for _ in owned_keys) + ')') if owned_keys else ''
            rows = self._db.execute('''SELECT * FROM verification_requests
                WHERE instance_id=? AND project_id=? AND revision=? AND snapshot_id=?
                AND plan_digest=? AND plan_hash=? AND approval_digest=? AND job_id IS NOT NULL'''
                + key_filter + ' ORDER BY step_id LIMIT ?', (self.config.instance_id, plan.project_id, plan.revision,
                plan.snapshot_id, plan.fingerprint, approved.gateway_plan_hash,
                approved.package.digest, *owned_keys, plan.max_jobs)).fetchall()
            if not rows:
                return ()
            steps = {s['step_id']: s for s in approved.package.card['steps']}
            results, deadline = [], time.monotonic() + self.wait_timeout
            try:
                self._client.health()
            except GatewayError as error:
                return tuple(dict(job_id=r['job_id'], status=error.code, cancel_requested=False) for r in rows)
            for row in rows:
                job_id, step = row['job_id'], steps.get(row['step_id'])
                if (step is None or row['capability'] != step['capability']
                        or row['plugin_id'] != _PROVIDERS[(step['capability'], step['provider'])]):
                    results.append(dict(job_id=job_id, status='POLICY_DENIED', cancel_requested=False))
                    continue
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    results.append(dict(job_id=job_id, status='TIMEOUT', cancel_requested=False))
                    continue
                try:
                    # GET Job proves current project/type. Exact original plan,
                    # revision/snapshot/step ownership is the committed receipt
                    # in our own ledger; public GET lacks these extra fields.
                    data = self._job(job_id, plan, row['capability'],
                                     timeout=min(self.config.timeout, remaining))
                except GatewayError as error:
                    results.append(dict(job_id=job_id, status=error.code, cancel_requested=False))
                    continue
                if data['state'] not in _ACTIVE:
                    results.append(dict(job_id=job_id, status=data['state'], cancel_requested=False))
                    continue
                if row['cancel_requested'] == 1:
                    results.append(dict(job_id=job_id, status='CancelRequested', cancel_requested=True))
                    continue
                if row['cancel_requested'] == -1:
                    results.append(dict(job_id=job_id, status='Unknown', cancel_requested=False))
                    continue
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    results.append(dict(job_id=job_id, status='TIMEOUT', cancel_requested=False))
                    continue
                # Cancel response loss also needs a committed pre-request
                # marker. Repeated host cancellation must not repeat a POST
                # whose acknowledgement was lost across a restart.
                with self._db:
                    claimed = self._db.execute('''UPDATE verification_requests SET cancel_requested=-1
                        WHERE key_digest=? AND cancel_requested=0''', (row['key_digest'],)).rowcount
                if not claimed:
                    marker = self._row(row['key_digest'])['cancel_requested']
                    results.append(dict(job_id=job_id, status='CancelRequested' if marker == 1 else 'Unknown',
                                        cancel_requested=marker == 1))
                    continue
                try:
                    ack = self._request('POST', '/api/v1/jobs/' + GatewayClient._identifier(job_id) + '/cancel',
                                        timeout=min(self.config.timeout, remaining))
                    if ack.get('job_id') != job_id or ack.get('accepted') is not True:
                        raise GatewayError('INVALID_RESPONSE')
                except GatewayError as error:
                    status = error.code if error.code in {'POLICY_DENIED', 'NOT_FOUND', 'UNAUTHENTICATED'} else 'Unknown'
                    if status != 'Unknown':
                        with self._db:
                            self._db.execute('UPDATE verification_requests SET cancel_requested=0 WHERE key_digest=?',
                                             (row['key_digest'],))
                    results.append(dict(job_id=job_id, status=status, cancel_requested=False))
                    continue
                with self._db:
                    self._db.execute('UPDATE verification_requests SET cancel_requested=1 WHERE key_digest=?',
                                     (row['key_digest'],))
                results.append(dict(job_id=job_id, status='CancelRequested', cancel_requested=True))
            return tuple(results)

    def reconcile(self, plan, step_index, grant_ref, idempotency_key, job_binding: Mapping, *, _approved=None):
        """Host supplies actual Core provenance, never a guessed candidate Job ID.

        GET Job lacks grant/plan/snapshot provenance, so a trusted Core manifest
        binding is required. The current public API alone cannot discover it.
        """
        with self._lock:
            plan = self._checked(plan)
            approved = _approved or self._approval(plan)
            GatewayClient._identifier(grant_ref)
            step, scope, body, digest, key_digest = self._material(plan, step_index, grant_ref, approved, idempotency_key)
            row = self._row(key_digest)
            if row is None or row['request_digest'] != digest:
                raise GatewayError('IDEMPOTENCY_CONFLICT')
            if row['state'] not in _ACTIVE | {'Unknown'}:
                raise GatewayError('POLICY_DENIED')
            body['params'] = self._dependencies(plan, step, scope) if step['depends_on'] else dict(step['params'])
            canonical = {k: body[k] for k in ('snapshot_id', 'capability', 'plugin_id', 'params')}
            # Current C++ StableDump emits unescaped strings. All mapped fields
            # here are restricted ASCII identifiers, making compact sorted JSON
            # equivalent for this deliberately narrow request subset.
            request_hash = hashlib.sha256(json.dumps(canonical, sort_keys=True,
                ensure_ascii=False, separators=(',', ':')).encode()).hexdigest()
            expected = dict(schema='sigflow.edu-agent-job-binding.v1',
                instance_id=self.config.instance_id, project_id=plan.project_id,
                revision=plan.revision, snapshot_id=plan.snapshot_id, capability=step['capability'],
                plan_hash=approved.gateway_plan_hash, step_id=step['step_id'], grant_id=grant_ref,
                idempotency_key=idempotency_key, idempotency_scope=plan.project_id + '|' + step['capability'],
                request_hash=request_hash)
            if not isinstance(job_binding, Mapping) or any(job_binding.get(k) != v for k, v in expected.items()):
                raise GatewayError('POLICY_DENIED')
            job_id = GatewayClient._identifier(job_binding.get('job_id'))
            if row['job_id'] is not None and row['job_id'] != job_id:
                raise GatewayError('IDEMPOTENCY_CONFLICT')
            self._client.health()
            data = self._job(job_id, plan, row['capability'])
            # Validate snapshot/provider provenance when already succeeded.
            candidate = dict(row, job_id=job_id)
            if data['state'] == 'Succeeded':
                self._report(candidate, plan)
            with self._db:
                self._db.execute('''UPDATE verification_requests SET job_id=?,state=
                    CASE WHEN state IN ('ObservationMismatch','Failed','Cancelled','TimedOut','Rejected')
                    THEN state ELSE 'Queued' END, approval_digest=? WHERE key_digest=?''',
                    (job_id, approved.package.digest, key_digest))
            return self._observe(plan, step_index, self._row(key_digest))
