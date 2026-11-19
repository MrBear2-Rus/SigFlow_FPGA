"""Build the explicit P1 lab netlist from reviewed model instances, no HDL guesses."""
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'examples/virtual/phase1'
def read(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def write(p,j):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(j,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
board=read(ROOT/'registry/boards/tang-nano-9k.virtual.json');board['version']='1.0.0-draft'
board['pin_database_ref']='registry/pins/tang-nano-9k.6202.reviewed.v2.json';board['electrical_profile_ref']='registry/electrical/tang-nano-9k.6202.reviewed.v2.json'
board['components'].append(dict(id='uart0',model='sigflow.uart-peer@1',params=dict(baud=921600,data_bits=8,parity='none',stop_bits=1)))
board['components'].append(read(OUT/'contracts/seven-segment.instance.json'))
board['components'].append(dict(id='dip0',model='sigflow.dip-switch@1',params=dict(width=2,initial_value=0)))
def connect(edge,pad,endpoint):board['connections'].append(dict(id=edge,net='pad.'+str(pad),endpoint=endpoint))
connect('wire.uart-rx',17,'uart0.RX');connect('wire.uart-tx',18,'uart0.TX')
segments=['a','b','c','d','e','f','g','dp'];pads=[25,26,27,28,29,30,33,34]
for segment,pad in zip(segments,pads):connect('wire.seg0.'+segment,pad,'seg0.'+segment)
for digit,pad in enumerate([35,36,37,38]):connect('wire.seg0.dig'+str(digit),pad,'seg0.dig'+str(digit))
for bit,pad in enumerate([40,41]):connect('wire.dip0.bit'+str(bit),pad,'dip0.bit'+str(bit))
board['unsupported_m0']=['inout','analogue current'];board['phase1']={'explicit_header_pads':pads+[35,36,37,38,40,41],'external_tf_lcd_attached':False}
write(ROOT/'registry/boards/tang-nano-9k.phase1.virtual.json',board)
profile=read(OUT/'contracts/fault-profile.json');profile['faults'].append(dict(id='stop-clock',enabled=False,visibility='student_visible',hook='clock.schedule',target=dict(kind='clock',id='clk27m'),effect=dict(type='clock_stop',hold_level=0,resume_phase='restart_half_period'),trigger=dict(kind='permanent',start_tick='0')))
write(OUT/'fault-profile.json',profile)
bindings=[('clock_in',52,33),('reset_n',4,18),('key_n',3,18),('uart_tx',17,33),('uart_rx',18,33)]
bindings += [(f'led_n[{i}]',pad,18) for i,pad in enumerate([10,11,13,14,15,16])]
bindings += [(f'seg_n[{i}]',pad,33) for i,pad in enumerate(pads)]
bindings += [(f'dig_en[{i}]',pad,33) for i,pad in enumerate([35,36,37,38])]
bindings += [(f'dip[{i}]',pad,33) for i,pad in enumerate([40,41])]
(OUT/'phase1.cst').write_text(''.join(f'IO_LOC "{label}" {pad};\nIO_PORT "{label}" IO_TYPE=LVCMOS{voltage} PULL_MODE=NONE;\n' for label,pad,voltage in bindings),encoding='utf-8')
project=read(ROOT/'examples/virtual/counter/virtual.project.json');project.update(source_files=['phase1.sv'],top_module='p1_lab',cst_path='phase1.cst',board_path='registry/boards/tang-nano-9k.phase1.virtual.json',electrical_profile_path='registry/electrical/tang-nano-9k.6202.reviewed.v2.json',phase1=True,integration_window_ticks='4000000',fault_profile_path='fault-profile.json',durability='durable')
project['inputs'] += [dict(target='uart_rx_p18',package_pin=18,fixed_drivers=[],initial_manual_state=2),dict(target='dip0_bit0',package_pin=40,fixed_drivers=[],initial_manual_state=2),dict(target='dip0_bit1',package_pin=41,fixed_drivers=[],initial_manual_state=2)]
write(OUT/'virtual.project.json',project)
print('P1 explicit board, 25 Pad CST, four-fault profile and project generated')
