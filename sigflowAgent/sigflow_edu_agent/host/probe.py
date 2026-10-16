"""Bounded, rule-only teaching probe for an already running trusted U2 host."""

import argparse
import http.client
import json
import math
import os
from pathlib import Path
import sys
import time

from .http import MAX_BODY, PROTOCOL, strict_json
from .runtime import identifier


def _request(port, method, path, *, token=None, learner=None, key=None, body=None, timeout=3):
    headers = {'Accept': 'application/json'}
    if token is not None:
        headers['Authorization'] = 'Bearer ' + token
    if learner is not None:
        headers['X-Learner-ID'] = learner
    if key is not None:
        headers['Idempotency-Key'] = key
    raw = None
    if body is not None:
        raw = json.dumps(body, ensure_ascii=True).encode('ascii')
        if len(raw) > MAX_BODY:
            raise ValueError('request_too_large')
        headers['Content-Type'] = 'application/json'
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=timeout)
    try:
        connection.request(method, path, raw, headers)
        response = connection.getresponse()
        payload = response.read(256 * 1024 + 1)
        if len(payload) > 256 * 1024:
            raise ValueError('invalid_host_response')
        envelope = strict_json(payload)
        if envelope.get('schema_version') != PROTOCOL or type(envelope.get('data')) is not dict:
            raise ValueError('host_request_failed')
        if response.status != (202 if method == 'POST' else 200):
            raise ValueError('host_request_failed')
        return envelope['data']
    finally:
        connection.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--instance-id', required=True)
    parser.add_argument('--request', type=Path, required=True)
    parser.add_argument('--key', required=True, help='Stable create idempotency key; reuse only for the same request')
    parser.add_argument('--wait', type=float, default=10, help='Maximum state polling seconds (0.1–60)')
    args = parser.parse_args(argv)
    try:
        identifier(args.instance_id)
        identifier(args.key)
        if not 1 <= args.port <= 65535 or not math.isfinite(args.wait) or not .1 <= args.wait <= 60:
            raise ValueError('invalid_probe_config')
        token = os.environ.get('SIGFLOW_U2_UI_TOKEN', '')
        if not 1 <= len(token) <= 4096 or any(ord(c) < 33 or ord(c) > 126 for c in token):
            raise ValueError('ui_token_required')
        with args.request.open('rb') as stream:
            raw = stream.read(MAX_BODY + 1)
        if len(raw) > MAX_BODY:
            raise ValueError('request_too_large')
        body = strict_json(raw)
        if body.get('intent', 'diagnose') not in ('diagnose', 'explain') or body.get('mode', 'rule') != 'rule':
            raise ValueError('readonly_probe_required')
        body = body | {'mode': 'rule'}
        learner = identifier(body.get('learner_id'))
        # No credentials are sent until the expected local instance responds.
        health = _request(args.port, 'GET', '/api/v1/health')
        if (health.get('protocol') != PROTOCOL or health.get('instance_id') != args.instance_id
                or health.get('ready') is not True):
            raise ValueError('host_binding_mismatch')
        created = _request(args.port, 'POST', '/api/v1/teaching/sessions', token=token,
                           learner=learner, key=args.key, body=body)
        sid = identifier(created.get('session_id'))
        if created.get('accepted') is not True:
            raise ValueError('invalid_host_response')
        deadline = time.monotonic() + args.wait
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                print(json.dumps({'status': 'Timeout'}))
                return 5
            state = _request(args.port, 'GET', f'/api/v1/teaching/sessions/{sid}/state',
                             token=token, learner=learner, timeout=min(3, remaining))
            expected = dict(session_id=sid, instance_id=args.instance_id, learner_id=learner,
                            **{k: body.get(k) for k in ('project_id', 'revision', 'snapshot_id', 'job_id', 'issue_id')})
            if any(state.get(k) != v for k, v in expected.items()):
                raise ValueError('host_binding_mismatch')
            status = state.get('status')
            if status != 'Running':
                if status not in ('WaitingStudent', 'Completed', 'WaitingEvidence', 'RecoveryRequired',
                                  'Failed', 'BudgetExceeded', 'Cancelled'):
                    raise ValueError('invalid_host_response')
                cards = state.get('cards')
                if type(cards) is not list or state.get('evidence_origin') not in ('fixture', 'tool'):
                    raise ValueError('invalid_host_response')
                print(json.dumps(dict(status=status, state_version=state.get('state_version'),
                                      card_count=len(cards), evidence_origin=state['evidence_origin'])))
                return 0 if status in ('WaitingStudent', 'Completed') else 3 if status == 'WaitingEvidence' else 4
            time.sleep(min(.05, max(0, deadline - time.monotonic())))
    except (OSError, http.client.HTTPException):
        print('probe_transport_unavailable', file=sys.stderr)
        return 2
    except ValueError as error:
        allowed = {'invalid_identifier', 'invalid_probe_config', 'ui_token_required', 'request_too_large',
                   'invalid_json', 'readonly_probe_required', 'host_binding_mismatch', 'host_request_failed',
                   'invalid_host_response'}
        print(str(error) if str(error) in allowed else 'invalid_probe_request', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
