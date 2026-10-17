"""Simulation acceptance boundaries; fixtures never count as real EDA runs."""
from copy import deepcopy
import importlib.util
import os
from pathlib import Path
import tempfile
import subprocess
import unittest


class SimulationAcceptanceTests(unittest.TestCase):
    def module(self):
        self.assertIsNotNone(importlib.util.find_spec('sigflow_edu_agent.host.simulation_acceptance'),
                             'simulation acceptance boundary is missing')
        from sigflow_edu_agent.host import simulation_acceptance
        return simulation_acceptance

    def fixtures(self):
        scope = dict(project_id='p1', revision='r1', snapshot_id='snap1')
        report = dict(**scope, job_id='job-run', state='Succeeded', exit_code=0,
                      origin='core', schema_version='edu.jobreport.v1', completeness='complete',
                      capability='eda.sim.run', plugin_id='eda-sim-verilator-run',
                      input_fingerprint='fingerprint',
                      artifacts=[dict(artifact_id='art-wave', schema='eda.wave.vcd.v1',
                                      sha256='a'*64, role='primary')])
        metadata = dict(artifact_id='art-wave', project_id='p1', revision='r1', job_id='job-run',
                        sha256='a'*64, schema='eda.wave.vcd.v1', role='waveform',
                        media_type='text/plain', size='128', inline_readable=True)
        return scope, report, metadata

    def test_waveform_facts_only_certify_bound_nonempty_vcd(self):
        m = self.module()
        scope, report, metadata = self.fixtures()
        result = m.verify_wave_artifact(report, metadata, scope, 'job-run')
        self.assertEqual(result, dict(artifact_id='art-wave', sha256='a'*64,
                                     schema='eda.wave.vcd.v1', size_bytes=128,
                                     metadata_verified=True, exact_samples_verified=False))

    def test_changed_job_revision_schema_hash_or_size_is_denied(self):
        m = self.module()
        scope, report, metadata = self.fixtures()
        for field, value in (('project_id','other'), ('revision','r2'), ('job_id','old'),
                             ('artifact_id','other'), ('sha256','b'*64),
                             ('schema','eda.wave.fst.v1'), ('size','0'), ('size',True),
                             ('size','-1'), ('size','18446744073709551616')):
            with self.subTest(field=field, value=value), self.assertRaises(m.RunFailure):
                m.verify_wave_artifact(report, metadata | {field:value}, scope, 'job-run')

    def test_incomplete_or_failed_core_report_cannot_certify_waveform(self):
        m = self.module()
        scope, report, metadata = self.fixtures()
        for field, value in (('snapshot_id','snap-old'), ('origin','fixture'),
                             ('completeness','partial'), ('state','Failed'), ('exit_code',True),
                             ('plugin_id','eda-sim-verilator'), ('input_fingerprint',''),
                             ('artifacts',[])):
            with self.subTest(field=field), self.assertRaises(m.RunFailure):
                m.verify_wave_artifact(report | {field:value}, metadata, scope, 'job-run')
        duplicate = deepcopy(report)
        duplicate['artifacts'] *= 2
        with self.assertRaises(m.RunFailure):
            m.verify_wave_artifact(duplicate, metadata, scope, 'job-run')

    @unittest.skipUnless(os.name == 'nt', 'Windows bundled tool environment')
    def test_tools_are_prepared_only_in_owned_child_environment(self):
        m = self.module()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            repo, owned, gitroot = root/'repo', root/'owned', root/'git'
            owned.mkdir()
            for path in (repo/'3rd/mingw-msvcrt/mingw64/bin/mingw32-make.exe',
                         repo/'3rd/mingw-msvcrt/mingw64/bin/g++.exe',
                         repo/'tools/verilator/verilator-install/bin/verilator_bin_dbg.exe',
                         gitroot/'cmd/git.exe', gitroot/'usr/bin/sh.exe', gitroot/'usr/bin/perl.exe'):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'test-executable')
            inherited = dict(PATH=str(gitroot/'cmd'), PRIVATE='kept')
            original = dict(inherited)
            env = m.simulation_environment(owned, repo_root=repo, inherited=inherited)
            self.assertEqual(inherited, original)
            self.assertEqual(env['PRIVATE'], 'kept')
            self.assertEqual((owned/'simulation-tools/make.exe').read_bytes(), b'test-executable')
            self.assertTrue(env['PATH'].startswith(str(owned/'simulation-tools')+os.pathsep))
            self.assertEqual(Path(env['SIGFLOW_SH']), gitroot/'usr/bin/sh.exe')
            self.assertEqual(Path(env['VERILATOR_ROOT']), repo/'tools/verilator/verilator-install')
            self.assertEqual((repo/'3rd/mingw-msvcrt/mingw64/bin/mingw32-make.exe').read_bytes(), b'test-executable')

    @unittest.skipUnless(os.name == 'nt', 'Windows bundled tool environment')
    def test_missing_tool_fails_without_fabricating_readiness(self):
        m = self.module()
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(m.RunFailure, '^simulation_tools_missing$'):
                m.simulation_environment(Path(temp), repo_root=Path(temp)/'missing', inherited={'PATH':''})
            self.assertFalse((Path(temp)/'simulation-tools').exists())

    @unittest.skipUnless(os.name == 'nt', 'Windows bundled make regression')
    def test_bundled_make_uses_child_overrides_instead_of_build_machine_paths(self):
        m = self.module()
        with tempfile.TemporaryDirectory() as temp:
            env = m.simulation_environment(Path(temp))
            makefile = ('CXX = missing/build-machine/cl.exe\n'
                        'LINK = missing/build-machine/cl.exe\n'
                        'AR = missing/build-machine/lib.exe\n'
                        'PYTHON3 = missing/build-machine/python.exe\n'
                        '$(info tools=$(CXX)|$(LINK)|$(AR)|$(PYTHON3))\nall:;\n')
            result = subprocess.run([str(Path(temp)/'simulation-tools/make.exe'), '-f', '-'],
                                    input=makefile, text=True, capture_output=True, env=env, timeout=5)
            self.assertEqual(result.returncode, 0)
            self.assertIn('tools=g++|g++|ar|python3', result.stdout)
            self.assertNotIn('missing/build-machine', result.stdout)

