"""Actual local BGE check; caller supplies an existing model, no downloads."""
import unittest,tempfile,json,os,math,socket
from pathlib import Path
from ua10._core.embedding import BGEEncoder
from ua10.errors import ConfigurationError

class BGETests(unittest.TestCase):
    def test_content_vectors_reopen_delete_and_space(self):
        model=Path(os.environ['UA10_TEST_MODEL']);text='组合逻辑路径缺少赋值，已修改但尚未复测。'
        updated='组合逻辑路径已补全；同范围验证通过，独立掌握状态未知。'
        with tempfile.TemporaryDirectory() as tmp:
            db=Path(tmp)/'vectors.sqlite';enc=BGEEncoder(db,model)
            try:
                original=enc.encode(text);self.assertEqual(enc.calls,1)
                self.assertEqual(original,enc.encode(text));self.assertEqual(enc.calls,1)
                final=enc.encode(updated);self.assertEqual(enc.calls,2)
                self.assertNotEqual(original,final)
                inputs=enc.tokenizer(updated,return_tensors='pt',padding=True,truncation=True,max_length=512)
                with enc.torch.no_grad():
                    recomputed=enc.torch.nn.functional.normalize(enc.model(**inputs).last_hidden_state[:,0],p=2,dim=1)[0].tolist()
                self.assertLess(max(abs(a-b) for a,b in zip(final,recomputed)),1e-6)
                enc.purge(text)
                self.assertEqual(enc.db.execute('SELECT count(*) FROM vectors').fetchone()[0],1)
            finally:enc.close()
            enc=BGEEncoder(db,model)
            try:
                self.assertEqual(final,enc.encode(updated));self.assertEqual(enc.calls,0)
                enc.db.execute("UPDATE vectors SET space='wrong-model-version'");enc.db.commit()
                with self.assertRaises(ConfigurationError):enc.encode(updated)
                enc.purge(updated);self.assertEqual(enc.encode(updated),final)
            finally:enc.close()

if __name__=='__main__':
    socket.socket.connect=lambda *a,**k:(_ for _ in ()).throw(PermissionError('network disabled'))
    unittest.main(verbosity=2)
