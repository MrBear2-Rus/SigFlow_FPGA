import json
import os
from pathlib import Path
import subprocess
import sys
import unittest


class CliTests(unittest.TestCase):
    def run_cli(self, *args, text=''):
        env = {k: v for k, v in os.environ.items() if not k.startswith('DEEPSEEK_')}
        env['PYTHONUTF8'] = '1'
        return subprocess.run([sys.executable, '-m', 'sigflow_edu_agent', *args],
                              input=text, capture_output=True, encoding='utf-8',
                              cwd=Path(__file__).resolve().parents[1], env=env, timeout=10)

    def test_demo_runs_without_network_or_key_and_reports_all_scenarios(self):
        result = self.run_cli('--demo')
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data['evidence_origin'], 'fixture')
        self.assertEqual(data['explain']['status'], 'Completed')
        self.assertEqual(data['diagnose']['level'], 3)
        self.assertEqual(data['unapproved']['status'], 'WaitingApproval')
        self.assertEqual(data['failed_validation']['status'], 'Failed')
        self.assertEqual(data['successful_validation']['status'], 'Completed')

    def test_deepseek_without_configuration_exits_explicitly(self):
        result = self.run_cli('--provider', 'deepseek', '--once')
        self.assertEqual(result.returncode, 2)
        self.assertIn('model_unconfigured', result.stderr)
        self.assertNotIn('Traceback', result.stderr)

    def test_interactive_rule_loop_accepts_hint_then_finish(self):
        result = self.run_cli('--provider', 'rule', text='next\nfinish\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('L2', result.stdout)
        self.assertIn('Completed', result.stdout)


if __name__ == '__main__':
    unittest.main()
