import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

from sigflow_edu_agent.clients import probe
from sigflow_edu_agent.clients.gateway import Capabilities, GatewayError


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.env = dict(SIGFLOW_GATEWAY_PORT='12345', SIGFLOW_GATEWAY_TOKEN='secret-token',
                        SIGFLOW_GATEWAY_INSTANCE='inst-1', SIGFLOW_JOB_ID='job-1',
                        SIGFLOW_PROJECT_ID='p1', SIGFLOW_REVISION='r1', SIGFLOW_SNAPSHOT_ID='snap-1')

    def invoke(self, args):
        out, err = io.StringIO(), io.StringIO()
        with patch.dict(os.environ, self.env, clear=True), contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = probe.main(args)
        return code, out.getvalue(), err.getvalue()

    @patch.object(probe, 'GatewayClient')
    def test_report_summary_excludes_raw_text_and_never_queries_capabilities(self, client):
        client.return_value.config.instance_id = 'inst-1'
        client.return_value.job_report.return_value = dict(
            job_id='job-1', project_id='p1', revision='r1', snapshot_id='snap-1',
            state='Running', completeness='partial', diagnostics=[{'summary': 'secret-token'}],
            artifacts=[{'path': 'private-path'}], reason='private-reason')
        code, out, err = self.invoke(['--report'])
        self.assertEqual((code, err), (0, ''))
        data = json.loads(out)
        self.assertEqual(data['state'], 'Running')
        self.assertEqual(data['diagnostic_count'], 1)
        self.assertEqual(data['artifact_count'], 1)
        self.assertFalse(data['evidence_validated'])
        for secret in ('secret-token', 'private-path', 'private-reason'):
            self.assertNotIn(secret, out)
        client.return_value.job_report.assert_called_once_with(
            job_id='job-1', project_id='p1', revision='r1', snapshot_id='snap-1')
        client.return_value.capabilities.assert_not_called()

    @patch.object(probe, 'GatewayClient')
    def test_each_missing_binding_fails_without_network(self, client):
        for key in ('SIGFLOW_JOB_ID', 'SIGFLOW_PROJECT_ID', 'SIGFLOW_REVISION', 'SIGFLOW_SNAPSHOT_ID'):
            value = self.env.pop(key)
            code, out, err = self.invoke(['--report'])
            self.assertEqual((code, out, err.strip()), (2, '', 'INVALID_ARGUMENT'))
            self.env[key] = value
        client.assert_not_called()

    def test_bad_port_is_fixed_error(self):
        self.env['SIGFLOW_GATEWAY_PORT'] = 'secret-token'
        self.assertEqual(self.invoke(['--report']), (2, '', 'invalid_gateway_port\n'))

    @patch.object(probe, 'GatewayClient')
    def test_gateway_error_is_machine_readable(self, client):
        client.return_value.job_report.side_effect = GatewayError('STALE_REVISION')
        self.assertEqual(self.invoke(['--report']), (2, '', 'STALE_REVISION\n'))

    @patch.object(probe, 'GatewayClient')
    def test_original_mode_preserved(self, client):
        client.return_value.capabilities.return_value = Capabilities('inst-1', ())
        code, out, err = self.invoke([])
        self.assertEqual((code, err), (0, ''))
        self.assertEqual(json.loads(out), {'instance_id': 'inst-1', 'available_ids': []})
        client.return_value.health.assert_called_once_with()
        client.return_value.job_report.assert_not_called()


class ProbeTransportTests(unittest.TestCase):
    def setUp(self):
        from test_gateway_report import GatewayReportTests
        self.fixture = GatewayReportTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

    def invoke(self, **changes):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('SIGFLOW_', 'DEEPSEEK_'))}
        env.update(SIGFLOW_GATEWAY_PORT=str(self.fixture.server.server_port),
                   SIGFLOW_GATEWAY_TOKEN='test-token', SIGFLOW_GATEWAY_INSTANCE='inst-1',
                   SIGFLOW_JOB_ID='job-1', SIGFLOW_PROJECT_ID='p1', SIGFLOW_REVISION='r1',
                   SIGFLOW_SNAPSHOT_ID='snap-1', PYTHONUTF8='1')
        env.update(changes)
        return subprocess.run([sys.executable, '-S', '-m', 'sigflow_edu_agent.clients.probe', '--report'],
                              cwd=Path(__file__).resolve().parents[1], env=env,
                              capture_output=True, encoding='utf-8', timeout=10)

    def test_real_cli_reads_only_health_and_bound_report(self):
        result = self.invoke()
        self.assertEqual((result.returncode, result.stderr), (0, ''))
        data = json.loads(result.stdout)
        self.assertEqual(data['job_id'], 'job-1')
        self.assertFalse(data['evidence_validated'])
        self.assertEqual(self.fixture.paths, ['/api/v1/health', '/api/v1/jobs/job-1/report'])
        self.assertEqual(self.fixture.auth, [None, 'Bearer test-token'])

    def test_invalid_binding_never_connects(self):
        result = self.invoke(SIGFLOW_JOB_ID='../secret')
        self.assertEqual((result.returncode, result.stdout, result.stderr.strip()), (2, '', 'INVALID_ARGUMENT'))
        self.assertEqual(self.fixture.paths, [])

    def test_stale_report_and_server_error_do_not_leak_body(self):
        self.fixture.report['revision'] = 'old'
        result = self.invoke()
        self.assertEqual((result.returncode, result.stdout, result.stderr.strip()), (2, '', 'STALE_REVISION'))
        self.fixture.status = 404
        result = self.invoke()
        self.assertEqual((result.returncode, result.stdout, result.stderr.strip()), (2, '', 'NOT_FOUND'))
        self.assertNotIn('test-token', result.stderr)
