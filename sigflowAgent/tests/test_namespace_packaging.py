"""U1/U2 import isolation and offline wheel delivery; no credentials or APIs."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile


SOURCE = Path(__file__).resolve().parents[1]
REPO = SOURCE.parent
OWNED = REPO / '.superpowers' / 'sdd' / 'u2-stage28-31'


class NamespacePackagingTests(unittest.TestCase):
    def setUp(self):
        OWNED.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='namespace-', dir=OWNED)
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        # Deliberately do not forward credential/configuration variables.
        self.env = {key: os.environ[key] for key in
                    ('PATH', 'SYSTEMROOT', 'WINDIR', 'COMSPEC') if key in os.environ}
        self.env.update(PYTHONUTF8='1', PYTHONDONTWRITEBYTECODE='1',
                        SIGFLOW_DEEPSEEK_CONFIG=str(self.directory / 'absent.json'),
                        TEMP=str(self.directory), TMP=str(self.directory))

    def run_python(self, program, *args):
        script = self.directory / 'probe.py'
        script.write_text(program, encoding='utf-8')
        result = subprocess.run([sys.executable, '-S', str(script), *map(str, args)],
                                cwd=self.directory, env=self.env, capture_output=True,
                                encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def assert_coexistence(self, u2_path):
        for order in ('u1-first', 'u2-first'):
            with self.subTest(order=order):
                self.run_python('''
import importlib, sys
from pathlib import Path
sys.path[:0] = sys.argv[1:3]
names = ['sigflow_edu_agent', 'sigflow_u2']
if sys.argv[3] == 'u2-first': names.reverse()
for name in names: importlib.import_module(name)
import sigflow_edu_agent as u1, sigflow_u2 as u2
from sigflow_edu_agent import cards
from sigflow_u2.host.runtime import Runtime
from sigflow_u2.rag import database
from sigflow_u2.runtime import session
assert Path(u1.__file__).resolve() == Path(sys.argv[1], 'sigflow_edu_agent', '__init__.py').resolve()
assert hasattr(u1, 'AGENT_VERSION')
assert Runtime.__module__ == 'sigflow_u2.host.runtime'
assert cards.__name__ == 'sigflow_edu_agent.cards'
assert not any(name.startswith('sigflow_edu_agent.host') for name in sys.modules)
''', REPO / 'agent' / 'src', u2_path, order)

    def test_source_u2_coexists_with_u1_in_both_import_orders(self):
        self.assert_coexistence(SOURCE)

    def test_source_new_and_legacy_cli_run_rule_demo(self):
        for module in ('sigflow_u2', 'sigflow_edu_agent'):
            with self.subTest(module=module):
                result = self.run_python('''
import runpy, sys
sys.path.insert(0, sys.argv.pop(1))
module = sys.argv.pop(1)
sys.argv = [module, '--demo']
runpy.run_module(module, run_name='__main__')
''', SOURCE, module)
                self.assertEqual(json.loads(result.stdout)['explain']['status'], 'Completed')

    def test_new_namespace_sidecar_launches_its_own_host(self):
        self.run_python('''
import sys
from pathlib import Path
from unittest.mock import patch
sys.path[:0] = sys.argv[1:3]
import sigflow_edu_agent
from sigflow_u2.host import closed_loop as runner
class StopLaunch(Exception): pass
seen = []
class Child:
    def __init__(self, argv, env, **kwargs):
        seen.append(argv)
        if len(seen) == 2: raise StopLaunch()
    def send(self, value): pass
    def ready(self, deadline):
        return dict(port=12345, project_id='project-test')
class HTTP:
    def __init__(self, *args): pass
    def close(self): pass
    def request(self, method, route, body=None, **kwargs):
        if route.endswith('/health'): return dict(instance_id='unused', protocol_version='edu.api.v1', edition='edu')
        if route.endswith('/capabilities'): return dict(capabilities=[dict(id='eda.synth', ready=True, plugin_id='eda-synth-yosys')])
        if route.endswith('/context'): return dict(revision='rev-test')
        if route.endswith('/snapshots'): return dict(snapshot_id='snapshot-test')
        if route.endswith('/grants'): return dict(grant_id='grant-test')
        if '/grants/' in route: return dict(used_jobs=0)
        if route.endswith('/report'): return dict(completeness='complete', artifacts=['one'], input_fingerprint='fp')
        if route.endswith('/jobs'):
            if method == 'GET': return dict(jobs=[])
            return dict(job_id='job-test')
        if '/jobs/' in route: return dict(state='Succeeded')
        raise AssertionError(route)
with patch.object(runner, 'Child', Child), patch.object(runner, 'HTTP', HTTP), \
     patch.object(runner, 'check_ready'), patch.object(runner, 'require'), \
     patch.object(runner, 'close_children'):
    try: runner.exercise(Path('unused'), 'rule', 10, Path.cwd())
    except StopLaunch: pass
assert len(seen) == 2
assert seen[1][seen[1].index('-m') + 1] == 'sigflow_u2.host', seen[1]
''', REPO / 'agent' / 'src', SOURCE)

    @unittest.skipUnless(all(importlib.util.find_spec(name) for name in ('pip', 'setuptools', 'wheel')),
                         'offline wheel verification requires existing pip/setuptools/wheel')
    def test_offline_wheel_installs_only_new_namespace_with_resources_and_cli(self):
        project = self.directory / 'project'
        project.mkdir()
        shutil.copy2(SOURCE / 'pyproject.toml', project)
        shutil.copytree(SOURCE / 'sigflow_edu_agent', project / 'sigflow_edu_agent',
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        if (SOURCE / 'sigflow_u2').is_dir():
            shutil.copytree(SOURCE / 'sigflow_u2', project / 'sigflow_u2')
        wheel_dir = self.directory / 'wheels'
        result = subprocess.run([sys.executable, '-m', 'pip', 'wheel', str(project),
                                 '--no-build-isolation', '--no-deps', '--no-index',
                                 '--disable-pip-version-check', '--wheel-dir', str(wheel_dir)],
                                env=self.env, cwd=self.directory, capture_output=True,
                                encoding='utf-8', timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        wheels = list(wheel_dir.glob('sigflow_u2_teaching-*.whl'))
        self.assertEqual(len(wheels), 1, 'U2 needs its own distribution name')
        with zipfile.ZipFile(wheels[0]) as archive:
            names = archive.namelist()
            self.assertFalse(any(name.startswith('sigflow_edu_agent/') for name in names))
            self.assertIn('sigflow_u2/host/runtime.py', names)
            self.assertIn('sigflow_u2/rag/resources/courses/concepts.yaml', names)
            self.assertIn('sigflow_u2/rag/resources/courses/passages/ysyx.jsonl', names)
            self.assertFalse(any(name.endswith('external_official_passages.jsonl') for name in names))
        target = self.directory / 'installed'
        result = subprocess.run([sys.executable, '-m', 'pip', 'install', str(wheels[0]),
                                 '--target', str(target), '--no-index', '--no-deps',
                                 '--disable-pip-version-check'], env=self.env, cwd=self.directory,
                                capture_output=True, encoding='utf-8', timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_coexistence(target)
        self.run_python('''
import importlib, importlib.metadata, importlib.resources, json, runpy, sys
sys.path.insert(0, sys.argv[1])
dist = importlib.metadata.distribution('sigflow-u2-teaching')
assert not dist.requires or all('extra ==' in value for value in dist.requires)
for entry in dist.entry_points:
    assert entry.value.startswith('sigflow_u2.'), entry
    assert callable(entry.load())
resource = importlib.resources.files('sigflow_u2.rag').joinpath('resources/courses/concepts.yaml')
assert resource.read_text(encoding='utf-8')
sys.argv = ['sigflow_u2', '--demo']
runpy.run_module('sigflow_u2', run_name='__main__')
''', target)


if __name__ == '__main__':
    unittest.main()
