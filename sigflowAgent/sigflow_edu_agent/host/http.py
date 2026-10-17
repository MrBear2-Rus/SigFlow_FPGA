"""Strict loopback HTTP transport for the private u2.host.v1 extension."""

from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn
import hmac
import json
import math
import re
import threading
from urllib.parse import parse_qs, urlsplit
import uuid

PROTOCOL = 'edu.api.v1'
MAX_BODY = 64 * 1024


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError('invalid_json')
        result[key] = value
    return result


def _constant(value):
    raise ValueError('invalid_json')


def _valid_strings(value, depth=0):
    if depth > 24:
        raise ValueError('invalid_json')
    if type(value) is str:
        value.encode('utf-8', errors='strict')
    elif type(value) is float and not math.isfinite(value):
        raise ValueError('invalid_json')
    elif type(value) is dict:
        for key, item in value.items():
            _valid_strings(key, depth + 1)
            _valid_strings(item, depth + 1)
    elif type(value) is list:
        for item in value:
            _valid_strings(item, depth + 1)


def strict_json(raw):
    try:
        result = json.loads(raw.decode('utf-8'), object_pairs_hook=_pairs, parse_constant=_constant)
        _valid_strings(result)
    except (ValueError, UnicodeError, RecursionError):
        raise ValueError('invalid_json') from None
    if type(result) is not dict:
        raise ValueError('invalid_json')
    return result


class HTTPFault(ValueError):
    def __init__(self, status, code):
        self.status, self.code = status, code
        super().__init__(code)


_ERROR_STATUS = {
    'prediction_challenge_stale':409,'prediction_unavailable':409,'prediction_budget_exceeded':409,'invalid_prediction':400,
    'scope_denied': 403, 'action_denied': 403, 'receipt_unavailable': 403,
    'stale_state': 409, 'action_conflict': 409, 'project_busy': 409, 'host_busy': 409,
    'operation_pending': 409, 'session_busy': 409, 'recovery_required': 409,
    'recovery_not_available': 409, 'request_mismatch': 409, 'host_closed': 409,
    'not_found': 404, 'session_not_found': 404, 'cursor_stale': 410, 'cursor_expired': 410,
    'store_unavailable': 503, 'store_closed': 503, 'session_limit': 409, 'action_limit': 409,
    'verification_unavailable': 409, 'approval_denied': 403,
    'state_conflict': 409, 'binding_conflict': 409, 'instance_conflict': 403,
    'storage_error': 503, 'state_corrupt': 503, 'checkpoint_corrupt': 503,
}


