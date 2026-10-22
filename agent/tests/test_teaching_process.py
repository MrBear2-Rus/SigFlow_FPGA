"""Real U1 bootstrap process with the opt-in in-process U2 composition."""
import http.client,json,os
from pathlib import Path
from queue import Queue
import subprocess,sys,tempfile
from threading import Thread
import unittest
import importlib.util

ROOT=Path(__file__).resolve().parents[2]
if sys.version_info < (3, 11):
    raise unittest.SkipTest('optional U2 composition requires Python 3.11+')
sys.path.append(str(ROOT/'sigflowAgent'))
try:
    _u2_spec = importlib.util.find_spec('sigflow_u2')
except ModuleNotFoundError as error:
    if error.name != 'sigflow_u2':
        raise
    _u2_spec = None
if _u2_spec is None:
    raise unittest.SkipTest('optional U2 source/package is not installed')


class TeachingProcessTests(unittest.TestCase):
    def test_one_ready_line_same_port_extension_and_authenticated_shutdown(self):
        from sigflow_edu_agent.__main__ import build_parser
        self.assertIn('--u2-teaching',build_parser().format_help(),'U1 has no opt-in U2 lifecycle')
        owned=ROOT/'.superpowers/sdd/u1-teaching-tests/process'
        owned.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=owned) as directory:
            env={k:v for k,v in os.environ.items() if not k.startswith(('DEEPSEEK_','SIGFLOW_'))}
            env.update(PYTHONPATH=os.pathsep.join(map(str,(ROOT/'agent/src',ROOT/'sigflowAgent'))),
                SIGFLOW_DEEPSEEK_CONFIG=str(Path(directory)/'absent.json'),TEMP=directory,TMP=directory)
            process=subprocess.Popen([sys.executable,'-m','sigflow_edu_agent','--bootstrap-stdin','--u2-teaching'],
                stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,cwd=directory,env=env)
            output=Queue()
            Thread(target=lambda:output.put(process.stdout.readline()),daemon=True).start()
            try:
                boot=dict(type='sigflow-bootstrap',protocol='edu.api.v1',instance_id='instance',nonce='a'*64,
                    gateway_url='http://127.0.0.1:1',gateway_token='gateway-test',ui_token='u1-ui',data_directory=directory)
                process.stdin.write((json.dumps(boot)+'\n').encode());process.stdin.flush()
                raw=output.get(timeout=15)
                self.assertTrue(raw,'shared U1 startup failed')
                ready=json.loads(raw)
                self.assertEqual((ready['type'],ready['nonce']),('ready','a'*64))
                port=ready['port']
                for method,path in (('GET','capabilities'),('POST','shutdown')):
                    connection=http.client.HTTPConnection('127.0.0.1',port,timeout=5)
                    try:
                        connection.request(method,'/api/v1/'+path,headers={'Authorization':'Bearer u1-ui'})
                        response=connection.getresponse();reply=json.loads(response.read())
                        self.assertEqual(response.status,202 if method=='POST' else 200)
                        if method=='GET':self.assertEqual(reply['data']['teaching']['schema_version'],'u1.u2-teaching.v1')
                    finally:connection.close()
                process.wait(timeout=8)
                self.assertEqual(process.returncode,0)
                self.assertEqual(process.stdout.read(),b'')
                self.assertTrue(list((Path(directory)/'u1-u2').glob('*.sqlite3')))
            finally:
                if process.poll() is None:process.kill();process.wait(timeout=5)
                for stream in (process.stdin,process.stdout,process.stderr):stream.close()
