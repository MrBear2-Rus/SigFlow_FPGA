"""Existing fixed native recovery layer + actual local Qdrant; no native LLM.

UA10_FIX_ROOT points to the reviewed lightmem_fix checkout. This optional test
does not install or vendor that checkout and never invokes its managers.
"""
import unittest,os,sys,tempfile,socket,importlib.util,json
from pathlib import Path
from types import SimpleNamespace
from ua10 import MemoryHost,MemoryConfig,Scope
from ua10.adapters.lightmem import LightMemBackend
from ua10._core.util import digest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('ua10_example',ROOT/'examples/basic_usage.py')
example=importlib.util.module_from_spec(spec);spec.loader.exec_module(example)

class NativeTests(unittest.TestCase):
    def test_recoverable_native_projection_reopen_and_revoke(self):
        fix=Path(os.environ['UA10_FIX_ROOT']);sys.path.insert(0,str(fix))
        from store import RecoverableIndex
        from safety import EvidencePolicy
        from qdrant_client import QdrantClient
        from lightmem.factory.retriever.embeddingretriever.qdrant import Qdrant
        from lightmem.configs.retriever.embeddingretriever.qdrant import QdrantConfig
        with tempfile.TemporaryDirectory() as tmp:
            data=Path(tmp);opened=[]
            def factory(*,scope,source_kind,registry,encoder):
                directory=data/'native'/digest([scope,source_kind]);directory.mkdir(parents=True,exist_ok=True)
                client=QdrantClient(path=str(directory/'qdrant'),force_disable_check_same_thread=True)
                try:native=Qdrant(QdrantConfig(client=client,path=str(directory/'qdrant'),on_disk=True,collection_name='module_fixture',embedding_model_dims=512))
                except BaseException:client.close();raise
                model=str(encoder.layout)
                embedding=SimpleNamespace(config=SimpleNamespace(model=model,embedding_dims=512),embed=encoder.encode)
                contract=SimpleNamespace(model=model,dimension=512,preprocessing_version=encoder.spec.preprocessing_version,weights_sha256=encoder.weights)
                index=RecoverableIndex(native,directory/'authority.sqlite',embedding,contract,EvidencePolicy(scope,registry,source_kind=source_kind))
                opened.append(index);return index
            cfg=MemoryConfig(data/'module',embedding_model_path=Path(os.environ['UA10_TEST_MODEL']),backend='lightmem',allow_experimental_lightmem=True)
            scope=Scope(example.LEARNER,example.PROJECT,example.INSTANCE,'session-two')
            host=MemoryHost(cfg,lightmem_backend=LightMemBackend(factory))
            try:
                example.seed(host);api=host.bind(scope)
                self.assertTrue(api.search_memories('锁存器诊断').facts)
                self.assertEqual(len(opened),1);index=opened[0]
                rows=index.get_all();self.assertEqual(len(rows),4)
                for row in rows:
                    eids=row['payload']['evidence_ids'];self.assertEqual(len(eids),1)
                    expected=host.encoder.encode(row['payload']['memory'])
                    actual=index._snapshot(row['id'])['vector']
                    self.assertLess(max(abs(a-b) for a,b in zip(expected,actual)),1e-6)
                api.revoke_learning_event('event-one');api.rebuild_index()
                self.assertFalse(any('event-one' in x['payload']['evidence_ids'] for x in index.get_all()))
            finally:
                for index in opened:index.close()
                opened.clear();host.close()
            host=MemoryHost(cfg,lightmem_backend=LightMemBackend(factory))
            try:
                api=host.bind(scope);self.assertTrue(api.search_memories('锁存器诊断').facts)
                self.assertEqual(len(opened[0].get_all()),3)
                for eid in ('event-modified','event-passed','event-three'):api.revoke_learning_event(eid)
                self.assertFalse(api.search_memories('锁存器诊断').facts)
                self.assertEqual(opened[0].get_all(),[])
            finally:
                for index in opened:index.close()
                host.close()

if __name__=='__main__':
    socket.socket.connect=lambda *a,**k:(_ for _ in ()).throw(PermissionError('network disabled'))
    unittest.main(verbosity=2)