class HostServer(ThreadingMixIn, HTTPServer):
    """Owns sockets and a bounded number of concurrent request handlers."""
    daemon_threads = True
    block_on_close = False
    allow_reuse_address = False

    def __init__(self, runtime, ui_token, *, agent_version='0.2.0', model_available=False,
                 http_workers=8, secrets=()):
        if (type(ui_token) is not str or not 1 <= len(ui_token) <= 4096
                or any(ord(c) < 33 or ord(c) > 126 for c in ui_token)):
            raise ValueError('invalid_ui_token')
        if type(http_workers) is not int or not 1 <= http_workers <= 32:
            raise ValueError('invalid_workers')
        self.runtime, self.ui_token = runtime, ui_token
        self.agent_version, self.model_available = agent_version, bool(model_available)
        self.secrets = tuple(s for s in (ui_token, *secrets) if type(s) is str and s)
        self._slots = threading.BoundedSemaphore(http_workers)
        self._stopping = threading.Event()
        super().__init__(('127.0.0.1', 0), Handler)

    def process_request(self, request, client_address):
        if not self._slots.acquire(False):
            # No extra handler thread is created when the limit is reached.
            try:
                request.settimeout(.1)
                token = uuid.uuid4().hex
                raw = json.dumps(dict(schema_version=PROTOCOL, request_id='req-' + token,
                    trace_id='trace-' + token, error=dict(code='host_busy', message='host_busy', retryable=True))).encode('ascii')
                request.sendall(b'HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: '
                                + str(len(raw)).encode() + b'\r\n\r\n' + raw)
            except OSError:
                pass
            finally:
                self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self._slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._slots.release()

    def handle_error(self, request, client_address):
        pass  # stdlib tracebacks can contain input or filesystem paths.

    def stop_async(self):
        if not self._stopping.is_set():
            self._stopping.set()
            threading.Thread(target=self.shutdown, daemon=True).start()

    def model(self):
        return dict(available=self.model_available, provider='deepseek' if self.model_available else None,
                    reason='configured' if self.model_available else 'model_unconfigured_rule_fallback')


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'SigFlowU2Host'
    sys_version = ''

    def setup(self):
        super().setup()
        self.connection.settimeout(3)

    def log_message(self, *args):
        pass

    def send_error(self, code, message=None, explain=None):
        self._reply(code, error='invalid_http')

    def handle_expect_100(self):
        self._reply(417, error='expectation_denied')
        return False

    def _reply(self, status, data=None, error=None):
        token = uuid.uuid4().hex
        envelope = dict(schema_version=PROTOCOL, request_id='req-' + token, trace_id='trace-' + token)
        if error:
            envelope['error'] = dict(code=error, message=error, retryable=status in (409, 503))
        else:
            envelope['data'] = data
        # Model-authored strings can echo tokens or absolute paths; curated
        # runtime state alone does not make those strings trusted.
        def scrub(value, field=None):
            if type(value) is str:
                for secret in self.server.secrets:
                    value = value.replace(secret, '[redacted]')
                # These metadata locators came from admitted CourseReference
                # values. Do not mistake an HTTPS URL's second slash for a local
                # absolute path; free-form hints still receive path filtering.
                if field in ('source_url', 'source_urls'):
                    locator = urlsplit(value)
                    if (locator.scheme in ('https', 'sigflow-note') and not locator.username
                            and not locator.password and not any(c.isspace() for c in value)):
                        return value
                return re.sub(r'(?:[A-Za-z]:[\\/]|\\\\|(?<![\w:])/(?:[^\s/]+/))[^\s"<>]*', '[path]', value)
            if type(value) is dict:
                return {k: scrub(v, k) for k, v in value.items()
                        if k.lower() not in ('checkpoint', 'token', 'api_key', 'grant', 'receipt', 'authorization')}
            if type(value) in (list, tuple):
                return [scrub(v, field) for v in value]
            return value
        raw = json.dumps(scrub(envelope), ensure_ascii=True, allow_nan=False, separators=(',', ':')).encode('ascii')
        self.close_connection = True
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(raw)))
        self.send_header('Connection', 'close')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        try:
            self.wfile.write(raw)
            self.wfile.flush()
        except OSError:
            pass

    def _one(self, name, required=False):
        values = self.headers.get_all(name, [])
        if len(values) > 1 or (required and len(values) != 1):
            raise HTTPFault(400, 'invalid_headers')
        return values[0] if values else None

    def _boundary(self):
        hosts = self.headers.get_all('Host', [])
        host = f'127.0.0.1:{self.server.server_port}'
        if hosts != [host]:
            raise HTTPFault(403, 'host_denied')
        origins = self.headers.get_all('Origin', [])
        if origins and origins != ['http://' + host]:
            raise HTTPFault(403, 'origin_denied')
        if self.headers.get_all('Transfer-Encoding') or self.headers.get_all('Content-Encoding'):
            raise HTTPFault(400, 'encoding_denied')
        length = self._one('Content-Length')
        if length is not None and re.fullmatch(r'(?:0|[1-9][0-9]{0,8})', length) is None:
            raise HTTPFault(400, 'invalid_length')
        if length is not None and int(length) > MAX_BODY:
            raise HTTPFault(413, 'request_too_large')
        if self.command == 'GET' and length not in (None, '0'):
            raise HTTPFault(400, 'body_denied')
        if len(self.path) > 2048 or '%' in self.path or '#' in self.path or '\\' in self.path:
            raise HTTPFault(400, 'invalid_route')
        parsed = urlsplit(self.path)
        if parsed.scheme or parsed.netloc or not parsed.path.startswith('/'):
            raise HTTPFault(400, 'invalid_route')
        return parsed

    def _body(self):
        length = self._one('Content-Length')
        if length is None:
            raise HTTPFault(411, 'length_required')
        content_type = self._one('Content-Type', required=True)
        if content_type.lower() not in ('application/json', 'application/json; charset=utf-8'):
            raise HTTPFault(415, 'content_type_denied')
        raw = self.rfile.read(int(length))
        if len(raw) != int(length):
            raise HTTPFault(400, 'incomplete_body')
        return strict_json(raw)

    def _auth(self):
        value = self._one('Authorization')
        if value is None or not hmac.compare_digest(value.encode('utf-8'), ('Bearer ' + self.server.ui_token).encode('ascii')):
            raise HTTPFault(401, 'unauthenticated')

    def _learner(self):
        value = self._one('X-Learner-ID', required=True)
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,128}', value):
            raise HTTPFault(400, 'invalid_learner')
        return value

    @staticmethod
    def _version(value):
        if type(value) is not str or re.fullmatch(r'(?:0|[1-9][0-9]{0,18})', value) is None or int(value) >= 2**63:
            raise HTTPFault(400, 'invalid_state_version')
        return int(value)

    def do_GET(self):
        self._dispatch()

    def do_POST(self):
        self._dispatch()

    def _dispatch(self):
        try:
            parsed = self._boundary()
            path, runtime = parsed.path, self.server.runtime
            if self.command == 'GET' and path == '/api/v1/health' and not parsed.query:
                self._reply(200, dict(protocol=PROTOCOL, agent_version=self.server.agent_version,
                                     instance_id=runtime.instance_id, ready=True, model=self.server.model()))
                return
            self._auth()
            if self.command == 'GET' and path == '/api/v1/capabilities' and not parsed.query:
                self._reply(200, dict(protocol=PROTOCOL, agent_version=self.server.agent_version,
                    extensions=['u2.host.v1'], model=self.server.model(),
                    verification=dict(configured=bool(getattr(runtime.factory, 'verification_available', False)), requires_gateway_grant=True),
                    prediction=dict(configured=bool(getattr(runtime.factory,'prediction_available',False)),
                                    requires_explicit_expectation=True,starts_jobs=False),
                    modes=[dict(id=f'L{level}', available=level <= 3, reason='readonly_teaching' if level <= 3 else 'receipt_unavailable') for level in range(1, 5)],
                    capabilities=[dict(name='u2.host.v1', ready=True, reason='readonly_teaching', plugin_version=self.server.agent_version)],
                    course=dict(available=bool(getattr(runtime.factory, 'course_enabled', False)),
                                reason='configured' if getattr(runtime.factory, 'course_enabled', False) else 'disabled'),
                    execution=dict(can_run_eda=False, note='Verification requires an explicit UI plan and scoped Gateway grant.')))
                return
            if self.command == 'POST' and path == '/api/v1/shutdown' and not parsed.query:
                if self._body() != {}:
                    raise HTTPFault(400, 'unknown_fields')
                self._reply(202, dict(accepted=True))
                self.server.stop_async()
                return
            learner = self._learner()
            if self.command == 'POST' and path == '/api/v1/teaching/sessions' and not parsed.query:
                body = self._body()
                if body.get('learner_id') != learner:
                    raise HTTPFault(403, 'scope_denied')
                key = self._one('Idempotency-Key', required=True)
                self._reply(202, runtime.create(body, key))
                return
            match = re.fullmatch(r'/api/v1/teaching/sessions/([A-Za-z0-9_-]{1,128})/(state|events|actions|cancel)', path)
            if not match:
                raise HTTPFault(404, 'not_found')
            sid, route = match.groups()
            if self.command == 'GET' and route == 'state' and not parsed.query:
                self._reply(200, runtime.snapshot(sid, learner))
            elif self.command == 'GET' and route == 'events':
                params = parse_qs(parsed.query, keep_blank_values=True, strict_parsing=True)
                if set(params) - {'after', 'limit'} or any(len(v) != 1 for v in params.values()):
                    raise HTTPFault(400, 'invalid_cursor')
                after = self._version(params.get('after', ['0'])[0])
                limit = self._version(params.get('limit', ['100'])[0])
                if not 1 <= limit <= 100:
                    raise HTTPFault(400, 'invalid_cursor')
                events = runtime.events(sid, learner, after, limit)
                if after > events['high_watermark']:
                    raise HTTPFault(410, 'cursor_stale')
                self._reply(200, events)
            elif self.command == 'POST' and route == 'actions' and not parsed.query:
                body = self._body()
                required = {'kind', 'action_id', 'expected_version'}
                prediction_kind=body.get('kind') in ('prediction_request','prediction_submit')
                extra = {'prediction'} if prediction_kind else {'question'} if body.get('kind') == 'recover' else {'grant_ref'} if body.get('kind') == 'execute_plan' else set()
                if (not required <= set(body) or set(body) - required - extra
                        or (body.get('kind') == 'recover' and 'question' not in body)
                        or type(body.get('kind')) is not str or type(body.get('action_id')) is not str
                        or ('grant_ref' in body and type(body['grant_ref']) is not str)):
                    raise HTTPFault(400, 'unknown_fields')
                version = self._version(body['expected_version'])
                if body['kind'] == 'recover':
                    self._reply(202, runtime.recover(sid, learner, body['action_id'], version, body['question']))
                elif prediction_kind:
                    self._reply(202,runtime.action(sid,learner,body['kind'],body['action_id'],version,
                                                  prediction=body.get('prediction')))
                elif body['kind'] == 'execute_plan':
                    self._reply(202, runtime.action(sid, learner, body['kind'], body['action_id'], version,
                                                    grant_ref=body.get('grant_ref', '')))
                else:
                    self._reply(202, runtime.action(sid, learner, body['kind'], body['action_id'], version))
            elif self.command == 'POST' and route == 'cancel' and not parsed.query:
                if self._body() != {}:
                    raise HTTPFault(400, 'unknown_fields')
                self._reply(202, runtime.cancel(sid, learner))
            else:
                raise HTTPFault(404, 'not_found')
        except HTTPFault as exc:
            self._reply(exc.status, error=exc.code)
        except ValueError as exc:
            code = str(exc)
            status = _ERROR_STATUS.get(code, 400)
            if code not in _ERROR_STATUS and code not in {'invalid_json', 'invalid_request', 'invalid_identifier', 'invalid_session_binding', 'invalid_input', 'invalid_state_version', 'unsupported_request'}:
                code = 'invalid_request'
            self._reply(status, error=code)
        except (TimeoutError, OSError):
            self._reply(503, error='service_unavailable')
        except Exception:
            self._reply(500, error='host_operation_failed')
