from dataclasses import asdict, replace
import importlib
import http.client
import json
import os
from pathlib import Path
from queue import Queue
import subprocess
import sys
import tempfile
from threading import Thread
import time
import unittest

from sigflow_edu_agent.clients.gateway import GatewayConfig
from sigflow_edu_agent.context.models import SourceRef
from sigflow_edu_agent.domain import EvidenceError, RunRequest, Status
from sigflow_edu_agent.host.factory import load_anchors, make_factory, make_report_resolver
from sigflow_edu_agent.runtime.gateway_session import SessionBinding
import test_gateway_source
from _model_isolation import isolate_model_settings


class CatalogFixture:
    def setUp(self):
        isolate_model_settings(self)
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'anchors.json'
        self.report, self.anchor = test_gateway_source.fixture()

    def catalog(self):
        # Missing public API is an assertion failure before importing it.
        specification = importlib.util.find_spec('sigflow_edu_agent.host.catalog')
        self.assertIsNotNone(specification, 'dynamic AnchorCatalog module missing')
        module = importlib.import_module('sigflow_edu_agent.host.catalog')
        self.assertTrue(hasattr(module, 'AnchorCatalog'), 'dynamic AnchorCatalog API missing')
        return module.AnchorCatalog(self.path)

    def write(self, *anchors):
        self.path.write_text(json.dumps({'anchors': [asdict(a) for a in anchors]}), encoding='utf-8')


class AnchorCatalogTests(CatalogFixture, unittest.TestCase):
    def test_snapshot_reads_updated_catalog_without_mutating_old_snapshot(self):
        self.write()
        catalog = self.catalog()
        empty = catalog.snapshot()
        self.write(self.anchor)
        self.assertEqual(catalog.snapshot(), (self.anchor,))
        self.assertEqual(empty, ())
        self.write()
        self.assertEqual(catalog.snapshot(), ())

    def test_missing_catalog_never_returns_previous_snapshot(self):
        self.write(self.anchor)
        catalog = self.catalog()
        self.assertEqual(catalog.snapshot(), (self.anchor,))
        self.path.rename(self.path.with_suffix('.withdrawn'))
        with self.assertRaisesRegex(ValueError, 'invalid_anchor_catalog'):
            catalog.snapshot()

    def test_corruption_expiration_and_duplicate_anchor_invalidate_entire_catalog(self):
        self.write(self.anchor)
        catalog = self.catalog()
        self.assertEqual(catalog.snapshot(), (self.anchor,))
        for raw in ('{', '{"anchors":[],"anchors":[]}', '{"anchors":NaN}',
                    json.dumps({'anchors': [asdict(replace(self.anchor, expired=True))]}),
                    json.dumps({'anchors': [asdict(self.anchor), asdict(self.anchor)]})):
            with self.subTest(raw=raw):
                self.path.write_text(raw, encoding='utf-8')
                with self.assertRaises(ValueError):
                    catalog.snapshot()

    def test_byte_and_count_limits_reject_whole_catalog(self):
        catalog = self.catalog()
        self.path.write_bytes(b' ' * 65537)
        with self.assertRaises(ValueError):
            catalog.snapshot()
        self.write(*(replace(self.anchor, evidence_id=f'ev-{n}') for n in range(257)))
        with self.assertRaises(ValueError):
            catalog.snapshot()

    def test_hostile_identifiers_hashes_ranges_and_shapes_remain_rejected(self):
        catalog = self.catalog()
        ref = SourceRef('prj-1', 'rev-12', 'source-1', 'b' * 64, 2, 3)
        valid = asdict(self.anchor)
        variants = [dict(valid, unexpected='field'), dict(valid, evidence_id='../evidence'),
                    dict(valid, artifact_id='C:/private/report'), dict(valid, artifact_hash='unhashed'),
                    dict(valid, expired=1), dict(valid, source_ref={'extra': 'field'}),
                    dict(valid, source_ref=asdict(replace(ref, project_id='other'))),
                    dict(valid, source_ref=asdict(replace(ref, revision='other'))),
                    dict(valid, source_ref=asdict(replace(ref, start_line=True))),
                    dict(valid, source_ref=asdict(replace(ref, end_line=1))),
                    dict(valid, source_ref=asdict(replace(ref, source_id='/etc/key'))),
                    dict(valid, source_ref=asdict(replace(ref, start_line=3, start_column=4, end_column=2)))]
        for item in variants:
            with self.subTest(item=item):
                self.path.write_text(json.dumps({'anchors': [item]}), encoding='utf-8')
                with self.assertRaises(ValueError):
                    catalog.snapshot()
        for raw in (b'[]', b'{"anchors":{}}', b'{"anchors":[],"extra":1}', b'\xff'):
            self.path.write_bytes(raw)
            with self.assertRaises(ValueError):
                catalog.snapshot()

    def test_legacy_loader_preserves_static_tuple_and_source_reference(self):
        ref = SourceRef('prj-1', 'rev-12', 'source-1', 'b' * 64, 2, 3)
        self.write(replace(self.anchor, source_ref=ref))
        self.assertEqual(load_anchors(self.path), (replace(self.anchor, source_ref=ref),))
        self.assertEqual(load_anchors(None), ())


