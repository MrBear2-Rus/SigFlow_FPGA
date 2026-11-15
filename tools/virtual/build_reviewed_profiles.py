"""Reproduce the reviewed 6202 M0 subset; never infer unreviewed pad banks.

Source audit: board schematic physical p2, UG803 QN88P column p7/9/10/12,
DS117 electrical tables physical p60/62. This is an engineering source audit,
not a claim that a board was measured or a human signed an approval.
"""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ROOT / '.work/k1/sources'
OUT = ROOT / 'registry'

def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

sources = [
    ('board-6202', 'tangnano9k-6202.pdf', 'Tang_Nano_9k_3672 / revision history 6202 (2022-10-23), schematic sheet 3/5',
     'https://dl.sipeed.com/fileList/TANG/Nano%209K/2_Schematic/Tang_Nano_9k_3672_Schematic.pdf', [1,2]),
    ('pinout', 'gw1nr9-pinout.pdf', 'UG803-1.6.5E, QN88P (PSRAM) column',
     'https://cdn.gowinsemi.com.cn/UG803E.pdf', [7, 9, 10, 12]),
    ('electrical', 'gw1nr-datasheet.pdf', 'DS117-3.2.5E, tables 3-11 / 3-12, printed pages 49 / 51',
     'https://cdn.gowinsemi.com.cn/DS117E.pdf', [60, 62]),
]
provenance = []
for sid, filename, version, url, pages in sources:
    content = (SOURCES / filename).read_bytes()
    destination = OUT / 'provenance' / filename
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(content)
    provenance.append(dict(id=sid, document_version=version, url=url,
                           sha256=hashlib.sha256(content).hexdigest(), physical_pages=pages,
                           archived_file='registry/provenance/' + filename))

# pin, vendor IO name, bank, voltage, endpoint direction, resource alias
rows = [(10,'IOL15A/GCLKT_6','3',1800,'sink','LED1'),
        (11,'IOL16B','3',1800,'sink','LED2'),
        (13,'IOL21B','3',1800,'sink','LED3'),
        (14,'IOL22B','3',1800,'sink','LED4'),
        (15,'IOL25B','3',1800,'sink','LED5'),
        (16,'IOL26B','3',1800,'sink','LED6'),
        (3,'IOT2A','3',1800,'source','BTN1'),
        (4,'IOL5A/JTAGSEL_N/LPLL_T_in','3',1800,'source','BTN2'),
        (52,'IOR17A/GCLKT_3','1',3300,'source','CLK'),
        (17,'IOB2A','2',3300,'sink','UART_TX'),
        (18,'IOB2B','2',3300,'source','UART_RX')]
lookup = {r[0]:r for r in rows}
pins, pads, endpoints = [], [], []
reserved = {5:'JTAG TMS',6:'JTAG TCK',7:'JTAG TDI',8:'JTAG TDO'}
for pin in range(1,89):
    r = lookup.get(pin)
    io = ('LVCMOS18' if r[3]==1800 else 'LVCMOS33') if r else None
    pins.append(dict(pin=pin, bank=int(r[2]) if r else None,
                     vendor_bank_id=r[2] if r else None, io=r[1] if r else '',
                     available=pin not in reserved, reserved=reserved.get(pin,''),
                     reviewed=bool(r), default_io_type=io,
                     provenance_refs=['board-6202','pinout'] if r else []))
    pads.append(dict(pin=pin, vendor_bank_id=r[2] if r else None,
                     reviewed=bool(r), available=pin not in reserved,
                     allowed_io_types=[io] if r else [],
                     provenance_refs=['board-6202','pinout','electrical'] if r else []))
    if r:
        endpoints.append(dict(pin=pin, role=r[4], voltage_mv=r[3],
                              pull_modes=['none'], alias=r[5],
                              active_low=r[5].startswith(('LED','BTN')),
                              external_pull_up_ohm=10000 if r[5].startswith('BTN') else None,
                              provenance_refs=['board-6202']))
legacy = json.loads((ROOT/'main/fpga/target-profiles/tang-nano-9k.pins.json').read_text(encoding='utf-8-sig'))
db = dict(schema='sigflow.pin-database.v2',device='GW1NR-LV9QN88PC6/I5',package='QFN88',
          vendor_package='QN88P',board_revision='6202',synthetic=False,
          electrical_profile_ref='registry/electrical/tang-nano-9k.6202.reviewed.v1.json',
          pins=pins,board_resources=[],io_types=legacy['io_types'],
          provenance=provenance)
# Resource names are board facts too. Drop the old fictitious CH340/WS2812/USB
# aliases rather than preserve incorrect connections to unreviewed pads.
for pin,io,bank,voltage,role,alias in rows:
    kind='LED' if alias.startswith('LED') else 'BUTTON' if alias.startswith('BTN') else 'CLK' if alias=='CLK' else 'UART'
    description=('Orange LED, active low' if kind=='LED' else
                 'Button to GND, 10K external pull-up' if kind=='BUTTON' else
                 '27MHz oscillator' if kind=='CLK' else 'FPGA '+alias.replace('UART_','')+' to onboard BL702 bridge')
    db['board_resources'].append(dict(alias=alias,pin=pin,description=description,type=kind,
                                     provenance_refs=['board-6202']))
profile = dict(schema='sigflow.electrical.v1',synthetic=False,
               device=db['device'],package='QFN88',vendor_package='QN88P',board_revision='6202',
               pads=pads,banks={'0':{'vccio_mv':3300},'1':{'vccio_mv':3300},
                                '2':{'vccio_mv':3300},'3':{'vccio_mv':1800}},
               io_standards={},endpoints=endpoints,provenance=provenance,
               policy='Conservative validated M0 subset. Matching nominal VCCIO required for input and output; not a claim of all silicon input tolerances. Internal pulls are outside this reviewed subset. Unreviewed pads are blocked.',
               audit='Source cross-check only; no hardware measurement or human sign-off claimed.')
for io, voltage in [('LVCMOS18',1800),('LVCMOS33',3300)]:
    profile['io_standards'][io] = dict(directions=['input','output'],vccio_allowed_mv=[voltage],
                                      drive_allowed_ma=[4,8,12],pull_modes=['none'],
                                      provenance_refs=['electrical'])
write(OUT/'pins/tang-nano-9k.6202.reviewed.v1.json',db)
write(OUT/'electrical/tang-nano-9k.6202.reviewed.v1.json',profile)
write(ROOT/'main/fpga/target-profiles/tang-nano-9k.pins.json',db)
print('Reviewed profiles regenerated; 11 pads reviewed, all other banks explicitly unknown.')
