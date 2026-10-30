"""Library utilities: no path discovery, environment mutation or socket patches."""
from pathlib import Path
import json,hashlib

def serial(value):
    return json.dumps(value,ensure_ascii=False,sort_keys=True,separators=(',',':'))

def digest(value):
    return hashlib.sha256(serial(value).encode('utf-8')).hexdigest()

def save(path,value):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,ensure_ascii=False,indent=2),encoding='utf-8')

def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))
