"""Export BOTH declaration sources from one immutable input snapshot.

Writes <yosys-json>.ports.v2.json for the production GUI adapter. Explicit sources
only: no HDL parsing, source-name heuristics or direction/width fallbacks.
The selected M0 frontend supports plain SV sources with no external includes or
parameter overrides; projects needing these must supply a future export adapter.
"""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def export(args):
    output=args.output.resolve();output.parent.mkdir(parents=True,exist_ok=True)
    sources=[dict(path=p.resolve().as_posix(),sha256=sha(p)) for p in args.sources]
    work=output.parent/(output.stem+'-declarations');work.mkdir(exist_ok=True)
    # Source argv is passed directly; Yosys uses its own quoted script grammar.
    def yquote(s):
        if '\n' in s or '\r' in s: raise ValueError('newline in path')
        return '"'+s.replace('\\','/').replace('"','\\"')+'"'
    if not args.top.replace('_','a').isalnum(): raise ValueError('unsupported top identifier')
    script=work/'export.ys'
    script.write_text('read_verilog -sv '+' '.join(yquote(s['path']) for s in sources)+
                      '\nhierarchy -check -top '+args.top+'\nproc\nwrite_json '+yquote(output.as_posix())+'\n',encoding='utf-8')
    env=dict(os.environ)
    env['VERILATOR_ROOT']=str(args.verilator.resolve().parent.parent)
    tool_hashes={'yosys_sha256':sha(args.yosys),'verilator_sha256':sha(args.verilator)}
    adapters=json.loads((ROOT/'registry/tools/port-export-adapters.v1.json').read_text(encoding='utf-8'))
    if tool_hashes not in adapters['supported_tool_pairs']:
        raise RuntimeError('E_MANIFEST_VERSION: tool pair is not validated by this AST adapter')
    logs=[]
    commands=[[str(args.yosys),'-Q','-T','-s',str(script)],
              [str(args.verilator),'--json-only','--MMD','--Mdir',str(work),'--top-module',args.top,'-Wno-fatal',
               *[s['path'] for s in sources]]]
    for i,cmd in enumerate(commands):
        result=subprocess.run(cmd,cwd=ROOT,env=env,capture_output=True,encoding='utf-8',errors='replace',timeout=120)
        (work/f'tool-{i}.log').write_text(result.stdout+result.stderr,encoding='utf-8')
        logs.append(dict(argv=cmd,exit_code=result.returncode))
        if result.returncode: raise RuntimeError(f'export failed: {work}/tool-{i}.log')
    for s in sources:
        if sha(s['path'])!=s['sha256']:raise RuntimeError('E_MANIFEST_STALE: source changed during export')
    # Consume the tool's dependency records, not HDL text or broken tree.meta.
    # Any user include outside the explicit source closure is rejected.
    allowed={Path(s['path']).resolve() for s in sources}
    builtins={args.verilator.resolve(),args.verilator.resolve().parent.parent/'include/verilated_std.sv',
              args.verilator.resolve().parent.parent/'include/verilated_std_waiver.vlt'}
    dependencies=[]
    for line in (work/('V'+args.top+'__verFiles.dat')).read_text(encoding='utf-8').splitlines():
        if not line.startswith('S '):continue
        if not line.endswith('"'):raise RuntimeError('E_EXPORT_DEPENDENCY_FORMAT')
        path=Path(line.rsplit('"',2)[1]).resolve()
        if path not in allowed and path not in builtins:
            raise RuntimeError('E_EXPORT_UNDECLARED_DEPENDENCY: '+str(path))
        dependencies.append(dict(path=path.as_posix(),sha256=sha(path)))
    if not allowed.issubset({Path(s['path']).resolve() for s in dependencies}):
        raise RuntimeError('E_EXPORT_DEPENDENCY_FORMAT: incomplete source closure')
    ast=work/('V'+args.top+'.tree.json')
    snapshot=dict(sources=sources,dependencies=dependencies,adapter='verilator-json-basicdtype-m0.v1',ast_sha256=sha(ast),yosys_sha256=sha(output),
                  parameters={},include_paths=[],commands=logs,
                  tools=tool_hashes)
    request=work/'snapshot.json';request.write_text(json.dumps(snapshot),encoding='utf-8')
    sidecar=Path(str(output)+'.ports.v2.json')
    subprocess.run([str(args.builder),str(ast),str(output),args.top,str(request),str(sidecar)],check=True,env=env)
    return sidecar
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--top',required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--verilator',type=Path,default=ROOT/'tools/verilator/verilator-install/bin/verilator_bin_dbg.exe')
    p.add_argument('--yosys',type=Path,default=ROOT/'external/fpga-tools/runtime/yosys/bin/yosys.exe')
    p.add_argument('--builder',type=Path,default=ROOT/'build-m0/sigflow_port_manifest.exe')
    p.add_argument('sources',type=Path,nargs='+')
    print(export(p.parse_args()))
if __name__=='__main__':main()
