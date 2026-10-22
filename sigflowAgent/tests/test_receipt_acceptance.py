"""CLI contract tests; actual Core receipt execution is a separate explicit run."""
from contextlib import redirect_stdout
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from sigflow_edu_agent.host import receipt_acceptance


class ReceiptAcceptanceTests(unittest.TestCase):
    def test_durable_probe_is_explicit_and_forwards_only_when_enabled(self):
        with tempfile.TemporaryDirectory() as owned:
            root = Path(owned)
            gateway = root / 'gateway.exe'
            gateway.touch()
            for extra, expected in (([], {}), (['--durable'], {'durable': True})):
                with self.subTest(extra=extra), patch.object(receipt_acceptance, 'exercise',
                        return_value=dict(status='passed')) as execute, redirect_stdout(io.StringIO()):
                    code = receipt_acceptance.main(['--gateway', str(gateway), '--output',
                                                    str(root / 'report.json'), *extra])
                self.assertEqual(code, 0)
                self.assertEqual(execute.call_args.kwargs, expected)
