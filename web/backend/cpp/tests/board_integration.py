"""Target profile and constraint validation through the production CLI."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

binary, build, data = sys.argv[1:]
def call(*args, code=0):
    result = subprocess.run([binary,*map(str,args)], text=True, capture_output=True, timeout=8)
    assert result.returncode == code, (args,result.stdout,result.stderr)
    events = [json.loads(x) for x in result.stdout.splitlines()]
    assert len(events)==1
    return events[0]
profile = call('target','show','--id','tang-nano-9k','--data-dir',data)
assert profile['type']=='result' and profile['data']['nextpnr']['family']=='GW1N-9C'
assert profile['data']['yosys']['synthesis_command']
call('target','show','--id','../escape','--data-dir',data,code=2)
call('target','show','--id','missing','--data-dir',data,code=2)
call('target','show','--id','tang-nano-9k','--id','again',code=2)
pins=Path(data)/'target-profiles/tang-nano-9k.pins.json'
with tempfile.TemporaryDirectory(prefix='board-',dir=build) as temp:
    root=Path(temp); cst=root/'pins.cst'
    def check(text, valid):
        cst.write_text(text)
        event=call('validate','cst','--cst',cst,'--pins',pins)
        assert event['data']['valid'] is valid, (text,event)
        return event['data']
    check('IO_LOC "clk" 1;\nIO_PORT "clk" IO_TYPE=LVCMOS33;\n',True)
    check('IO_LOC "clk" 7;\n',False)
    check('IO_LOC "clk" 1;\nIO_LOC "out" 1;\n',False)
    check('IO_LOC "clk" 1;\nIO_LOC "clk" 2;\n',False)
    check('IO_LOC "clk" 999;\n',False)
    check('IO_PORT "clk" DRIVE=8;\n',False)
    check('garbage\n',False)
    check('',False)
    design=root/'bindings.json';netlist=root/'netlist.json'
    netlist.write_text(json.dumps({'modules':{'top':{'ports':{'clk':{'direction':'input','bits':[1]},'led':{'direction':'output','bits':[2,3]}}}}}))
    base={'top_module':'top','device':'GW1NR-LV9QN88PC6/I5','bindings':[{'port':'clk','bit':0,'package_pin':1},{'port':'led','bit':0,'package_pin':2},{'port':'led','bit':1,'package_pin':3}]}
    def check_sheet(sheet,valid):
        design.write_text(json.dumps(sheet))
        event=call('validate','pins','--design',design,'--pins',pins,'--netlist',netlist)
        assert event['data']['valid'] is valid,(sheet,event)
        return event['data']
    check_sheet(base,True)
    for change in ('reserved','duplicate-pin','duplicate-bit','missing-port','out-of-range','partial'):
        sheet=json.loads(json.dumps(base))
        if change=='reserved':sheet['bindings'][0]['package_pin']=7
        elif change=='duplicate-pin':sheet['bindings'][0]['package_pin']=2
        elif change=='duplicate-bit':sheet['bindings'][2]['bit']=0
        elif change=='missing-port':sheet['bindings'][0]['port']='no_such_port'
        elif change=='out-of-range':sheet['bindings'][2]['bit']=2
        elif change=='partial':sheet['bindings'].pop()
        check_sheet(sheet,False)
    sheet=json.loads(json.dumps(base));sheet['bindings'][0]['io_type']='UNKNOWN'
    result=check_sheet(sheet,True)
    assert any(d['severity']=='warning' for d in result['diagnostics'])
    sheet=json.loads(json.dumps(base));sheet['device']='wrong';design.write_text(json.dumps(sheet))
    call('validate','pins','--design',design,'--pins',pins,'--netlist',netlist,code=2)
    call('validate','pins','--design',design,'--pins',pins,code=2)
    badpins=root/'bad.json';badpins.write_text('{}')
    call('validate','cst','--cst',cst,'--pins',badpins,code=4)
    custom=root/'custom';(custom/'target-profiles').mkdir(parents=True)
    (custom/'target-profiles/evil.json').write_text(json.dumps({'id':'different'}))
    call('target','show','--id','evil','--data-dir',custom,code=4)
print('Board target and validation integration passed')
