"""Optional composition tests must remain discoverable in a base U1 install."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]


class OptionalCompositionDiscoveryTests(unittest.TestCase):
    def _check_parent(self, module, case, method):
        owned = ROOT/'.superpowers/sdd/u2-stage35-current-adaptation-plan/u1'
        owned.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=owned) as directory:
            workspace = Path(directory)/'fresh-checkout'
            class ParentChecked(Exception):
                pass
            def probe(*args, **kwargs):
                self.assertTrue(Path(kwargs['dir']).is_dir(), 'test temporary parent is absent in a fresh checkout')
                raise ParentChecked()
            target = case(method)
            try:
                with patch.object(module, 'ROOT', workspace, create=True), \
                     patch.object(module, '__file__', str(workspace/'agent/tests/test_teaching_store.py')), \
                     patch.object(module.tempfile, 'TemporaryDirectory', side_effect=probe):
                    with self.assertRaises(ParentChecked):
                        getattr(target, 'setUp' if method != 'test_one_ready_line_same_port_extension_and_authenticated_shutdown' else method)()
            finally:
                target.doCleanups()

    def test_base_association_tests_create_their_temporary_parent(self):
        from tests import test_teaching_store as module
        self._check_parent(module, module.TeachingStoreTests, 'test_limits_reject_without_evicting_unfinished_associations')

    def test_optional_composition_tests_create_their_temporary_parent(self):
        from tests import test_teaching_service, test_teaching_process
        for module, case, method in (
                (test_teaching_service, test_teaching_service.TeachingServiceTests,
                 'test_explicit_hint_and_finish_project_pending_then_real_terminal'),
                (test_teaching_process, test_teaching_process.TeachingProcessTests,
                 'test_one_ready_line_same_port_extension_and_authenticated_shutdown')):
            with self.subTest(module=module.__name__):
                self._check_parent(module, case, method)

    def test_missing_u2_and_older_python_skip_only_composition_modules(self):
        script = r'''
import importlib.abc, runpy, sys, unittest
mode, target = sys.argv[1:]
class NoU2(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname == 'sigflow_u2' or fullname.startswith('sigflow_u2.'):
            raise ModuleNotFoundError('optional U2 is absent', name=fullname)
if mode == 'missing':
    sys.meta_path.insert(0, NoU2())
else:
    sys.version_info = (3, 10, 0)
try:
    runpy.run_module(target, run_name='discovery_probe')
except unittest.SkipTest as error:
    assert 'U2' in str(error), str(error)
else:
    raise AssertionError('optional composition module was not skipped')
assert not any(name == 'sigflow_u2' or name.startswith('sigflow_u2.') for name in sys.modules)
'''
        env = dict(os.environ, PYTHONPATH=os.pathsep.join(map(str, (ROOT/'agent/src', ROOT/'agent', ROOT/'sigflowAgent'))))
        for mode in ('missing', 'older'):
            for module in ('test_teaching_service', 'test_teaching_http', 'test_teaching_process'):
                with self.subTest(mode=mode, module=module):
                    result = subprocess.run([sys.executable, '-c', script, mode, 'tests.'+module],
                                            env=env, cwd=ROOT, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
