from pathlib import Path
import unittest,tempfile,subprocess,sys,sqlite3,shutil
from ua10 import MemoryHost,MemoryConfig,Scope

CHILD='''
import os,sys
from ua10 import MemoryHost,MemoryConfig,LearningEvent,JobEvidence,Observation,Scope
def fault(phase):
 if phase==sys.argv[2]:os._exit(73)
with MemoryHost(MemoryConfig(sys.argv[1]),_fault=fault) as host:
 if sys.argv[2]=='after_source_commit':
  host.create_profile('learner');scope=host.create_session('learner','project','instance','session');api=host.bind(scope)
  api.create_run('r1','snapshot',run_id='run',execution_mode='real')
  job=JobEvidence('job','project','instance','r1','snapshot','report','sha256:'+'0'*64,kind='real',outcome='failed')
  api.append_learning_event(LearningEvent('event','run','issue_observed',('latch',),job=job,created_at='2025-01-01T00:00:00Z'),observation=Observation('LATCH_INFERRED','exercise'),available_at='2025-01-02T00:00:00Z',synthetic=True)
 else:
  api=host.bind(Scope('learner','project','instance','session'));api.revoke_learning_event('event')
'''
class CrashTests(unittest.TestCase):
    def test_commit_replay_revocation_and_stale_projection(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp);r=subprocess.run([sys.executable,'-c',CHILD,str(path),'after_source_commit'],capture_output=True)
            self.assertEqual(r.returncode,73,r.stderr.decode(errors='replace'))
            with MemoryHost(MemoryConfig(path)) as host:
                api=host.bind(Scope('learner','project','instance','session'))
                self.assertEqual(len(api.get_learning_timeline(as_of='2025-01-03T00:00:00Z').facts),1)
                self.assertFalse(api.get_learning_timeline(as_of='2025-01-01T12:00:00Z').facts)
                self.assertEqual(host.authority.v1.db.connection.execute('SELECT count(*) FROM learning_events').fetchone()[0],1)
            shutil.copyfile(path/'derived.sqlite',path/'stale.sqlite')
            r=subprocess.run([sys.executable,'-c',CHILD,str(path),'after_revocation_commit'],capture_output=True)
            self.assertEqual(r.returncode,73,r.stderr.decode(errors='replace'))
            shutil.copyfile(path/'stale.sqlite',path/'derived.sqlite')
            with MemoryHost(MemoryConfig(path)) as host:
                api=host.bind(Scope('learner','project','instance','session'))
                self.assertFalse(api.search_memories('锁存器诊断').facts);api.rebuild_index()
                self.assertFalse(api.get_learning_timeline().facts)

if __name__=='__main__':unittest.main(verbosity=2)
