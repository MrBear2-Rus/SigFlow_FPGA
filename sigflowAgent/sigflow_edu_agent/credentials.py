"""Current-user Windows DPAPI storage; no plaintext persistence fallback."""
from dataclasses import asdict
import base64
import ctypes
import json
import os
from pathlib import Path
import tempfile

from .deepseek import DeepSeekConfig

MAX_BYTES = 16384
SCHEMA = 'sigflow.deepseek.dpapi.v1'


class CredentialError(ValueError):
    """Fixed diagnostic code; never the key, path or raw OS exception."""


def default_path():
    override = os.environ.get('SIGFLOW_DEEPSEEK_CONFIG')
    return Path(override) if override else Path(__file__).resolve().parents[1] / '.local' / 'deepseek.credentials.json'


def _pairs(items):
    result = {}
    for key,value in items:
        if key in result:
            raise ValueError
        result[key] = value
    return result


def _dpapi(payload, *, decrypt=False):
    if os.name != 'nt':
        raise CredentialError('key_store_unsupported')
    from ctypes import wintypes
    class Blob(ctypes.Structure):
        _fields_ = [('size',wintypes.DWORD),('data',ctypes.POINTER(ctypes.c_ubyte))]
    buffer = ctypes.create_string_buffer(payload)
    source = Blob(len(payload),ctypes.cast(buffer,ctypes.POINTER(ctypes.c_ubyte)))
    entropy_buffer = ctypes.create_string_buffer(SCHEMA.encode('ascii'))
    entropy = Blob(len(SCHEMA),ctypes.cast(entropy_buffer,ctypes.POINTER(ctypes.c_ubyte)))
    output = Blob()
    try:
        library = ctypes.WinDLL('crypt32',use_last_error=True)
        kernel = ctypes.WinDLL('kernel32',use_last_error=True)
        function = library.CryptUnprotectData if decrypt else library.CryptProtectData
        function.argtypes = [ctypes.POINTER(Blob),ctypes.c_void_p,ctypes.POINTER(Blob),
                             ctypes.c_void_p,ctypes.c_void_p,wintypes.DWORD,ctypes.POINTER(Blob)]
        function.restype = wintypes.BOOL
        kernel.LocalFree.argtypes = [ctypes.c_void_p]
        kernel.LocalFree.restype = ctypes.c_void_p
        # No machine-wide flag: Windows binds this to the current logon user.
        if not function(ctypes.byref(source),None,ctypes.byref(entropy),None,None,1,ctypes.byref(output)):
            raise CredentialError('key_store_invalid' if decrypt else 'key_store_unavailable')
        if not output.data or not 0 < output.size <= MAX_BYTES:
            raise CredentialError('key_store_invalid')
        return ctypes.string_at(output.data,output.size)
    except OSError:
        raise CredentialError('key_store_unavailable') from None
    finally:
        ctypes.memset(buffer,0,len(buffer))
        if output.data:
            ctypes.memset(output.data,0,output.size)
            kernel.LocalFree(output.data)


def save_config(config, path=None):
    if type(config) is not DeepSeekConfig:
        raise CredentialError('key_store_invalid')
    protected = _dpapi(json.dumps(asdict(config),ensure_ascii=True).encode('ascii'))
    payload = json.dumps(dict(schema=SCHEMA,encrypted=base64.b64encode(protected).decode('ascii'))).encode('ascii')
    target = default_path() if path is None else Path(path)
    temporary = None
    try:
        target.parent.mkdir(parents=True,exist_ok=True)
        descriptor,temporary = tempfile.mkstemp(prefix='.deepseek-',suffix='.tmp',dir=target.parent)
        with os.fdopen(descriptor,'wb') as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary,target)
    except OSError:
        raise CredentialError('key_store_write_failed') from None
    finally:
        if temporary is not None:
            try:
                Path(temporary).unlink(missing_ok=True)
            except OSError:
                pass  # Only this operation's own encrypted temporary file.


def load_config(path=None):
    target = default_path() if path is None else Path(path)
    try:
        with target.open('rb') as stream:
            raw = stream.read(MAX_BYTES+1)
    except FileNotFoundError:
        return None
    except OSError:
        raise CredentialError('key_store_unavailable') from None
    try:
        if len(raw)>MAX_BYTES:
            raise ValueError
        envelope = json.loads(raw,object_pairs_hook=_pairs)
        if type(envelope) is not dict or set(envelope)!={'schema','encrypted'} or envelope['schema']!=SCHEMA:
            raise ValueError
        if type(envelope['encrypted']) is not str:
            raise ValueError
        plaintext = _dpapi(base64.b64decode(envelope['encrypted'],validate=True),decrypt=True)
        document = json.loads(plaintext,object_pairs_hook=_pairs)
        if type(document) is not dict or set(document)!={'api_key','model','base_url','timeout','max_tokens'}:
            raise ValueError
        return DeepSeekConfig(**document)
    except CredentialError:
        raise
    except Exception:
        raise CredentialError('key_store_invalid') from None
