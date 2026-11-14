"""Build production code, execute CTest and archive factual K1 gate evidence."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'.work/k1'
EVIDENCE=ROOT/'docs/virtual/evidence'
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    WORK.mkdir(parents=True,exist_ok=True);EVIDENCE.mkdir(parents=True,exist_ok=True)
    cmake=ROOT/'.work/cmake-python/cmake/data/bin/cmake.exe';ctest=cmake.with_name('ctest.exe')
    env=dict(os.environ);env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+env.get('PATH','')
    commands=[]
    (WORK/'ctest-final.xml').unlink(missing_ok=True)
    failure=None
    for name,argv in [('build-final',[cmake,'--build',ROOT/'build-m0','--parallel','4']),
                      ('ctest-final',[ctest,'--test-dir',ROOT/'build-m0','--output-on-failure','-j','3',
                                      '--output-junit',WORK/'ctest-final.xml'])]:
        result=subprocess.run([str(x) for x in argv],cwd=ROOT,env=env,capture_output=True,
                              encoding='utf-8',errors='replace',timeout=1200)
        (WORK/(name+'.log')).write_text(result.stdout+result.stderr,encoding='utf-8')
        commands.append(dict(argv=[str(x) for x in argv],exit_code=result.returncode))
        if result.returncode:
            failure=name+' failed; see .work/k1/'+name+'.log'
            break
    tests=[]
    xml=ET.parse(WORK/'ctest-final.xml').getroot() if (WORK/'ctest-final.xml').exists() else ET.Element('testsuite')
    for t in xml.iter('testcase'):
        tests.append(dict(name=t.attrib['name'],seconds=float(t.attrib.get('time',0)),
                          passed=t.find('failure') is None and t.find('skipped') is None and t.attrib.get('status')!='notrun',
                          output=t.findtext('system-out','')))
    required={'k1_port_index_roundtrip','k1_electrical_acceptance','k1_real_tool_export','k1_real_tool_gui_cst'}
    passed=not failure and bool(tests) and all(t['passed'] for t in tests) and required.issubset({t['name'] for t in tests})
    profile=json.loads((ROOT/'registry/electrical/tang-nano-9k.6202.reviewed.v1.json').read_text(encoding='utf-8'))
    production_files=[*sorted((ROOT/'core/src/eda-virtual-board').glob('*.*')),
                      ROOT/'include/eda/api/port_manifest.hpp',ROOT/'include/eda/api/electrical_profile.hpp',
                      ROOT/'InnerPlugin/eda-cst-gowin-cst/src/ConstraintSheetV2.cpp',
                      ROOT/'main/fpga/FpgaConstraint.cpp',ROOT/'main/fpga/FpgaPinBindingPanel.cpp']
    hashes={p.relative_to(ROOT).as_posix():sha(p) for p in production_files if p.is_file()}
    report=dict(stage='K1-implementation',g1_closed=passed,A2=passed,A3=passed,
                status='passed' if passed else 'failed',failure=failure,commands=commands,tests=tests,
                test_total=len(tests),test_passed=sum(t['passed'] for t in tests),
                gui_verification='Real FpgaConstraint.cpp and wxBase adapter in C++ CTest; interactive window clicks not tested.',
                actual_tool_fixture='tests/virtual/k1/ports.sv; 17-bit structural roundtrip, separate real 11-pad electrical acceptance.',
                executable=dict(path='build-m0/sigflow.exe',sha256=sha(ROOT/'build-m0/sigflow.exe')),
                builder_executable_sha256=sha(ROOT/'build-m0/sigflow_port_manifest.exe'),
                production_source_sha256=hashes,
                source_snapshot_sha256=hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest(),
                production_electrical=dict(synthetic=profile['synthetic'],reviewed_pads=[p['pin'] for p in profile['pads'] if p['reviewed']],
                                           banks=profile['banks'],provenance=profile['provenance'],
                                           policy=profile['policy']),
                intentional_baseline_corrections=['JTAG TCK is pad 6 (not 56); source-reviewed M0 aliases replace 18 legacy incorrect/unverified resources.'],
                execution_environment='Windows x64, GCC 14.2 ucrt-posix-seh; CTest outside restricted sandbox because CREATE_NO_WINDOW cmd failed with 0xC0000142 inside it.',
                limitations=['Plain source export entry is explicit CLI; missing/stale sidecar blocks GUI instead of guessing.',
                             'Internal pull modes/inout/other IO standards are outside the reviewed M0 electrical subset.',
                             'K1 does not assert completion of VirtualSession, LiveTrace, board rendering or full M0.'])
    for name in ['port-index-cases.json','electrical-cases.synthetic.json']:
        p=ROOT/'tests/virtual/k1/fixtures'/name
        report.setdefault('fixtures',{})[name]=dict(sha256=sha(p),cases=len(json.loads(p.read_text(encoding='utf-8'))['cases']))
    (EVIDENCE/'K1-implementation-summary.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    for name in ['ctest-final.xml','ctest-final.log','build-final.log']:
        if (WORK/name).exists():(EVIDENCE/('K1-'+name)).write_bytes((WORK/name).read_bytes())
    print(f"K1: {report['test_passed']}/{len(tests)} tests; G1/A2/A3 {'CLOSED' if passed else 'OPEN'}")
    if failure:raise RuntimeError(failure)
if __name__=='__main__':main()
