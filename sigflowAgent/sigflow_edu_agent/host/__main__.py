"""One-line bootstrap/ready entry point; secrets remain process-local."""

import argparse
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import threading
from urllib.parse import urlsplit

from ..clients.gateway import GatewayConfig
from .catalog import AnchorCatalog
from .gateway_evidence import GatewayEvidenceCatalog
from .factory import make_factory, make_report_resolver
from .http import HostServer, MAX_BODY, PROTOCOL, strict_json
from .runtime import Runtime, identifier
from .store import DurableStore
from .verification import GatewayVerification


@dataclass(frozen=True)
class Bootstrap:
    instance_id: str
    nonce: str = field(repr=False)
    gateway_port: int
    gateway_token: str = field(repr=False)
    ui_token: str = field(repr=False)
    data_directory: str = field(repr=False)


def parse_bootstrap(raw):
    if type(raw) is not bytes or not raw or len(raw) > MAX_BODY or not raw.endswith(b'\n') or b'\n' in raw[:-1]:
        raise ValueError('invalid_bootstrap')
    body = strict_json(raw)
    required = {'type', 'protocol', 'instance_id', 'nonce', 'gateway_url', 'gateway_token', 'ui_token', 'data_directory'}
    if set(body) != required or any(type(v) is not str or not v or any(ord(c) < 32 for c in v) for v in body.values()):
        raise ValueError('invalid_bootstrap')
    if body['type'] != 'sigflow-bootstrap' or body['protocol'] != PROTOCOL:
        raise ValueError('invalid_bootstrap')
    identifier(body['instance_id'])
    if re.fullmatch(r'[a-fA-F0-9]{64}', body['nonce']) is None:
        raise ValueError('invalid_bootstrap')
    url = urlsplit(body['gateway_url'])
    if (url.scheme != 'http' or url.hostname != '127.0.0.1' or url.username is not None or url.password is not None
            or url.path or url.query or url.fragment or url.port is None
            or body['gateway_url'] != f'http://127.0.0.1:{url.port}'):
        raise ValueError('invalid_bootstrap')
    GatewayConfig(url.port, body['gateway_token'], body['instance_id'])
    if not 1 <= len(body['ui_token']) <= 4096 or any(ord(c) < 33 or ord(c) > 126 for c in body['ui_token']):
        raise ValueError('invalid_bootstrap')
    if not Path(body['data_directory']).is_absolute() or len(body['data_directory']) > 4096:
        raise ValueError('invalid_bootstrap')
    return Bootstrap(body['instance_id'], body['nonce'], url.port, body['gateway_token'], body['ui_token'], body['data_directory'])


def main(argv=None, *, teaching_memory=None):
    """Accept optional trusted in-process composition; never load adapters from input."""
    parser = argparse.ArgumentParser(prog='sigflow-u2-host')
    parser.add_argument('--bootstrap-stdin', action='store_true', required=True)
    evidence = parser.add_mutually_exclusive_group()
    evidence.add_argument('--anchors', type=Path, default=os.environ.get('SIGFLOW_U2_ANCHORS') or None)
    evidence.add_argument('--gateway-evidence', action='store_true')
    parser.add_argument('--evidence-origin', choices=('tool', 'fixture'), default='tool')
    course = parser.add_mutually_exclusive_group()
    course.add_argument('--course-db', type=Path)
    course.add_argument('--no-course', action='store_true')
    args = parser.parse_args(argv)
    if args.gateway_evidence and args.anchors is not None:
        parser.error('gateway_evidence_conflicts_with_anchors')
    if args.gateway_evidence and args.evidence_origin != 'tool':
        parser.error('gateway_evidence_requires_tool_origin')
    store = runtime = server = verification = None
    try:
        bootstrap = parse_bootstrap(sys.stdin.buffer.readline(MAX_BODY + 1))
        anchors = AnchorCatalog(args.anchors) if args.anchors is not None else ()
        if isinstance(anchors, AnchorCatalog):
            anchors.snapshot()
        gateway = GatewayConfig(bootstrap.gateway_port, bootstrap.gateway_token, bootstrap.instance_id)
        if args.gateway_evidence:
            anchors = GatewayEvidenceCatalog(gateway)
        directory = Path(bootstrap.data_directory) / 'u2-host'
        directory.mkdir(parents=True, exist_ok=True)
        filename = hashlib.sha256(bootstrap.instance_id.encode()).hexdigest()[:32] + '.sqlite3'
        store = DurableStore(directory / filename, bootstrap.instance_id)
        verification = GatewayVerification(gateway, directory / (filename[:-8] + '-verification.sqlite3'),
            make_report_resolver(gateway, anchors, evidence_origin=args.evidence_origin))
        from ..courses import SQLiteCourseProvider
        course_provider = None if args.no_course else SQLiteCourseProvider(args.course_db)
        from ..clients.waveform import GatewayWaveformReader
        factory = make_factory(gateway, anchors=anchors, evidence_origin=args.evidence_origin,
                               verification=verification, course_provider=course_provider,
                               waveform_reader=GatewayWaveformReader(gateway) if args.gateway_evidence else None)
        runtime = Runtime(store, bootstrap.instance_id, factory, evidence_origin=args.evidence_origin,
                          teaching_memory=teaching_memory)
        secrets = [bootstrap.gateway_token]
        secrets.extend(factory.model_secrets)
        if os.environ.get('DEEPSEEK_API_KEY'):
            secrets.append(os.environ['DEEPSEEK_API_KEY'])
        server = HostServer(runtime, bootstrap.ui_token, model_available=factory.model_available, secrets=secrets)
        ready = dict(type='ready', protocol=PROTOCOL, nonce=bootstrap.nonce,
                     port=server.server_port, agent_version=server.agent_version)
        sys.stdout.write(json.dumps(ready, separators=(',', ':')) + '\n')
        sys.stdout.flush()
        server.serve_forever(poll_interval=.05)
        return 0
    except KeyboardInterrupt:
        return 0
    except Exception:
        sys.stderr.write('u2-host: initialization_or_runtime_failed\n')
        return 2
    finally:
        if server:
            server.server_close()
        if runtime:
            runtime.close(timeout=2)
        if store:
            store.close()
        if verification:
            # The adapter serializes network work under its lock. A daemon
            # cleanup cannot extend the process exit beyond the runtime bound.
            def close_verification():
                try:
                    verification.close()
                except Exception:
                    pass  # A daemon traceback must not reveal local paths.
            cleanup = threading.Thread(target=close_verification, daemon=True)
            cleanup.start()
            cleanup.join(.1)


if __name__ == '__main__':
    raise SystemExit(main())
