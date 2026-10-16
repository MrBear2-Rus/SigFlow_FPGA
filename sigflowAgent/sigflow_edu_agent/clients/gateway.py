"""Loopback-only client for the currently implemented Gateway v1 routes."""

from dataclasses import dataclass, field
import http.client
import json
import math
import re

PROTOCOL = 'edu.api.v1'
EDUCATIONAL_CAPABILITIES = ('eda.synth', 'eda.sim.build', 'eda.sim.run')


def _json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate_key')
        result[key] = value
    return result


def _json_constant(value):
    raise ValueError('invalid_number')


class GatewayError(ValueError):
    CODES = {'INVALID_RESPONSE', 'UNAUTHENTICATED', 'POLICY_DENIED', 'NOT_FOUND',
             'PROTOCOL_MISMATCH', 'INSTANCE_MISMATCH', 'RESOURCE_EXHAUSTED',
             'SERVICE_UNAVAILABLE', 'TIMEOUT', 'REDIRECT_DENIED', 'INVALID_ARGUMENT',
             'STALE_REVISION', 'IDEMPOTENCY_CONFLICT', 'CURSOR_EXPIRED', 'ARTIFACT_EXPIRED',
             'CAPABILITY_UNAVAILABLE', 'UNSUPPORTED_MAPPING', 'RECOVERY_REQUIRED'}

    def __init__(self, code):
        self.code = code if code in self.CODES else 'INVALID_RESPONSE'
        super().__init__(self.code)


@dataclass(frozen=True)
class GatewayConfig:
    port: int
    token: str = field(repr=False)
    instance_id: str
    timeout: float = 3.0

    def __post_init__(self):
        if type(self.port) is not int or not 1 <= self.port <= 65535:
            raise ValueError('invalid_gateway_port')
        if (type(self.token) is not str or not 1 <= len(self.token) <= 4096
                or any(ord(c) < 33 or ord(c) > 126 for c in self.token)):
            raise ValueError('invalid_gateway_token')
        if type(self.instance_id) is not str or not self.instance_id.strip() or len(self.instance_id) > 256:
            raise ValueError('invalid_gateway_instance')
        if type(self.timeout) not in (int, float) or not math.isfinite(self.timeout) or not 0 < self.timeout <= 30:
            raise ValueError('invalid_gateway_timeout')


@dataclass(frozen=True)
class Capability:
    id: str
    ready: bool
    disabled: bool
    plugin_id: str | None
    plugin_version: str | None
    reason: str | None


@dataclass(frozen=True)
class Capabilities:
    instance_id: str
    items: tuple[Capability, ...]

    @property
    def available_ids(self):
        """Availability only; this never grants permission to execute a Job."""
        return tuple(c.id for c in self.items if c.id in EDUCATIONAL_CAPABILITIES
                     and c.ready and not c.disabled)


