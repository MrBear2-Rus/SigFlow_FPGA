"""Host probe contract; real Core execution remains an explicit CLI run."""
from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from sigflow_edu_agent.host import receipt_acceptance
from sigflow_edu_agent.host.reference import ReviewedReference
from sigflow_edu_agent.runtime.gateway_session import SessionBinding


class ReferenceHostAcceptanceTests(unittest.TestCase):
    def test_host_probe_is_opt_in_and_combines_with_durable_without_changing_defaults(self):
        with tempfile.TemporaryDirectory() as owned:
            root = Path(owned)
            gateway = root / 'gateway.exe'
            gateway.touch()
            cases = (([], {}), (['--durable'], dict(durable=True)),
                     (['--host-metadata'], dict(host_metadata=True)),
                     (['--durable', '--host-metadata'], dict(durable=True, host_metadata=True)))
            for extra, expected in cases:
                with self.subTest(extra=extra), patch.object(receipt_acceptance, 'exercise',
                        return_value=dict(status='passed')) as execute, redirect_stdout(io.StringIO()):
                    code = receipt_acceptance.main(['--gateway', str(gateway), '--output',
                                                   str(root / 'report.json'), *extra])
                self.assertEqual(code, 0)
                self.assertEqual(execute.call_args.kwargs, expected)

    def test_host_probe_replays_original_receipt_after_explicit_revalidation_and_redacts_report(self):
        from sigflow_edu_agent.host.reference_host_acceptance import exercise_reference_host
        binding = SessionBinding('probe-inst', 'outer-probe', 'fixture-learner', 'fixture-project',
                                 'fixture-revision', 'fixture-snapshot', 'probe-job', 'probe-diag', 'probe-issue')
        content = 'Synthetic host probe answer body must never persist 813729'
        item = ReviewedReference('fixture-reference', content,
            hashlib.sha256(content.encode()).hexdigest(), ('synthetic-a', 'synthetic-b'))

        class CorePort:
            def __init__(self):
                self.record = None
                self.calls = []
                self.lookups = 0
                self.issuances = 0

            def request(self, method, route, body, status):
                self.issuances += 1
                if (method, route, status) != ('POST', '/project/ui-receipts', 201):
                    raise AssertionError('unexpected UI endpoint')
                if body['ttl_seconds'] != 300 or body['issue'] != binding.issue_id:
                    raise AssertionError('incorrect approval scope')
                self.record = dict(body, receipt_id='ui-receipt-1', project_id=binding.project_id,
                                   status='active')
                return dict(self.record)

            def lookup(self, receipt_id):
                self.lookups += 1
                if receipt_id != self.record['receipt_id']:
                    raise AssertionError('receipt changed')
                return dict(self.record)

            def consume(self, receipt_id, project_id, run_id, action_id, state_version):
                args = (receipt_id, project_id, run_id, action_id, state_version)
                if (receipt_id, project_id, action_id, str(state_version)) != (
                        self.record['receipt_id'], binding.project_id, self.record['action_id'],
                        self.record['state_version']):
                    raise AssertionError('consume scope changed')
                if self.calls and args != self.calls[0]:
                    raise AssertionError('reconcile changed original operation')
                self.calls.append(args)
                replayed = self.record['status'] == 'consumed'
                self.record['status'] = 'consumed'
                return dict(receipt_id=receipt_id, action_id=action_id, consumed=True,
                            status='consumed', replayed=replayed)

        port = CorePort()
        with tempfile.TemporaryDirectory() as owned:
            report = exercise_reference_host(Path(owned), binding, item, port, port, '/project')
            serialized = json.dumps(report)
            self.assertNotIn(owned, serialized)
        self.assertEqual(report['status'], 'passed')
        self.assertEqual((port.issuances, len(port.calls), port.lookups), (1, 2, 2))
        self.assertEqual(report['acknowledged_replays'], [False, True])
        self.assertEqual((report['level'], report['card_count'], report['model_calls']), (3, 3, 0))
        self.assertFalse(report['body_available'])
        self.assertNotIn(content, serialized)
        self.assertNotIn('ui-receipt-1', serialized)


if __name__ == '__main__':
    unittest.main()
