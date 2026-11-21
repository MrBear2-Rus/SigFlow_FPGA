"""Stable desktop + 47 CTests; append P4 archives without rewriting M0/P1/P2/P3."""
import gzip,hashlib,json,os,shutil,subprocess,sys,time,xml.etree.ElementTree as ET
from datetime import datetime
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'.work/p4/final';OUT=ROOT/'docs/virtual/evidence';P4=OUT/'P4'
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
    """Freeze the M0/P1/P2 ledger plus every already-archived P3 artifact byte."""
    frozen={}
    reference=ROOT/'.work/p3/historical-sha256.json'
    if reference.exists():frozen.update(read(reference))
    for base in [OUT/'P3',OUT/'P3-acceptance-summary.json']:
        if base.is_file():frozen[base.relative_to(ROOT).as_posix()]=sha(base)
        elif base.is_dir():
            for p in base.rglob('*'):
                if p.is_file():frozen[p.relative_to(ROOT).as_posix()]=sha(p)
    return frozen
def main():
    sys.stdout.reconfigure(encoding='utf-8',errors='replace');WORK.mkdir(parents=True,exist_ok=True);P4.mkdir(parents=True,exist_ok=True)
    reference=ROOT/'.work/p4/historical-sha256.json'
    if not reference.exists():
        frozen=snapshot_historical();write(reference,frozen)
        print(f'froze {len(frozen)} historical evidence files into {reference}')
    protected=read(reference)
    if len(protected)<175:raise RuntimeError(f'historical snapshot incomplete: {len(protected)}')
    started=time.time();before=sources();original=read(OUT/'P3-acceptance-summary.json');old={t['name'] for t in original['tests']}
    additions={'p4_i2c_spi_oled_and_bus_faults','p4_crc_tamper_and_cdc_approx','p4_gui_oled_keypad_and_course05_acceptance'}
    if len(old)!=44:raise RuntimeError('Expected 44 original CTests')
    unchanged=lambda:all((ROOT/p).is_file() and sha(ROOT/p)==digest for p,digest in protected.items())
    if not unchanged():raise RuntimeError('Historical evidence changed before validation')
    env=dict(os.environ);env['PATH']='E:/sigflow_fpga/toolchain/mingw64/bin;'+str(ROOT/'build-m0')+';'+env.get('PATH','');temp=WORK/'temp'/str(time.time_ns());temp.mkdir(parents=True,exist_ok=True);env['TMP']=env['TEMP']=str(temp)
    cmake=ROOT/'.work/cmake-python/cmake/data/bin/cmake.exe';ctest=cmake.with_name('ctest.exe');build=ROOT/'build-m0';xml=WORK/'ctest.xml';xml.unlink(missing_ok=True);commands=[]
    for name,args in [('build',[cmake,'--build',build,'--parallel','4']),('ctest',[ctest,'--test-dir',build,'--output-on-failure','-j','1','--output-junit',xml])]:
        print('Running P4 final '+name,flush=True);begin=time.monotonic()
        with (WORK/(name+'.log')).open('w',encoding='utf-8') as stream:r=subprocess.run([str(a) for a in args],cwd=ROOT,env=env,stdout=stream,stderr=subprocess.STDOUT,timeout=7200)
        commands.append(dict(argv=[str(a) for a in args],exit_code=r.returncode,seconds=round(time.monotonic()-begin,3)))
        if r.returncode:print((WORK/(name+'.log')).read_text(encoding='utf-8',errors='replace')[-12000:]);break
    tests=[]
    if xml.exists():
        for t in ET.parse(xml).getroot().iter('testcase'):tests.append(dict(name=t.attrib['name'],passed=t.find('failure') is None and t.find('skipped') is None and t.attrib.get('status')!='notrun',seconds=float(t.attrib.get('time',0))))
    reports={name:read(ROOT/'.work/p4'/path) if (ROOT/'.work/p4'/path).exists() else {} for name,path in [('bus','bus/bus-results.json'),('protocols','protocols/protocols-results.json'),('gui','gui/gui-results.json')]}
    native=read(ROOT/'.work/p4/bus/native.json') if (ROOT/'.work/p4/bus/native.json').exists() else {}
    stable=before==sources();preserved=unchanged();byname={t['name'] for t in tests}
    good=len(commands)==2 and all(c['exit_code']==0 for c in commands) and len(tests)==47 and byname==old|additions and all(t['passed'] for t in tests) and stable and preserved and native.get('status')=='passed' and all(r.get('status')=='passed' for r in reports.values())
    artifacts={}
    def copy(source,name=None,compress=False):
        source=Path(source)
        if not source.is_file():return
        target=P4/(name or source.name);target.parent.mkdir(parents=True,exist_ok=True)
        if compress:target.write_bytes(gzip.compress(source.read_bytes(),mtime=0))
        else:shutil.copyfile(source,target)
        artifacts[target.relative_to(P4).as_posix()]=dict(sha256=sha(target),bytes=target.stat().st_size,**(dict(uncompressed_sha256=sha(source),raw_bytes=source.stat().st_size) if compress else {}))
    for name in ['build.log','ctest.log','ctest.xml']:copy(WORK/name)
    for name,path in [('bus-results.json','bus/bus-results.json'),('protocols-results.json','protocols/protocols-results.json'),('gui-results.json','gui/gui-results.json')]:copy(ROOT/'.work/p4'/path,name)
    copy(ROOT/'.work/p4/bus/native.json','native.json')
    for mode in ['bus','protocols','gui']:copy(ROOT/'.work/p4'/mode/'progress.log',mode+'-progress.log')
    for name in ['p4-trajectory.json','p4-frames.sfvb','p4-frames.json','p4-decoded.json','p4-js-decoded.json','sfvb-encode-summary.json']:
        copy(ROOT/'.work/p4/protocols'/name,'protocols/'+name)
    for name in ['board-oled-keypad-light.png','board-oled-keypad-dark.png','course05-evidence.json','course05-blind-frame.json']:
        copy(ROOT/'.work/p4/gui'/name,'gui/'+name)
    for scenario in ['bus','protocols','gui']:
        base=ROOT/'.work/p4'/scenario
        if not base.is_dir():continue
        for p in base.rglob('private.journal.jsonl'):
            if p.stat().st_mtime>=started:copy(p,'journals/'+scenario+'-'+p.parent.parent.name+'.jsonl.gz',True)
    write(P4/'historical-sha256.json',protected);artifacts['historical-sha256.json']=dict(sha256=sha(P4/'historical-sha256.json'),bytes=(P4/'historical-sha256.json').stat().st_size)
    limits=['Digital nominal voltage/state models; no SPICE current or analog metastability','I2C SDA is an open-drain 13-state bus: 0 drives SLO, 1 releases to FLT and the board pull-up synthesizes WHI','SPI OLED is an SSD1306 teaching subset (page addressing, 0xAE/0xAF/0xA6/0xA7/0xB0..0xB7, column nibbles); no DMA/charge-pump modeling','crc_tamper only flips the upper-layer crc8-0x07 byte; bottom-layer 8N1 framing and baud stay legal by construction','cdc_sampling_approx is a functional_metastability_approx: seeded whole-cycle sampling branches, never analog metastability or MTBF','Matrix keypad ghosting follows the digital junction model; diode mode conducts only the scanned row','Course 05 grading runs on the trusted side with three hidden seeds; local blindness does not protect against a machine owner','Physical multi-monitor and real-hardware I2C/SPI electrical timing are out of scope','Web remains deferred; SFVB frames are consumed by the Node reference decoder']
    result=dict(schema='sigflow.phase4-acceptance.v1',date=datetime.now().astimezone().isoformat(),status='passed' if good else 'failed',Phase4_complete=bool(good),base_commit='d7ba89d',branch=subprocess.check_output(['git','branch','--show-current'],cwd=ROOT,text=True).strip(),test_total=len(tests),test_passed=sum(t['passed'] for t in tests),original_tests=44,new_tests=3,tests=tests,commands=commands,source_unchanged_during_validation=stable,source_sha256=before,historical_archive_unchanged=preserved,historical_count=len(protected),historical_sha256=protected,native=native,bus=reports['bus'],protocols=reports['protocols'],gui=reports['gui'],desktop_binary=dict(path=str(build/'sigflow.exe'),sha256=sha(build/'sigflow.exe')),artifacts=artifacts,scope_limits=limits)
    write(OUT/'P4-acceptance-summary.json',result)
    lines=['# Phase 4 高级总线外设与协议时序故障实测报告','',f'验收时间：{result["date"]}；基线 `d7ba89d`；分支 `{result["branch"]}`。','',f'最终状态：**{result["status"]}**，`Phase4_complete={str(good).lower()}`。','', '| 验收 | 实测结果 |','|---|---|',f'| 全部 CTest | {result["test_passed"]}/{len(tests)}；原 44 项 + 新 3 项 |',f'| I2C/SPI-OLED/总线故障 | {reports["bus"].get("check_count",0)} 项真实 RTL 断言；开漏 SDA、ACK/NAK、VRAM 摘要 |',f'| CRC/CDC/SFVB | {reports["protocols"].get("check_count",0)} 项；上层 CRC 篡改、功能级 CDC 近似、C++↔JS 一致 |',f'| GUI 与第 5 门课 | {reports["gui"].get("check_count",0)} 项真实 wxApp 断言；键盘点击、OLED 点阵、course-05 三种子判题 |',f'| 历史归档 | {len(protected)} 份 SHA-256 保持：{preserved} |',f'| 参与验收的源码 | 构建前后逐文件哈希一致：{stable} |','','## 实现与证据','','| 模块 | 生产入口 / 证据 |','|---|---|','| I2C 开漏传感器 | Phase1Peripherals::I2c + BoardTopology junction；P4/bus-results.json |','| SPI OLED | Phase1Peripherals::Oled（VRAM + frame_sha256）；P4/bus-results.json |','| 总线/协议/CDC 故障 | Phase4Faults（i2c_nak、spi_mode_fault、crc_tamper、cdc_sampling_approx） |','| SFVB 跨端 | BinaryFrameCodec + sfvb_reference_decoder.js；P4/protocols/*.sfvb |','| GUI | BoardRenderer 键盘/OLED/I2C + VirtualBoardPanel 点击；P4/gui/*.png |','| 第 5 门盲测课 | generate_phase4_assets.py 签名 course-05；P4/gui/course05-evidence.json |','','原生代数测试仍使用显式替身；I2C/SPI/CRC/CDC 与 GUI 验收全部编译并运行真实 Verilator DUT 与真实 wxApp。','','## 复现与边界','','执行 `python tools/virtual/verify_phase4_final.py`；三个新模式可单独运行：`python tools/virtual/verify_phase4.py --mode bus|protocols` 与 `python tools/virtual/verify_phase4_gui.py`。','',*['- '+s for s in limits],'','机器可读结论和逐文件哈希见 [P4-acceptance-summary.json](evidence/P4-acceptance-summary.json)。原 M0/P1/P2/P3 报告及归档保持原字节。']
    (ROOT/'docs/virtual/Phase4高级总线外设与协议时序故障实测报告.md').write_text('\n'.join(lines),encoding='utf-8')
    print(f'P4={good}; {result["test_passed"]}/{len(tests)} CTest; historical={preserved}; source stable={stable}');return 0 if good else 1
if __name__=='__main__':raise SystemExit(main())
