"""Optional local BGE; content-addressed vectors with validated space metadata."""
import hashlib,json,sqlite3,math
from pathlib import Path
from types import SimpleNamespace
from .util import digest,serial
from ..errors import ConfigurationError

def source_ref(event):return digest(['longitudinal-frozen-v1',event['event_id']])
def representation(event):return event['summary']+' [SRC:'+source_ref(event)+']'

class NoEmbeddings:
    available=False;weights='none';measurement='utf8_bytes_token_upper_bound'
    spec=SimpleNamespace(preprocessing_version='no-embedding-v1')
    def encode(self,text):return None
    def tokens(self,text):return len(text.encode('utf-8'))
    def close(self):pass
    def purge(self,text):pass
    def purge_event(self,event_id):pass

class BGEEncoder:
    available=True;measurement='local_bge_tokenizer'
    def __init__(self,path,model_path):
        self.layout=Path(model_path)
        weight=self.layout/'model.safetensors'
        if not weight.is_file():raise ConfigurationError('local model.safetensors is required; no network fallback')
        try:
            import torch
            from transformers import AutoTokenizer,AutoModel
        except ImportError as exc:raise ConfigurationError('install the bge extra in this environment') from exc
        self.torch=torch;torch.set_num_threads(2)
        self.tokenizer=AutoTokenizer.from_pretrained(str(self.layout),local_files_only=True,trust_remote_code=False)
        self.model=AutoModel.from_pretrained(str(self.layout),local_files_only=True,trust_remote_code=False).to('cpu').eval()
        self.dimension=self.model.config.hidden_size
        if self.dimension!=512:raise ConfigurationError('this BGE adapter requires 512 dimensions')
        with weight.open('rb') as stream:self.weights=hashlib.file_digest(stream,'sha256').hexdigest()
        self.spec=SimpleNamespace(preprocessing_version='BGE-CLS-L2-max512-no-prefix-fix-v1')
        self.space=serial([self.weights,self.dimension,self.spec.preprocessing_version])
        self.db=sqlite3.connect(path);self.db.execute('PRAGMA synchronous=FULL')
        self.db.execute('CREATE TABLE IF NOT EXISTS vectors(hash TEXT PRIMARY KEY,text TEXT,space TEXT,vector TEXT)')
        self.calls=0
    def encode(self,text):
        key=digest(text);row=self.db.execute('SELECT text,space,vector FROM vectors WHERE hash=?',(key,)).fetchone()
        if row:
            if row[0]!=text or row[1]!=self.space:raise ConfigurationError('embedding space changed: explicitly rebuild in a new index directory')
            vector=json.loads(row[2])
        else:
            inputs=self.tokenizer(text,return_tensors='pt',padding=True,truncation=True,max_length=512)
            with self.torch.no_grad():
                vector=self.torch.nn.functional.normalize(self.model(**inputs).last_hidden_state[:,0],p=2,dim=1)[0].tolist()
            self.calls+=1
        if len(vector)!=512 or not all(math.isfinite(x) for x in vector) or not math.isclose(sum(x*x for x in vector),1.,abs_tol=1e-5):
            raise ValueError('invalid embedding vector')
        if not row:
            with self.db:self.db.execute('INSERT INTO vectors VALUES (?,?,?,?)',(key,text,self.space,serial(vector)))
        return vector
    def tokens(self,text):return len(self.tokenizer.encode(text,add_special_tokens=False,truncation=False,verbose=False))
    def purge(self,text):
        with self.db:self.db.execute('DELETE FROM vectors WHERE hash=?',(digest(text),))
    def purge_event(self,event_id):
        marker='[SRC:'+digest(['longitudinal-frozen-v1',event_id])+']'
        with self.db:self.db.execute('DELETE FROM vectors WHERE instr(text,?)>0',(marker,))
    def close(self):self.db.close()
