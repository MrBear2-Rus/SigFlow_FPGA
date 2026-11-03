"""Exercise real Verilog analysis and JSONL through the production CLI."""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
binary, build = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='analysis-',dir=build) as temp:
    project=Path(temp)/'工程 space';project.mkdir();(project/'src').mkdir()
    source=project/'src/top.v'
    def run(command,*extra,code=0):
        result=subprocess.run([binary,command,'--file',str(source),*map(str,extra)],text=True,capture_output=True,timeout=12)
        assert result.returncode==code,(command,extra,result.returncode,result.stdout,result.stderr)
        lines=result.stdout.splitlines();assert len(lines)==1 and len(lines[0].encode())<1024*1024
        event=json.loads(lines[0]);return event
    source.write_text('module top(input a, output wire y);\nwire x;\nassign y=a;\nalways @(a) begin end\nendmodule\n')
    event=run('structure');assert event['ok'] and event['data']['schema']=='sigflow.structure.v1'
    nodes=event['data']['nodes']; modules=[n for n in nodes if n['kind']=='module']
    assert len(modules)==1 and modules[0]['name']=='top',nodes
    assert {n['kind'] for n in nodes}>={'module','port','signal','assign','always'}
    ids={n['id'] for n in nodes};assert len(ids)==len(nodes)
    children=[x for n in nodes for x in n['children']]
    assert all(x in ids for x in children) and modules[0]['id'] not in children
    assert modules[0]['range']['start']=={'line':1,'col':1}
    assert modules[0]['range']['end']=={'line':5,'col':9}
    event=run('lint','--project-root',project);assert event['data']['diagnostics']==[]
    source.write_text('module top(input a; endmodule\n')
    event=run('lint','--project-root',project);diagnostics=event['data']['diagnostics']
    assert diagnostics and all(d['file']=='src/top.v' and d['line']>=1 and d['col']>=1 for d in diagnostics),event
    run('lint',code=4)
    run('lint','--project-root',project/'src'/'missing',code=4)
    run('structure','--project-root',project/'src'/'missing',code=4)
    run('structure','--wat',1,code=4)
    run('lint','--project-root',Path(temp), '--project-root',project,code=4)
    run('lint','--project-root',project/'src'/'..',code=4)
    source.write_bytes(b'module top; // \xff\nendmodule\n')
    run('structure',code=4)
    source.write_text('')
    assert run('structure')['data']['nodes']==[]
    assert run('lint','--project-root',project)['data']['diagnostics']==[]
    source.write_text(' '* (2*1024*1024+1))
    run('structure',code=4)
    source.write_text('module top;\n'+''.join(f'wire a{i};\n' for i in range(5100))+'endmodule\n')
    run('structure',code=4)
    source.write_text('module top;\n'+''.join(f'wire long_{i}_'+('x'*180)+';\n' for i in range(4000))+'endmodule\n')
    event=run('structure',code=4)
    assert 'output exceeds limit' in event['message'],event
    outside=Path(temp)/'outside.v';outside.write_text('module outside; endmodule\n')
    source.unlink();source.symlink_to(outside)
    run('lint','--project-root',project,code=4)
    source.unlink()
    # Columns use UTF-16 units (as in CodeMirror) rather than UTF-8 bytes.
    source.write_text('/*x*/ module top(input a; endmodule\n')
    plain=run('lint','--project-root',project)['data']['diagnostics']
    source.write_text('/*中😀*/ module top(input a; endmodule\n')
    unicode=run('lint','--project-root',project)['data']['diagnostics']
    assert plain and unicode and plain[0]['line']==unicode[0]['line']==1
    assert unicode[0]['col']-plain[0]['col']==2,(plain,unicode)
print('Verilog analysis CLI integration passed')
