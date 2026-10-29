"""No dependency on pytest, experiment paths, a running U2 or network."""
from pathlib import Path
import socket,unittest,json,sys
ROOT=Path(__file__).resolve().parent
def deny(*args,**kwargs):raise PermissionError('external network disabled in module tests')
socket.socket.connect=deny
suite=unittest.defaultTestLoader.discover(str(ROOT),pattern='test_*.py')
result=unittest.TextTestRunner(verbosity=2).run(suite)
print(json.dumps(dict(tests=result.testsRun,failures=len(result.failures),errors=len(result.errors),skipped=len(result.skipped))))
raise SystemExit(not result.wasSuccessful())