class LiveCatalogFactoryTests(CatalogFixture, unittest.TestCase):
    def setUp(self):
        super().setUp()
        self.gateway = test_gateway_source.GatewaySourceHTTPTests()
        self.gateway.setUp()
        self.addCleanup(self.gateway.doCleanups)
        self.config = GatewayConfig(self.gateway.server.server_port, 'fixture-token', 'inst-1')
        self.binding = SessionBinding('inst-1', 'session-1', 'learner-1', 'prj-1', 'rev-12',
                                      'snap-12', 'job-21', 'diag-1', 'issue-1')
        self.request = RunRequest('prj-1', 'rev-12', 'issue-1', '解释观察')
        self.scope = dict(project_id='prj-1', revision='rev-12', snapshot_id='snap-12')

    def session(self, anchors):
        factory = make_factory(self.config, anchors=anchors, evidence_origin='fixture')
        return factory(self.binding, self.request, 'rule', lambda: None)

    def pause(self, session):
        for _ in range(12):
            if session.state.status != Status.RUNNING:
                return session.state
            session.advance(session.state.version)
        self.fail('session did not pause')

    def test_same_session_source_refreshes_when_catalog_gains_anchor(self):
        self.write()
        session = self.session(self.catalog())
        state = self.pause(session)
        self.assertEqual(state.status, Status.WAITING_EVIDENCE)
        self.assertEqual(state.cards, [])
        self.write(self.anchor)
        session.host_action('refresh_evidence', 'refresh-1', state.version)
        state = self.pause(session)
        self.assertEqual(state.status, Status.WAITING_STUDENT)
        self.assertEqual(state.cards[0].evidence_ids, ('ev-1',))

    def test_existing_source_rejects_withdrawn_corrupt_missing_or_expired_catalog(self):
        self.write(self.anchor)
        session = self.session(self.catalog())
        source = session._observed_source
        self.assertEqual(source.collect('prj-1', 'rev-12').anchor, self.anchor)
        for raw in ('{"anchors":[]}', '{broken',
                    json.dumps({'anchors': [asdict(replace(self.anchor, expired=True))]})):
            with self.subTest(raw=raw):
                self.path.write_text(raw, encoding='utf-8')
                with self.assertRaises(EvidenceError):
                    source.collect('prj-1', 'rev-12')
        self.path.rename(self.path.with_suffix('.withdrawn'))
        with self.assertRaises(EvidenceError):
            source.collect('prj-1', 'rev-12')

    def test_resolver_observes_new_anchor_then_revocation_and_corruption(self):
        self.write()
        resolve = make_report_resolver(self.config, self.catalog(), evidence_origin='fixture')
        self.assertIsNone(resolve('job-21', **self.scope))
        self.assertEqual(self.gateway.requests, [])
        self.write(self.anchor)
        self.assertEqual(resolve('job-21', **self.scope).anchor, self.anchor)
        self.assertIsNone(resolve('job-21', **(self.scope | {'revision': 'other'})))
        self.write()
        self.assertIsNone(resolve('job-21', **self.scope))
        self.path.write_text('{broken', encoding='utf-8')
        with self.assertRaises(ValueError):
            resolve('job-21', **self.scope)

    def test_static_sequence_is_copied_for_factory_and_resolver(self):
        anchors = [self.anchor]
        session = self.session(anchors)
        resolve = make_report_resolver(self.config, anchors, evidence_origin='fixture')
        anchors.clear()
        self.assertEqual(self.pause(session).status, Status.WAITING_STUDENT)
        self.assertEqual(resolve('job-21', **self.scope).anchor, self.anchor)


