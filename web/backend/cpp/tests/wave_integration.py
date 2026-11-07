"""Production wave CLI, paging and precision checks."""
import json, subprocess, sys, tempfile
from pathlib import Path
binary, build = sys.argv[1:]
with tempfile.TemporaryDirectory(dir=build,prefix='wave-') as d:
    root=Path(d);vcd=root/'wave.vcd'
    vcd.write_text('$timescale 1 ns $end\n$scope module top $end\n$var wire 1 ! clk $end\n$var wire 1 ! alias $end\n$upscope $end\n$enddefinitions $end\n$dumpvars\n0!\n$end\n#9007199254740993\n1!\n#9007199254740994\n0!\n')
    def call(*args,code=0):
        r=subprocess.run([binary,'wave',*map(str,args)],capture_output=True,text=True,timeout=8)
        assert r.returncode==code,(args,r.stdout,r.stderr)
        return json.loads(r.stdout)
    first=call('signals','--vcd',vcd,'--limit',1)['data']
    assert first['timescale']=='1ns' and first['time_range']['end']=='9007199254740994'
    assert len(first['signals'])==1 and first['next_offset']==1
    second=call('signals','--vcd',vcd,'--limit',1,'--offset',1)['data']
    assert second['signals'][0]['name']=='alias' and second['next_offset'] is None
    params=['query','--vcd',vcd,'--signals','0','--begin','0','--end','9007199254740994','--limit','2']
    one=call(*params)['data'];assert [x['time'] for x in one['transitions']]==['0','9007199254740993']
    assert one['initial_value']=='0' and one['next_offset']==2
    two=call(*params,'--offset',2)['data'];assert two['transitions'][0]['time']=='9007199254740994' and two['next_offset'] is None
    for p in [('query','--vcd',vcd,'--signals','8','--begin','0','--end','1'),
              ('query','--vcd',vcd,'--signals','0','--begin','3','--end','2'),
              ('signals','--vcd',vcd,'--limit','0'),('signals','--vcd',vcd,'--limit','1001')]:call(*p,code=4)
    requests = [
        {'id':'open-1','op':'open','vcd':str(vcd)},
        {'id':'open-bad','op':'open','vcd':str(root/'missing.vcd')},
        {'id':'signals-1','op':'signals','limit':1},
        {'id':'query-1','op':'query','signal_id':0,'begin':'0','end':'9007199254740994','limit':2},
    ]
    stream=''.join(json.dumps(request)+'\n' for request in requests)
    stream+='{malformed json}\n'
    stream+=json.dumps({'id':'close-1','op':'close'})+'\n'
    stream+=json.dumps({'id':'closed-1','op':'signals'})+'\n'
    served=subprocess.run([binary,'wave','serve'],input=stream,capture_output=True,text=True,timeout=8)
    assert served.returncode==0,served.stderr
    events=[json.loads(line) for line in served.stdout.splitlines()]
    assert len(events)==7
    assert events[0]['id']=='open-1' and events[0]['ok'] is True
    assert events[1]['id']=='open-bad' and events[1]['ok'] is False
    assert events[2]['id']=='signals-1' and events[2]['data']['signals'][0]['name']=='clk'
    assert events[3]['id']=='query-1' and events[3]['data']['transitions'][0]['time']=='0'
    assert events[4]['ok'] is False and events[4]['error']['code']=='INVALID_JSON'
    assert events[5]['id']=='close-1' and events[5]['ok'] is True
    assert events[6]['id']=='closed-1' and events[6]['ok'] is False
    too_long='x'*(64*1024+1)+'\n'+json.dumps({'id':'recovered','op':'open','vcd':str(vcd)})+'\n'
    bounded=subprocess.run([binary,'wave','serve'],input=too_long,capture_output=True,text=True,timeout=8)
    bounded_events=[json.loads(line) for line in bounded.stdout.splitlines()]
    assert bounded.returncode==0 and len(bounded_events)==2
    assert bounded_events[0]['error']['code']=='REQUEST_TOO_LARGE'
    assert bounded_events[1]['id']=='recovered' and bounded_events[1]['ok'] is True
    link=root/'link.vcd';link.symlink_to(vcd);call('signals','--vcd',link,code=4)
    vcd.write_text('$enddefinitions $end\n#2\n1!\n');call('signals','--vcd',vcd,code=4)
print('wave CLI integration passed')