class GatewayClient:
    MAX_RESPONSE_BYTES = 1048576

    def __init__(self, config: GatewayConfig):
        self.config = config

    @staticmethod
    def _identifier(value):
        if (type(value) is not str or len(value) > 256
                or re.fullmatch(r'[A-Za-z0-9_-]+(?:[.:][A-Za-z0-9_-]+)*', value) is None):
            raise GatewayError('INVALID_ARGUMENT')
        return value

    def _get(self, route, *, job_id=None, artifact_id=None):
        # Only these route factories can reach the transport; no caller URL,
        # query string, redirect, or alternate HTTP method is accepted.
        if route in ('health', 'capabilities') and job_id is None and artifact_id is None:
            path = '/api/v1/' + route
        elif route == 'job_report':
            path = '/api/v1/jobs/' + self._identifier(job_id) + '/report'
        elif route == 'artifact_metadata' and job_id is None:
            path = '/api/v1/artifacts/' + self._identifier(artifact_id)
        else:
            raise GatewayError('UNSUPPORTED_MAPPING')
        data = self._exchange('GET', path, authenticated=route != 'health')
        if route in ('job_report', 'artifact_metadata'):
            return data
        if data.get('protocol_version') != PROTOCOL:
            raise GatewayError('PROTOCOL_MISMATCH')
        if data.get('instance_id') != self.config.instance_id:
            raise GatewayError('INSTANCE_MISMATCH')
        if data.get('edition') != 'edu':
            raise GatewayError('POLICY_DENIED')
        return data

    def _exchange(self, method, path, body=None, *, authenticated=True, max_bytes=None):
        """Internal loopback transport; public methods own route construction."""
        if method not in ('GET', 'POST') or not path.startswith('/api/v1/') or any(c in path for c in '\r\n'):
            raise GatewayError('UNSUPPORTED_MAPPING')
        limit = self.MAX_RESPONSE_BYTES if max_bytes is None else max_bytes
        connection = http.client.HTTPConnection('127.0.0.1', self.config.port, timeout=self.config.timeout)
        headers = {'Accept': 'application/json'}
        if authenticated:
            headers['Authorization'] = 'Bearer ' + self.config.token
        if body is not None:
            headers['Content-Type'] = 'application/json'
        try:
            if body is None:
                connection.request(method,path,headers=headers)
            else:
                connection.request(method,path,body=json.dumps(body,allow_nan=False).encode('utf-8'),headers=headers)
            response = connection.getresponse()
            if 300 <= response.status < 400:
                raise GatewayError('REDIRECT_DENIED')
            raw = response.read(limit + 1)
            if len(raw) > limit:
                raise GatewayError('RESOURCE_EXHAUSTED')
            status = response.status
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
        if not isinstance(envelope, dict):
            raise GatewayError('INVALID_RESPONSE')
        if envelope.get('schema_version') != PROTOCOL:
            raise GatewayError('PROTOCOL_MISMATCH')
        if not all(type(envelope.get(k)) is str and envelope[k] for k in ('request_id', 'trace_id')):
            raise GatewayError('INVALID_RESPONSE')
        if status != 200:
            error = envelope.get('error')
            if (not isinstance(error, dict) or type(error.get('code')) is not str
                    or type(error.get('message')) is not str or type(error.get('retryable')) is not bool):
                raise GatewayError('INVALID_RESPONSE')
            raise GatewayError(error['code'])
        data = envelope.get('data')
        if 'error' in envelope or not isinstance(data, dict):
            raise GatewayError('INVALID_RESPONSE')
        return data

    def health(self):
        return self._get('health')

    def artifact_metadata(self, artifact_id, *, project_id, revision, job_id, sha256):
        """Read metadata only, checking exact ownership and the report's hash.

        Snapshot binding belongs to the report: this route exposes no snapshot.
        Core Describe rehashes the artifact and rejects expired/modified files.
        """
        for value in (artifact_id, project_id, revision, job_id):
            self._identifier(value)
        if type(sha256) is not str or re.fullmatch(r'(?:sha256:)?[a-fA-F0-9]{64}', sha256) is None:
            raise GatewayError('INVALID_ARGUMENT')
        self.health()
        data = self._get('artifact_metadata', artifact_id=artifact_id)
        if tuple(data.get(k) for k in ('artifact_id', 'project_id', 'revision', 'job_id', 'sha256')) != (
                artifact_id, project_id, revision, job_id, sha256):
            raise GatewayError('INVALID_RESPONSE')
        if (any(type(data.get(k)) is not str or len(data[k]) > 4096
                for k in ('schema', 'role', 'media_type'))
                or type(data.get('size')) is not str
                or re.fullmatch(r'0|[1-9][0-9]{0,19}', data['size']) is None
                or int(data['size']) > 18446744073709551615
                or type(data.get('inline_readable')) is not bool):
            raise GatewayError('INVALID_RESPONSE')
        return data

    def job_report(self, job_id, *, project_id, revision, snapshot_id):
        """Fetch a bound raw report; this does not certify evidence or anchors."""
        for value in (job_id, project_id, revision, snapshot_id):
            self._identifier(value)
        self.health()
        data = self._get('job_report', job_id=job_id)
        if data.get('schema_version') != 'edu.jobreport.v1':
            raise GatewayError('PROTOCOL_MISMATCH')
        if data.get('job_id') != job_id or data.get('project_id') != project_id:
            raise GatewayError('INVALID_RESPONSE')
        if data.get('revision') != revision or data.get('snapshot_id') != snapshot_id:
            raise GatewayError('STALE_REVISION')
        if (data.get('origin') != 'core'
                or data.get('completeness') not in ('complete', 'partial', 'unavailable')
                or data.get('state') not in ('Queued', 'Running', 'Cancelling', 'Cancelled',
                                           'Succeeded', 'Failed', 'TimedOut', 'Rejected', 'Unknown')
                or type(data.get('capability')) is not str or not data['capability']):
            raise GatewayError('INVALID_RESPONSE')
        for key, expected in (('diagnostics', list), ('metrics', dict), ('artifacts', list)):
            if key in data and type(data[key]) is not expected:
                raise GatewayError('INVALID_RESPONSE')
        for key in ('plugin_id', 'plugin_version', 'input_fingerprint', 'raw_report_schema', 'reason'):
            if data.get(key) is not None and type(data[key]) is not str:
                raise GatewayError('INVALID_RESPONSE')
        if data.get('exit_code') is not None and type(data['exit_code']) is not int:
            raise GatewayError('INVALID_RESPONSE')
        return data

    def capabilities(self):
        data = self._get('capabilities')
        entries = data.get('capabilities')
        if not isinstance(entries, list) or len(entries) > 256:
            raise GatewayError('INVALID_RESPONSE')
        seen, items = set(), []
        for entry in entries:
            if not isinstance(entry, dict):
                raise GatewayError('INVALID_RESPONSE')
            name = entry.get('id')
            if type(name) is not str or not name or name in seen:
                raise GatewayError('INVALID_RESPONSE')
            if any(type(entry.get(key)) is not bool for key in ('ready', 'disabled')):
                raise GatewayError('INVALID_RESPONSE')
            for key in ('plugin_id', 'plugin_version', 'reason'):
                if entry.get(key) is not None and type(entry[key]) is not str:
                    raise GatewayError('INVALID_RESPONSE')
            if entry.get('input_schema') is not None and not isinstance(entry['input_schema'], dict):
                raise GatewayError('INVALID_RESPONSE')
            seen.add(name)
            items.append(Capability(name, entry['ready'], entry['disabled'],
                                    entry.get('plugin_id'), entry.get('plugin_version'), entry.get('reason')))
        return Capabilities(self.config.instance_id, tuple(items))
