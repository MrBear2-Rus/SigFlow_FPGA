import hashlib,json,os,re,subprocess,sys,tempfile
from pathlib import Path
binary,build=sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="synth-",dir=build) as tmp:
 root=Path(tmp)/"工程 space";root.mkdir();source=root/"top.sv";source.write_text("module top(input a, output y); assign y=a; endmodule\n")
 tool=root/"yosys fake";tool.write_text("""#!/usr/bin/python3
import json,os,re,sys
from pathlib import Path
script=Path(sys.argv[2]).read_text()
assert 'read_verilog -sv' in script and 'synth_gowin -family gw1n -top top' in script
output=Path(re.search(r'write_json "([^\\n]+)"',script).group(1))
mode=os.environ.get('FAKE_MODE','success')
print('synthesis fixture')
if mode=='long': print('x'*100000)
if mode=='nonzero': sys.exit(17)
if mode=='missing': sys.exit(0)
if mode=='invalid': output.write_text('{invalid');sys.exit(0)
if mode=='wrongtop': output.write_text(json.dumps({'modules':{'other':{'ports':{},'cells':{},'netnames':{}}}}));sys.exit(0)
if mode=='missingfield': output.write_text(json.dumps({'modules':{'top':{'ports':{},'cells':{}}}}));sys.exit(0)
output.write_text(json.dumps({'modules':{'top':{'ports':{'a':{}},'cells':{},'netnames':{}}}}))
""");tool.chmod(0o700);index=0
 def run(params=None,mode='success',toolpath=True,extra=()):
  global index
  index+=1;job=f'job-{index}';work=root/'.sigflow/agent/jobs/synth'/job;work.mkdir(parents=True)
  data={'top_module':'top','source_files':[str(source)]};data.update(params or {})
  env=dict(os.environ,FAKE_MODE=mode);env.pop('SIGFLOW_TOOL_RUNTIME_DIR',None);env.pop('SIGFLOW_YOSYS',None);env['PATH']='/nonexistent'
  if toolpath:env['SIGFLOW_YOSYS']=str(tool)
  cmd=[binary,'run','--job-type','synth','--work-dir',str(work),'--job-id',job,'--project',str(root),'--project-id','p1','--params','-','--timeout-sec','5',*extra]
  result=subprocess.run(cmd,input=json.dumps(data),text=True,capture_output=True,env=env,timeout=12)
  events=[json.loads(x) for x in result.stdout.splitlines()]
  report=json.loads((work/'reports/report.json').read_text())
  assert sum(x['type']=='result' for x in events)==1,(result.stdout,result.stderr)
  return result,report,work
 for strategy in ['baseline','debug','resource_optimized']:
  result,report,work=run({'strategy':strategy})
  assert result.returncode==0,(result.stdout,result.stderr)
  assert report['state']=='Succeeded' and report['exit_code']==0
  art=report['artifacts'][0];data=(work/art['path']).read_bytes()
  assert art['id']=='netlist' and art['sha256']==hashlib.sha256(data).hexdigest()
  assert json.loads(data)['modules']['top']['ports']=={'a':{}}
 for mode in ['nonzero','missing','invalid','wrongtop','missingfield']:
  result,report,work=run(mode=mode);assert result.returncode==4,(mode,result.stdout,result.stderr)
  assert report['state']=='Failed' and not report['artifacts']
  if mode=='nonzero':assert report['exit_code']==17
 result,report,work=run(mode='long',extra=['--max-output-bytes','128']);assert result.returncode==0 and {'output_truncated':True} in report['metrics']
 result,report,work=run(toolpath=False);assert result.returncode==4 and report['exit_code']==-1
 for invalid in [{'strategy':'bad'},{'top_module':'top;bad'},{'source_files':[]},
                 {'source_files':['/etc/passwd']},{'output_json':'/tmp/escape.json'},
                 {'yosys_family':'gw1n;bad'},{'source_files':[None]}]:
  result,report,work=run(invalid);assert result.returncode==4,(invalid,result.stdout)
  assert report['state']=='Failed' and not report['artifacts']
 print(f'Yosys provider: {index} fake-tool cases passed')
