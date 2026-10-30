"""源码依赖和公开模块入口的轻量检查，不下载任何依赖。"""
import hashlib
import json
from pathlib import Path
import unittest
import ua10


class PackagingTests(unittest.TestCase):
    def test_v1_snapshot_matches_provenance(self):
        root=Path(__file__).resolve().parents[1]/'vendor/memory'
        manifest=json.loads((root/'PROVENANCE.json').read_text(encoding='utf-8'))
        for name,expected in manifest['sha256'].items():
            with self.subTest(file=name):
                content=(root/name).read_bytes().replace(b'\r\n',b'\n')
                self.assertEqual(hashlib.sha256(content).hexdigest(),expected)

    def test_public_thin_entry_is_available_without_optional_dependencies(self):
        self.assertTrue(callable(ua10.TeachingMemory))
        self.assertEqual(ua10.__version__,'0.3.0.dev0')
        self.assertFalse(ua10.MemoryConfig(Path('.')).enable_real_api)


if __name__=='__main__':unittest.main(verbosity=2)