class LiveCatalogProcessTests(CatalogFixture, unittest.TestCase):
    def test_cli_overrides_environment_and_keeps_existing_session_catalog_live(self):
        gateway = test_gateway_source.GatewaySourceHTTPTests()
        gateway.setUp()
        self.addCleanup(gateway.doCleanups)
        self.write()
        package = str(Path(__file__).resolve().parents[1])
        environment = os.environ | {'PYTHONPATH': package,
                                    'SIGFLOW_U2_ANCHORS': str(self.path.with_suffix('.missing'))}
        environment.pop('DEEPSEEK_API_KEY', None)
        process = subprocess.Popen([sys.executable, '-m', 'sigflow_edu_agent.host', '--bootstrap-stdin',
                                    '--anchors', str(self.path), '--evidence-origin', 'fixture'],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   cwd=package, env=environment)
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        bootstrap = dict(type='sigflow-bootstrap', protocol='edu.api.v1', instance_id='inst-1',
                         nonce='a' * 64, gateway_url=f'http://127.0.0.1:{gateway.server.server_port}',
                         gateway_token='fixture-token', ui_token='ui-private', data_directory=self.directory.name)
        process.stdin.write(json.dumps(bootstrap).encode() + b'\n')
        process.stdin.flush()
        ready = Queue()
        Thread(target=lambda: ready.put(process.stdout.readline()), daemon=True).start()
        raw = ready.get(timeout=5)
        self.assertTrue(raw, 'explicit CLI catalog must take precedence over invalid environment catalog')
        port = json.loads(raw)['port']

        def request(method, path, body=None, **headers):
            conn = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
            defaults = {'Authorization': 'Bearer ui-private', 'X-Learner-ID': 'learner-1'}
            if body is not None:
                defaults['Content-Type'] = 'application/json'
                body = json.dumps(body).encode()
            try:
                conn.request(method, path, body, defaults | headers)
                response = conn.getresponse()
                return response.status, json.loads(response.read())
            finally:
                conn.close()

        def wait(sid, status):
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                code, result = request('GET', f'/api/v1/teaching/sessions/{sid}/state')
                self.assertEqual(code, 200, result)
                if result['data']['status'] == status:
                    return result['data']
                time.sleep(.01)
            self.fail(f'live CLI session did not reach {status}: {result["data"]["status"]}')

        try:
            body = dict(learner_id='learner-1', project_id='prj-1', revision='rev-12', snapshot_id='snap-12',
                        job_id='job-21', diagnostic_id='diag-1', issue_id='issue-1', question='解释观察', mode='rule')
            code, result = request('POST', '/api/v1/teaching/sessions', body, **{'Idempotency-Key': 'create-1'})
            self.assertEqual(code, 202, result)
            sid = result['data']['session_id']
            waiting = wait(sid, 'WaitingEvidence')
            self.write(self.anchor)
            code, result = request('POST', f'/api/v1/teaching/sessions/{sid}/actions',
                                   dict(kind='refresh_evidence', action_id='refresh-1',
                                        expected_version=waiting['state_version']))
            self.assertEqual(code, 202, result)
            observed = wait(sid, 'WaitingStudent')
            self.assertEqual(observed['cards'][0]['evidence_ids'], ['ev-1'])
            self.assertEqual(observed['model_calls'], 0)
        finally:
            request('POST', '/api/v1/shutdown', {})
            output, errors = process.communicate(timeout=6)
            self.assertEqual(process.returncode, 0, errors)
            self.assertEqual(output, b'')


if __name__ == '__main__':
    unittest.main()
