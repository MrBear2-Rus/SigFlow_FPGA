"""Maintain the declarative P2 contracts. C++ additionally checks live topology."""
import copy,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'registry/schemas'
def save(name,value):
    (OUT/name).write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def obj(properties,required=None):
    return dict(type='object',additionalProperties=False,properties=properties,required=list(properties) if required is None else required)
def integer(low=0,high=4294967295):return dict(type='integer',minimum=low,maximum=high)
def tick_string():return dict(type='string',pattern='^(0|[1-9][0-9]{0,19})$')
def main():
    profile=json.loads((OUT/'fault-profile.v1.schema.json').read_text(encoding='utf-8'))
    profile['$id']='sigflow.fault-profile.v2';profile['properties']['schema']['const']=profile['$id']
    profile['$defs']['target']['properties']['kind']['enum']+=['node','driver','pad_map','input']
    profile['$defs']['fault']['properties']['hook']['enum']+=['mapping.overlay','peripheral.observe','protocol.transform','input.sampling']
    extensions=[('bridge',dict(other_node=dict(type='string',minLength=1)),'topology.transform','node'),('pull_removal',{},'driver.transform','driver'),('mapping_overlay',dict(bindings=dict(type='array',minItems=1,maxItems=88,items=obj(dict(port_ref=dict(type='string',minLength=1),pad=dict(anyOf=[integer(1,88),dict(type='null')]))))),'mapping.overlay','pad_map'),('led_polarity_reverse',{},'peripheral.observe','component'),('common_mismatch',{},'peripheral.observe','component'),('dead_segment',dict(segment=dict(enum=['a','b','c','d','e','f','g','dp'])),'peripheral.observe','component'),('uart_baud_offset_ppm',dict(offset_ppm=integer(-500000,500000)),'protocol.transform','component'),('uart_framing_error',dict(bit=dict(enum=['start','stop'])),'protocol.transform','component'),('uart_parity_flip',{},'protocol.transform','component'),
        ('reset_glitch',dict(pressed_state=integer(0,1),duration_ticks=tick_string()),'peripheral.event','component'),
        ('clock_divider_offset',dict(den_offset=tick_string(),resume_phase=dict(const='restart_half_period')),'clock.schedule','clock'),
        ('i2c_nak',dict(phase=dict(enum=['address','data']),nth_transaction=integer(1,1000)),'peripheral.event','component'),
        ('spi_mode_fault',dict(mode=dict(enum=['invert_cpha','bit_slip']),slip_bits=integer(1,7)),'peripheral.event','component'),
        ('crc_tamper',dict(protocol_id=dict(const='sigflow.uart-packet-crc8.v1'),xor_mask=integer(0,255),frame_index=integer(0,1000)),'protocol.transform','component'),
        ('cdc_sampling_approx',dict(window_ps=tick_string(),max_delay_cycles=integer(1,2),policy=dict(const='seeded_enum')),'input.sampling','input')]
    for name,fields,hook,kind in extensions:
        profile['$defs']['effect']['oneOf'].append(obj(dict(type=dict(const=name),**fields)))
        profile['$defs']['fault']['allOf'].append({
            'if':{'properties':{'effect':{'properties':{'type':{'const':name}}}}},
            'then':{'properties':{'hook':{'const':hook},'target':{'properties':{'kind':{'const':kind}}},'trigger':{'properties':{'kind':{'enum':['permanent','transient']}}}}}
        })
    save('fault-profile.v2.schema.json',profile)
    tick=dict(type='string',pattern='^[0-9]{1,20}$')
    capture=obj(dict(schema=dict(const='sigflow.virtual-capture.v1'),source=dict(enum=['virtual_probe','virtual_rtl','independent_wrapper_probe']),layout=dict(enum=['circular_pretrigger_chronological','finite_linear_rtl']),sample_phase=dict(enum=['pre_eval','post_eval']),armed=dict(type='boolean'),triggered=dict(type='boolean'),trigger_index=integer(0,4095),decimation=integer(1,255),sample_clock_hz=tick,clock_pad=integer(1,88),source_fingerprint=dict(type='string',pattern='^[a-f0-9]{64}$'),targets=dict(type='array',minItems=1,maxItems=32,uniqueItems=True,items=dict(type='string')),samples=dict(type='array',maxItems=4096,items=obj(dict(tick=tick,edge_index=tick,sample_index=tick,value=integer(),valid_mask=integer()),['tick','edge_index','value','valid_mask']))))
    capture.update({'$schema':profile['$schema'],'$id':'sigflow.virtual-capture.v1'});save('virtual-capture.v1.schema.json',capture)
    task=obj(dict(schema=dict(const='sigflow.course-task.v1'),id=dict(type='string'),title=dict(type='string'),objective=dict(type='string'),allowed_observations=dict(type='array',minItems=1,items=dict(type='string')),public_tests=dict(type='array',items=dict(type='string')),hint_levels=dict(const=['L1','L2','L3','L4']),mode=dict(const='student_blind_test'),clock_fixture=obj(dict(frequency_hz=tick,physical_board_oscillator_hz=dict(const='27000000'),teaching_override=dict(type='boolean')))))
    task.update({'$schema':profile['$schema'],'$id':'sigflow.course-task.v1'});save('course-task.v1.schema.json',task)
    config=obj(dict(source=dict(const='virtual_probe'),targets=dict(type='array',minItems=1,maxItems=32,uniqueItems=True,items=dict(type='string')),depth=integer(2,4096),pretrigger=integer(0,4095),decimation=integer(1,255),trigger=obj(dict(kind=dict(enum=['mask_equal','edge_rising','edge_falling','stall','timeout']),mask=integer(),value=integer(),count=integer(1,65535))),sample_en=dict(type='boolean'),gate=dict(type='string'),max_samples=integer(1,1000000),max_edges=integer(1,1000000)),['source','targets','depth','pretrigger','decimation','trigger'])
    config.update({'$schema':profile['$schema'],'$id':'sigflow.probe-capture-config.v1'});save('probe-capture-config.v1.schema.json',config)
if __name__=='__main__':main()
