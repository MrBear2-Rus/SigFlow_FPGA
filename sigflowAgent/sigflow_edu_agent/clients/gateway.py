"""Loopback-only client for the currently implemented Gateway v1 routes."""

from dataclasses import dataclass, field
import http.client
import json
import math

PROTOCOL = 'edu.api.v1'
EDUCATIONAL_CAPABILITIES = ('eda.synth', 'eda.sim.build', 'eda.sim.run')


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

    def _get(self, route):
        if route not in ('health', 'capabilities'):
            raise GatewayError('UNSUPPORTED_MAPPING')
        connection = http.client.HTTPConnection('127.0.0.1', self.config.port, timeout=self.config.timeout)
        headers = {'Accept': 'application/json'}
        if route == 'capabilities':
            headers['Authorization'] = 'Bearer ' + self.config.token
        try:
            connection.request('GET', '/api/v1/' + route, headers=headers)
            response = connection.getresponse()
            if 300 <= response.status < 400:
                raise GatewayError('REDIRECT_DENIED')
            raw = response.read(self.MAX_RESPONSE_BYTES + 1)
            if len(raw) > self.MAX_RESPONSE_BYTES:
                raise GatewayError('RESOURCE_EXHAUSTED')
            status = response.status
        except TimeoutError:
            raise GatewayError('TIMEOUT') from None
        except (OSError, http.client.HTTPException):
            raise GatewayError('SERVICE_UNAVAILABLE') from None
        finally:
            connection.close()
        try:
            envelope = json.loads(raw)
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
        if data.get('protocol_version') != PROTOCOL:
            raise GatewayError('PROTOCOL_MISMATCH')
        if data.get('instance_id') != self.config.instance_id:
            raise GatewayError('INSTANCE_MISMATCH')
        if data.get('edition') != 'edu':
            raise GatewayError('POLICY_DENIED')
        return data

    def health(self):
        return self._get('health')

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
