"""Shadow wrapper + actual Verilated ILA/UART link; no student file is edited."""
import argparse,hashlib,json,os,re,shutil,subprocess,sys,time
from pathlib import Path
from build_virtual_project import read,write,sha
ROOT=Path(__file__).resolve().parents[2]
def ascii_comments(text):
    # The locked Windows debug frontend asserts on signed UTF-8 comment chars.
    # Strings and all non-comment tokens are preserved byte-for-byte.
    pattern=r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*[\s\S]*?\*/'
    def comment(match):
        value=match.group();return ''.join(c if ord(c)<128 else ' ' for c in value) if value.startswith(('/',)) else value
    output=re.sub(pattern,comment,text)
    if any(ord(c)>127 for c in output):raise ValueError('E_TRACE_NON_ASCII_TOKEN')
    return output
def build(request):
    project=read(request['project']);project_root=Path(request['project']).resolve().parent;build=Path(request.get('build_dir',ROOT/'build-m0')).resolve();out=Path(request['output']).resolve();out.mkdir(parents=True,exist_ok=True)
    files=[(project_root/p).resolve() for p in project['source_files']];top=project['top_module'];hz=int(project['clock']['frequency_hz']);baud=int(request.get('baud',921600));depth=int(request.get('depth',16))
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_$]*',top) or depth<4 or depth>1024 or depth&(depth-1) or baud<=0 or hz//baud<4 or not 1<=int(request.get('decimation',1))<=255 or int(request.get('pretrigger',0))!=0:raise ValueError('E_TRACE_OVERLAY_CONFIG')
    compiler=Path('E:/sigflow_fpga/toolchain/mingw64/bin/g++.exe');verilator=ROOT/'tools/verilator/verilator-install/bin/verilator_bin_dbg.exe';env=dict(os.environ);env['PATH']=str(compiler.parent)+';'+env.get('PATH','');env['VERILATOR_ROOT']=verilator.parent.parent.as_posix();commands=[]
    shell=Path('E:/sigflow_fpga/toolchain/posix/sh.exe');posix=out/'posix';posix.mkdir(exist_ok=True);shutil.copyfile(shell.parent/'busybox64.exe',posix/'uname.exe');env['PATH']=';'.join([str(compiler.parent),str(shell.parent),str(posix),env['PATH']]);env['SIGFLOW_PYTHON3']=sys.executable;env['SIGFLOW_SH']=str(shell)
    def run(args,timeout=300):
        args=[x.as_posix() if isinstance(x,Path) else str(x) for x in args]
        started=time.monotonic();log=out/('command-'+str(len(commands)+1)+'.log')
        with log.open('w',encoding='utf-8') as stream:r=subprocess.run([str(x) for x in args],cwd=ROOT,env=env,stdin=subprocess.DEVNULL,stdout=stream,stderr=subprocess.STDOUT,timeout=timeout)
        commands.append(dict(argv=[str(x) for x in args],exit_code=r.returncode,seconds=round(time.monotonic()-started,3)))
        if r.returncode:raise RuntimeError(log.read_text(encoding='utf-8',errors='replace')[-4000:])
        return r
    source_hash={str(p):sha(p) for p in files};ports=out/'ports.json';run([sys.executable,ROOT/'tools/virtual/export_ports.py','--top',top,'--output',ports,'--builder',build/'sigflow_port_manifest.exe',*files]);manifest=read(str(ports)+'.ports.v2.json')['manifest']
    # Port declarations are from the production declaration sidecar, never guessed widths.
    declarations=[];connections=[];outputs={};clock=project['clock']['port_ref'];reset=request.get('reset_port','reset_n');constants=request.get('input_constants',{})
    for p in manifest['ports']:
        name=p['rtl_name'];width=p['width'];shape=p.get('shape','scalar');left=p.get('decl_left');right=p.get('decl_right');span='' if width==1 and left is None else '['+str(left)+':'+str(right)+'] '
        if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_$]*',name):raise ValueError('E_TRACE_PORT_NAME')
        direction=p['direction'];is_output=direction in ['output',1]
        if is_output:declarations.append('wire '+span+'tap_'+name+';');connections.append('.'+name+'(tap_'+name+')');outputs[name]=width
        elif name==clock:connections.append('.'+name+'(tb_clk)')
        elif name==reset:connections.append('.'+name+'(tb_rst_n)')
        else:
            value=int(constants.get(name,0));connections.append('.'+name+'('+str(width)+"'d"+str(value)+')')
    selected=request.get('probes') or [p['rtl_name']+('['+str(bit['hdl_index'])+']' if bit.get('hdl_index') is not None else '') for p in manifest['ports'] if p['rtl_name'] in outputs for bit in p['bits']][:32]
    if not selected or len(selected)>32:raise ValueError('E_TRACE_PROBES')
    terms=[]
    for name in selected:
        if not any(manifest['top_module']==top and (b.get('hdl_index') is None and name==p['rtl_name'] or b.get('hdl_index') is not None and name==p['rtl_name']+'['+str(b['hdl_index'])+']') for p in manifest['ports'] if p['rtl_name'] in outputs for b in p['bits']):raise ValueError('E_TRACE_PROBE_REF: '+name)
        terms.append('tap_'+name)
    packed="{"+str(32-len(terms))+"'b0,"+','.join(reversed(terms))+"}" if len(terms)<32 else '{'+','.join(reversed(terms))+'}'
    identity=hashlib.sha256(json.dumps(dict(sources=source_hash,probes=selected,hz=hz,depth=depth,rtl={p.name:sha(p) for p in (ROOT/'rtl/debug').glob('*.sv')}),sort_keys=True).encode()).hexdigest();fingerprint=int(identity[-16:],16)
    wrapper=out/'overlay.sv';wrapper.write_text('module p2_trace_overlay(input wire tb_clk,tb_rst_n,tb_rx,output wire tb_tx,tb_take,output wire [31:0] tb_probe);\n'+'\n'.join(declarations)+'\n'+top+' dut('+','.join(connections)+');\n'+f'''wire [31:0] probe={packed};
wire arm,reset,busy,done,triggered,en;wire [31:0] mask,value,data;wire [7:0] decimation;wire [2:0] mode;wire [15:0] count;wire [$clog2({depth})-1:0] address,index;
sf_micro_ila #(.WIDTH(32),.DEPTH({depth})) ila(.clk(tb_clk),.rst_n(tb_rst_n & ~reset),.probe(probe),.arm(arm),.trigger_mask(mask),.trigger_value(value),.trigger_mode(mode),.trigger_count(count),.decimation(decimation),.sample_en(en),.hs_valid(1'b0),.hs_ready(1'b1),.busy(busy),.done(done),.triggered(triggered),.rd_addr(address),.rd_data(data),.trigger_index(index));
sf_debug_link #(.CLK_HZ({hz}),.BAUD({baud}),.WIDTH(32),.DEPTH({depth}),.FINGERPRINT64(64'h{fingerprint:016x})) link(.clk(tb_clk),.rst_n(tb_rst_n),.rx(tb_rx),.tx(tb_tx),.arm_pulse(arm),.reset_pulse(reset),.trigger_mask(mask),.trigger_value(value),.decimation(decimation),.trigger_mode(mode),.trigger_count(count),.sample_en(en),.busy(busy),.done(done),.triggered(triggered),.trigger_index(index),.rd_addr(address),.rd_data(data));
assign tb_take=busy && en && ila.sample_tick;assign tb_probe=probe;
endmodule
''',encoding='utf-8')
    cpp=out/'host.cpp';cpp.write_text('''#include "Vp2_trace_overlay.h"
#include "virtual/BitClockTransport.h"
#include "debug/DebugAcquisition.h"
#include "debug/CaptureDecoder.h"
#include <eda/api/Types.h>
#include <fstream>
#include <filesystem>
#include <iostream>
int main(int argc,char** argv){if(argc!=2)return 2;try{Vp2_trace_overlay dut;dut.tb_rst_n=0;dut.tb_rx=1;uint64_t cycles=0;eda::Json observations=eda::Json::array();
auto cycle=[&](bool rx){dut.tb_rx=rx;dut.contextp()->time((cycles*1000000000000ULL+HZ/2)/HZ);dut.tb_clk=0;dut.eval();auto tick=((cycles*2+1)*500000000000ULL+HZ/2)/HZ;if(dut.tb_take)observations.push_back({{"tick",std::to_string(tick)},{"value",dut.tb_probe},{"edge_index",std::to_string(cycles+1)},{"valid_mask",0xffffffffu}});dut.contextp()->time(tick);dut.tb_clk=1;dut.eval();++cycles;return bool(dut.tb_tx);};for(int i=0;i<8;++i)cycle(true);dut.tb_rst_n=1;for(int i=0;i<8;++i)cycle(true);
sigflow::virtual_lab::BitClockTransport transport(cycle,HZ,BAUD);transport.Open();sigflow::debug::DebugContract contract;contract.sampleClock.frequencyHz=HZ;contract.sampleClock.signal="clock_in";contract.capture.depth=DEPTH;contract.capture.decimation=DECIMATION;contract.transport.kind="uart";contract.transport.protocol="full";contract.transport.baud=BAUD;contract.transport.syncEnabled=false;contract.trigger.kind="mask_equal";contract.trigger.mask="0x0";contract.trigger.value="0x0";contract.fingerprints.fingerprint64=FINGERPRINT;
PROBES
sigflow::debug::DebugProtocol protocol(transport);uint8_t ping_version=0;std::string ping_error;if(!protocol.Ping(ping_version,ping_error) || ping_version!=1)throw std::runtime_error("E_VIRTUAL_RTL_PING: "+ping_error);
sigflow::debug::DebugAcquisition acquisition(transport,contract);sigflow::debug::DebugAcquisitionOptions options;options.syncCalibrate=false;options.pollIntervalMs=0;options.autoReconnect=false;std::string error;if(!acquisition.Acquire(argv[1],error,options))throw std::runtime_error(error);const auto& r=acquisition.Result();sigflow::debug::CaptureTimebase tb;tb.sampleClockHz=HZ;tb.decimation=DECIMATION;tb.triggerObserved=true;for(const auto& observation:observations)tb.sampleTicks.push_back(std::stoull(observation.at("tick").get<std::string>()));std::vector<sigflow::debug::CaptureProbe> captureProbes;for(const auto& p:contract.probes)captureProbes.push_back({p.id,p.path,p.width,p.bitOffset});sigflow::debug::CaptureVcdMeta vm;if(!sigflow::debug::DecodeCaptureToVcd(r.samples,r.samples.size(),r.triggerIndex,captureProbes,r.vcdPath,vm,error,tb))throw std::runtime_error(error);eda::Json commands=eda::Json::array();for(const auto& wire:transport.SentFrames()){uint8_t type=0,seq=0;std::vector<uint8_t> data;if(!sigflow::debug::proto::FrameDecode(wire,type,seq,data))throw std::runtime_error("E_HOST_SENT_INVALID_FRAME");commands.push_back({{"type",type},{"sequence",seq},{"wire",wire}});}eda::Json result={{"status","passed"},{"source","virtual_rtl"},{"transport","bit_level_debug_uart"},{"cycles",cycles},{"decoded_uart_bytes",transport.ReceivedBytes()},{"samples",r.samples},{"trigger_index",r.triggerIndex},{"fingerprint64",r.fingerprint64},{"raw",r.rawPath},{"vcd",r.vcdPath},{"protocol_commands",r.protocolStats.commandCalls+1},{"sent_cobs_crc16_frames",commands},{"ping_verified",true}};result["independent_observations"]=observations;std::ofstream(std::filesystem::path(argv[1])/"result.json")<<result.dump(2);std::cout<<result.dump()<<std::endl;dut.final();return 0;}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
'''.replace('HZ',str(hz)).replace('BAUD',str(baud)).replace('DEPTH',str(depth)).replace('DECIMATION',str(int(request.get('decimation',1)))).replace('FINGERPRINT',str(fingerprint)+'ULL').replace('PROBES','\n'.join('contract.probes.push_back({"p'+str(i)+'","'+name+'",1,'+str(i)+',"",false});' for i,name in enumerate(selected))),encoding='utf-8')
    obj=out/'obj';link=read(build/'p2-debug-link.json');link['platform']=(build/'core/libeda_platform.a').as_posix();wx=ROOT/'3rd/wx-mingw';flags=['-std=c++20','-DVL_TIME_CONTEXT','-DWIN32','-DWXUSINGDLL','-D__WXMSW__','-DUNICODE','-D_UNICODE','-I'+str(ROOT/'main'),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'3rd'),'-I'+str(wx/'include'),'-I'+str(wx/'lib/wx/include/msw-unicode-3.2')];flags=[f.replace('\\','/') for f in flags]
    shadow=out/'shadow-hdl';shadow.mkdir(exist_ok=True);rtl_sources=[*files,ROOT/'rtl/debug/sf_micro_ila.sv',ROOT/'rtl/debug/sf_debug_link.sv',ROOT/'rtl/debug/sf_uart_link.sv'];shadow_files=[]
    for index,source in enumerate(rtl_sources):
        target=shadow/(str(index)+'-'+source.name);target.write_text(ascii_comments(source.read_text(encoding='utf-8-sig')),encoding='ascii');shadow_files.append(target)
    run([verilator,'--cc',wrapper,*shadow_files,'--exe',cpp,'-Wno-fatal','--top-module','p2_trace_overlay','--Mdir',obj,'-CFLAGS',' '.join(flags),'-LDFLAGS',' '.join(str(link[k]).replace('\\','/') for k in ['library','platform','wx_core','wx_base'])],600)
    shell=Path('E:/sigflow_fpga/toolchain/posix/sh.exe');env['PYTHON3']=sys.executable;env['MAKEFLAGS']='-e';env['CXX']=env['LINK']=str(compiler).replace('\\','/');env['AR']=str(compiler.parent/'ar.exe').replace('\\','/');env['SHELL']=str(shell).replace('\\','/');env['PATH']=str(shell.parent)+';'+env['PATH'];run([compiler.parent/'mingw32-make.exe','-C',obj,'-f','Vp2_trace_overlay.mk','-j4','CXX='+env['CXX'],'LINK='+env['LINK'],'AR='+env['AR'],'SHELL='+env['SHELL'],'PYTHON3='+sys.executable.replace('\\','/')],600)
    exe=obj/'Vp2_trace_overlay.exe';capture=out/'capture';capture.mkdir(exist_ok=True);env['PATH']=str(build)+';'+env['PATH'];run([exe,capture],180)
    result=read(capture/'result.json');result.update(commands=commands,source_sha256=source_hash,source_unchanged=all(sha(p)==h for p,h in source_hash.items()),rtl_original_sha256={p.name:sha(p) for p in rtl_sources},comment_normalization='UTF-8 characters in comments replaced with ASCII spaces in shadow files only; all non-comment tokens unchanged',overlay=str(wrapper),fingerprint=identity,scope='Shadow wrapper with actual sf_micro_ila + sf_debug_link + sf_uart_link. Dedicated virtual UART, not board Pad17/18 transport validation. RTL finite linear capture rejects pretrigger.')
    observed=result['independent_observations']
    if [s['value'] for s in observed]!=result['samples']:raise RuntimeError('E_RTL_CAPTURE_VS_INDEPENDENT_PROBE')
    for s in observed:s['valid_mask']=(1<<len(selected))-1
    result['capture_snapshot']=dict(schema='sigflow.virtual-capture.v1',source='virtual_rtl',layout='finite_linear_rtl',sample_phase='pre_eval',armed=False,triggered=True,trigger_index=result['trigger_index'],decimation=int(request.get('decimation',1)),sample_clock_hz=str(hz),clock_pad=int(project['clock']['package_pin']),source_fingerprint=manifest['source_sha256'],targets=['port:'+n for n in selected],samples=observed)
    result['independent_reference']=dict(result['capture_snapshot'],source='independent_wrapper_probe',samples=[dict(s) for s in observed])
    write(capture/'capture.extended.json',result['capture_snapshot']);return result
def main():
    p=argparse.ArgumentParser();p.add_argument('--request',type=Path,required=True);p.add_argument('--result',type=Path,required=True);a=p.parse_args()
    try:r=build(read(a.request))
    except Exception as e:write(a.result,dict(status='failed',error=str(e)));raise
    write(a.result,r)
if __name__=='__main__':main()
