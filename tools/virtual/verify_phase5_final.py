"""Full-platform v1.0 final acceptance + 50 CTests; archive P5 without touching M0..P4."""
import gzip,hashlib,json,os,shutil,subprocess,sys,time,xml.etree.ElementTree as ET
from datetime import datetime
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'.work/p5/final';OUT=ROOT/'docs/virtual/evidence';P5=OUT/'P5'

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def read(p):return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def write(p,j):Path(p).write_text(json.dumps(j,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

def sources():
    paths=set()
    for folder in ['core/src','include','main','tests','tools/virtual','registry','examples/virtual','rtl/debug','agent/src','GUI','InnerPlugin/eda-sim-verilator','InnerPlugin/eda-cst-gowin-cst']:
        paths.update(p for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in {'.cpp','.h','.hpp','.inc','.cmake','.txt','.py','.js','.sv','.v','.cst','.json','.pub','.html','.md'} and not {'__pycache__','bin','CMakeFiles','node_modules'}&set(p.parts))
    paths.update(ROOT/p for p in ['CMakeLists.txt','core/CMakeLists.txt','tests/CMakeLists.txt','.gitattributes','.gitignore'])
    return {p.relative_to(ROOT).as_posix():sha(p) for p in sorted(paths)}

def snapshot_historical():
    """Freeze the M0..P3 ledger (176) plus every P4 archived file (22 + summary = 23) = 199 files."""
    frozen={}
    ref=OUT/'P4/historical-sha256.json'
    if not ref.is_file():ref=ROOT/'.work/p4/historical-sha256.json'
    if ref.is_file():frozen.update(read(ref))
    for base in [OUT/'P4',OUT/'P4-acceptance-summary.json']:
        if base.is_file():frozen[base.relative_to(ROOT).as_posix()]=sha(base)
        elif base.is_dir():
            for p in base.rglob('*'):
                if p.is_file():frozen[p.relative_to(ROOT).as_posix()]=sha(p)
    return frozen

def main():
    sys.stdout.reconfigure(encoding='utf-8',errors='replace');WORK.mkdir(parents=True,exist_ok=True);P5.mkdir(parents=True,exist_ok=True)
    reference=ROOT/'.work/p5/historical-sha256.json'
    if not reference.exists():
        frozen=snapshot_historical();write(reference,frozen)
        print(f'froze {len(frozen)} historical evidence files into {reference}')
    protected=read(reference)
    if len(protected)<198:raise RuntimeError(f'historical snapshot incomplete: {len(protected)}')
    started=time.time();before=sources();original=read(OUT/'P4-acceptance-summary.json');old={t['name'] for t in original['tests']}
    additions={'p5_i2c_multibyte_burst_and_recovery','p5_sfvb_web_workbench_parity','p5_gui_full_platform_showcase_acceptance'}
    if len(old)!=47:raise RuntimeError('Expected 47 original CTests')
    unchanged=lambda:all((ROOT/p).is_file() and sha(ROOT/p)==digest for p,digest in protected.items())
    if not unchanged():raise RuntimeError('Historical evidence changed before validation')

    env=dict(os.environ);env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+str(ROOT/'build-m0')+';'+env.get('PATH','')
    temp=WORK/'temp'/str(time.time_ns());temp.mkdir(parents=True,exist_ok=True);env['TMP']=env['TEMP']=str(temp)
    cmake=ROOT/'.work/cmake-python/cmake/data/bin/cmake.exe';ctest=cmake.with_name('ctest.exe');build=ROOT/'build-m0';xml=WORK/'ctest.xml';xml.unlink(missing_ok=True);commands=[]
    for name,args in [('build',[cmake,'--build',build,'--parallel','4']),('ctest',[ctest,'--test-dir',build,'--output-on-failure','-j','1','--output-junit',xml])]:
        print('Running P5 final '+name,flush=True);begin=time.monotonic()
        with (WORK/(name+'.log')).open('w',encoding='utf-8') as stream:
            r=subprocess.run([str(a) for a in args],cwd=ROOT,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=7200)
        commands.append(dict(argv=[str(a) for a in args],exit_code=r.returncode,seconds=round(time.monotonic()-begin,3)))
        if r.returncode:
            print((WORK/(name+'.log')).read_text(encoding='utf-8',errors='replace')[-12000:]);break

    tests=[]
    if xml.exists():
        for t in ET.parse(xml).getroot().iter('testcase'):
            tests.append(dict(name=t.attrib['name'],passed=t.find('failure') is None and t.find('skipped') is None and t.attrib.get('status')!='notrun',seconds=float(t.attrib.get('time',0))))

    reports={name:read(ROOT/'.work/p5'/path) if (ROOT/'.work/p5'/path).exists() else {}
        for name,path in [('i2c_burst','i2c_burst/i2c_burst-results.json'),('web_parity','web_parity/web_parity-results.json'),('gui','gui/gui-results.json')]}
    stable=before==sources();preserved=unchanged();byname={t['name'] for t in tests}
    good=(len(commands)==2 and all(c['exit_code']==0 for c in commands) and len(tests)==50
        and byname==old|additions and all(t['passed'] for t in tests) and stable and preserved
        and all(r.get('status')=='passed' for r in reports.values()))

    artifacts={}
    def copy(source,name=None,compress=False):
        source=Path(source)
        if not source.is_file():return
        target=P5/(name or source.name);target.parent.mkdir(parents=True,exist_ok=True)
        if compress:target.write_bytes(gzip.compress(source.read_bytes(),mtime=0))
        else:shutil.copyfile(source,target)
        artifacts[target.relative_to(P5).as_posix()]=dict(sha256=sha(target),bytes=target.stat().st_size,
            **(dict(uncompressed_sha256=sha(source),raw_bytes=source.stat().st_size) if compress else {}))

    for name in ['build.log','ctest.log','ctest.xml']:copy(WORK/name)
    for name,path in [('i2c_burst-results.json','i2c_burst/i2c_burst-results.json'),('web_parity-results.json','web_parity/web_parity-results.json'),('gui-results.json','gui/gui-results.json')]:
        copy(ROOT/'.work/p5'/path,name)
    for mode in ['i2c_burst','web_parity','gui']:copy(ROOT/'.work/p5'/mode/'progress.log',mode+'-progress.log')
    for sfvb in (ROOT/'.work/p5/web_parity').glob('*.sfvb'):copy(sfvb,'web_parity/'+sfvb.name)
    copy(ROOT/'.work/p5/web_parity/web-projector-selftest.json','web_parity/web-projector-selftest.json')
    for name in ['full-showcase-light.png','full-showcase-dark.png','float-showcase.png']:copy(ROOT/'.work/p5/gui'/name,'gui/'+name)
    for scenario in ['i2c_burst','web_parity']:
        base=ROOT/'.work/p5'/scenario
        if not base.is_dir():continue
        for p in base.rglob('private.journal.jsonl'):
            if p.stat().st_mtime>=started:copy(p,'journals/'+scenario+'-'+p.parent.parent.name+'.jsonl.gz',True)

    write(P5/'historical-sha256.json',protected)
    artifacts['historical-sha256.json']=dict(sha256=sha(P5/'historical-sha256.json'),bytes=(P5/'historical-sha256.json').stat().st_size)

    limits=[
        'Digital nominal voltage/state models; no SPICE current or analog metastability',
        'I2C SDA is an open-drain 13-state bus: 0 drives SLO, 1 releases to FLT and the board pull-up synthesizes WHI',
        'SPI OLED is an SSD1306 teaching subset; no analog charge-pump or DMA timing',
        'Web SFVB player (GUI/virtual-sfvb-player.html + sfvb_web_projector.js) is an offline same-source binary stream projector and inspector, not a second Web simulation backend',
        'Local blind courses provide educational isolation, not hardware-enforced DRM against machine owners',
        'Physical multi-monitor hot-plug and real-hardware electrical timing remain out of scope'
    ]
    result=dict(
        schema='sigflow.phase5-acceptance.v1',
        date=datetime.now().astimezone().isoformat(),
        status='passed' if good else 'failed',
        Phase5_complete=bool(good),
        Full_Platform_v1_complete=bool(good),
        base_commit='ec24b72',
        branch=subprocess.check_output(['git','branch','--show-current'],cwd=ROOT,text=True).strip(),
        test_total=len(tests),
        test_passed=sum(t['passed'] for t in tests),
        original_tests=47,
        new_tests=3,
        tests=tests,
        commands=commands,
        source_unchanged_during_validation=stable,
        source_sha256=before,
        historical_archive_unchanged=preserved,
        historical_count=len(protected),
        historical_sha256=protected,
        i2c_burst=reports['i2c_burst'],
        web_parity=reports['web_parity'],
        gui=reports['gui'],
        desktop_binary=dict(path=str(build/'sigflow.exe'),sha256=sha(build/'sigflow.exe')),
        artifacts=artifacts,
        scope_limits=limits
    )
    write(OUT/'P5-acceptance-summary.json',result)

    lines=[
        '# Phase 5 全平台收官与跨端同源实测总报告',
        '',
        f'验收时间：{result["date"]}；基线 `ec24b72`；分支 `{result["branch"]}`。',
        '',
        f'最终状态：**{result["status"]}**，`Phase5_complete={str(good).lower()}`，`Full_Platform_v1_complete={str(good).lower()}`。',
        '',
        '| 验收 | 实测结果 |',
        '|---|---|',
        f'| 全部 CTest | {result["test_passed"]}/{len(tests)}；原 47 项 + 新 3 项 |',
        f'| I2C Burst Read 与零回归 | {reports["i2c_burst"].get("check_count",0)} 项真实 Verilator 断言；3 字节连续读、MSB=1、data/addr NAK 恢复、Phase 4 双 digest 零回归 |',
        f'| 跨端同源 SFVB 与 Web 投影 | {reports["web_parity"].get("check_count",0)} 项；5 条真实工程流 100% 逐帧同源一致 + 断线重同步/幂等 ACK/BigInt 64-bit/零泄密 |',
        f'| GUI 全阶段实验台与诚实渲染 | {reports["gui"].get("check_count",0)} 项真实 wxApp 断言；7 实验一键加载、`P16 → 外接`、`KEY P3 未挂载`、浮动窗交互与 Light/Dark 离屏导出 |',
        f'| 历史归档保护 | {len(protected)} 份历史文件（M0–P4）SHA-256 100% 不变：{preserved} |',
        f'| 参与验收的源码 | 构建与测试前后逐文件哈希一致：{stable} |',
        '',
        '## 实现与证据',
        '',
        '| 模块 | 生产入口 / 证据 |',
        '|---|---|',
        '| 模块 A：I2C Burst Read 修复 | `Phase1Peripherals::I2c`（`MasterAck` `read_bit=1` 与 `AckWriteHold`）+ `examples/virtual/phase5/i2c-burst/`；`P5/i2c_burst-results.json` |',
        '| 模块 B：全阶段 GUI 与诚实引脚渲染 | `VirtualBoardPanel::Examples` / `LoadExample` + `BoardRenderer::LedSlotStatus` / `KeySlotStatus`；`P5/gui/full-showcase-*.png` |',
        '| 模块 C：跨端同源 SFVB 投影与回放器 | `tools/virtual/sfvb_web_projector.js` + `GUI/virtual-sfvb-player.html` + `SPEC §7.5`；`P5/web_parity/*.sfvb` |',
        '| 模块 D：全平台 v1.0 50/50 终验 | `tests/virtual/p5/` + `tools/virtual/verify_phase5_final.py`；`P5-acceptance-summary.json` |',
        '',
        '## 复现与边界',
        '',
        '执行 `python tools/virtual/verify_phase5_final.py`；三项新测试可独立运行：`python tools/virtual/verify_phase5.py --build-dir build-m0 --mode i2c_burst|web_parity` 与 `python tools/virtual/verify_phase5_gui.py --build-dir build-m0 --gui build-m0/tests/virtual/u1/u1_m0_full_product_acceptance.exe`。',
        '',
        *['- '+s for s in limits],
        '',
        '机器可读结论和逐文件哈希见 [P5-acceptance-summary.json](evidence/P5-acceptance-summary.json)。M0–P4 历史报告与归档保持原字节未变。'
    ]
    (ROOT/'docs/virtual/Phase5全平台收官与跨端同源实测总报告.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print(f'P5={good}; Full_Platform_v1={good}; {result["test_passed"]}/{len(tests)} CTest; historical={preserved}; source stable={stable}')
    return 0 if good else 1

if __name__=='__main__':raise SystemExit(main())
