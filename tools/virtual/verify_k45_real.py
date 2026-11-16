"""K4/K5 on a real generated Verilator runner, never a GUI/model stub."""
import argparse,json,time
from pathlib import Path
from generated_runner_test_support import ROOT,Harness,Client,real_config,write,sha

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--build-dir',type=Path,default=ROOT/'build-m0');args=parser.parse_args()
    h=Harness(args.build_dir,ROOT/'.work/k45/real');clients=[];checks=[];report=dict(status='running',checks=checks,commands=h.commands)
    def check(ok,message):
        if not ok:raise AssertionError(message)
        checks.append(message)
    def client(cfg,name):
        result=Client(h,exe,write(h.work/(name+'.json'),cfg));clients.append(result);return result
    def start(c):
        c.send('set','release-reset',target='reset_p4',state=2);c.send('set','press',target='key_p3',state=13);return c.send('step','first')
    def node(frame,name):return frame['board']['nodes'][name]
    def audit3(c,name):
        stopped=c.send('stop','stop');audit=c.send('journal');records=audit['records'];triples=[]
        for attempt in range(3):
            r=c.send('replay',records=records);check(r['status']=='passed' and r['board_trace_verified'] and r['circuit_digest']==stopped['digests']['circuit'] and r['control_digest']==stopped['digests']['control'],f'{name} from-zero real RTL board/trace replay {attempt+1}')
            triples.append(dict(circuit=r['circuit_digest'],control=r['control_digest'],commits=r['matched_commits']))
        expected=[e for r in records if r['kind']=='commit' for e in r['trace_events']];actual=[];revision='0'
        while True:
            page=c.send('trace.commits',after_revision=revision,through_revision=stopped['trace']['revision'],limit=2)
            for b in page['batches']:actual.extend(b['events'])
            revision=page['next_revision']
            if page['complete']:break
        check(actual==expected,f'{name} paged trace transport matches every committed endpoint event')
        recovered=c.send('recover',path=cfg_base['journal_path']) if name=='fault' else None
        if recovered:check(recovered['status']=='passed' and recovered['trace_revision']==stopped['trace']['revision'],'fault journal OS recovery and trace reconstruction match')
        write(h.work/(name+'-transcript.json'),c.transcript);write(h.work/(name+'-journal.json'),audit)
        c.close();clients.remove(c);return triples,records
    try:
        exe,manifest=h.build_runner(ROOT/'tests/virtual/k2/counter.sv','k2_counter')
        cfg_base=real_config(h.work,True);cfg_base['board_path']=(ROOT/'registry/boards/tang-nano-9k.virtual.json').as_posix();cfg_base['clock']['port_ref']='clock_in'
        for e in cfg_base['inputs']:e['fixed_drivers']=[]
        # Correct, swapped and unbound use exactly the same binary/source.
        normal_cfg=dict(cfg_base,durability='memory',run_id='k45-normal');normal=client(normal_cfg,'normal');a=start(normal)
        check(a['board']['leds']['led0']['lit'] and node(a,'pad.10')['state_code']==13,'A4 correct CST drives physical LED0')
        original=(ROOT/'tests/virtual/k2/counter.cst').read_text(encoding='utf-8')
        swapped=original.replace('"led_n[0]" 10','"led_n[0]" 11').replace('"led_n[1]" 11','"led_n[1]" 10')
        cst=h.work/'swapped.cst';cst.write_text(swapped,encoding='utf-8');wrong=client(dict(normal_cfg,cst_path=cst.as_posix(),run_id='k45-swapped'),'swapped');b=start(wrong)
        check(b['board']['leds']['led1']['lit'] and not b['board']['leds']['led0']['lit'],'A4 physical phenomenon migrates to LED1 with CST swap')
        wrong.close();clients.remove(wrong)
        missing=h.work/'missing.cst';missing.write_text('\n'.join(line for line in original.splitlines() if '"led_n[0]"' not in line)+'\n',encoding='utf-8')
        unbound=client(dict(normal_cfg,cst_path=missing.as_posix(),allow_unbound_outputs=True,run_id='k45-unbound'),'unbound');u=start(unbound)
        check(node(u,'pad.10')['state_code']==2 and node(u,'led0.K')['state_code']==2 and not u['board']['leds']['led0']['lit'] and u['binding_status']=='teaching-unbound','A4 unbound output never bypasses CST')
        unbound.close();clients.remove(unbound)
        for name,text,code in [('missing-clock','\n'.join(line for line in original.splitlines() if '"clock_in"' not in line),'E_MISSING_BINDING'),
                               ('wrong-clock',original.replace('"clock_in" 52','"clock_in" 3').replace('"key_n" 3','"key_n" 52').replace('"clock_in" IO_TYPE=LVCMOS33','"clock_in" IO_TYPE=LVCMOS18').replace('"key_n" IO_TYPE=LVCMOS18','"key_n" IO_TYPE=LVCMOS33'),'E_CLOCK_BINDING')]:
            path=h.work/(name+'.cst');path.write_text(text,encoding='utf-8');config=write(h.work/(name+'.json'),dict(normal_cfg,cst_path=path.as_posix()))
            result=h.run([exe,config],name,required=False);check(result.returncode!=0 and code in result.stderr,f'A4 {name} refuses model startup')
        normal.send('step','normal-second');normal_replays,normal_records=audit3(normal,'normal')
        fault=client(cfg_base,'fault');initial=start(fault)
        p=fault.send('fault.apply','open',fault_id='led-open',edge_id='wire.led0')
        check(p['status']=='pending' and p['trace']==initial['trace'] and p['input_writes']==initial['input_writes'],'A9 paused fault intent does not modify circuit/trace')
        opened=fault.send('step','open-step');again=fault.send('step','while-open')
        check(opened['reason']=='StepComplete' and again['reason']=='StepComplete' and node(opened,'led0.K')['state_code']==node(again,'led0.K')['state_code']==2 and node(opened,'pad.10')['state_code']!=node(again,'pad.10')['state_code'],'A9 output branch remains ObserveOnly: Pad toggles, LED floats, DUT advances')
        fault.send('fault.revoke','restore',fault_id='led-open');restored=fault.send('step','restore-step')
        check(node(restored,'pad.10')['state_code']==node(restored,'led0.K')['state_code'],'A9 paused revoke reconnects exact endpoints')
        fault.send('play','play');running=fault.send('fault.apply','running-open',fault_id='run-open',edge_id='wire.led0');paused=fault.send('pause','pause')
        check(running['status']=='applied' and running['state']=='Running' and node(running,'led0.K')['state_code']==2 and paused['reason']=='User','A9 running fault applies at safe boundary without UnknownInput pause')
        fault.send('play','play-again');running_restore=fault.send('fault.revoke','running-revoke',fault_id='run-open');fault.send('pause','pause-again')
        check(running_restore['status']=='applied' and node(running_restore,'pad.10')['state_code']==node(running_restore,'led0.K')['state_code'],'A9 running revoke restores at safe boundary')
        fault_replays,fault_records=audit3(fault,'fault')
        check(any(r['kind']=='commit' and not r['evaluated'] for r in fault_records),'topology phase has committed boundaries without DUT eval')
        # Input guard contrast: initial unknown, unpulled input open, pulled input open.
        descriptor=json.loads(Path(cfg_base['board_path']).read_text(encoding='utf-8'));descriptor['fixed_drivers']=[]
        no_pull=write(h.work/'no-pull-board.json',descriptor);unknown_cfg=dict(normal_cfg,board_path=no_pull.as_posix())
        unknown=client(unknown_cfg,'initial-unknown');g=unknown.send('get')
        check(g['reason']=='UnknownInput' and g['input_writes']==g['eval_calls']=='0','A7 initial unknown guarded with zero DUT writes/evals');unknown.close();clients.remove(unknown)
        contention_cfg=json.loads(json.dumps(normal_cfg));contention_cfg['inputs'][0]['initial_manual_state']=44
        contention=client(contention_cfg,'initial-contention');g=contention.send('get')
        check(g['reason']=='UnknownInput' and g['input_writes']==g['eval_calls']=='0' and g['guard']['state']&32,'A7 initial CON guarded without any DUT import');contention.close();clients.remove(contention)
        asynchronous=client(normal_cfg,'async-contention');before=start(asynchronous);asynchronous.send('set','bad-reset',target='reset_p4',state=44);g=asynchronous.send('step','async-block')
        check(g['reason']=='UnknownInput' and g['input_writes']==before['input_writes'] and g['eval_calls']==before['eval_calls'] and g['trace']==before['trace'],'A7 asynchronous reset CON blocks pre-clock delta with zero writes/evals/trace')
        asynchronous.close();clients.remove(asynchronous)
        known_cfg=json.loads(json.dumps(unknown_cfg));known_cfg['inputs'][0]['initial_manual_state']=13
        lost=client(known_cfg,'unpulled-open');before=lost.send('get');lost.send('fault.apply','input-open',fault_id='input',edge_id='wire.key3');blocked=lost.send('step','blocked')
        check(blocked['reason']=='UnknownInput' and blocked['input_writes']==before['input_writes'] and blocked['eval_calls']==before['eval_calls'] and blocked['guard']['last_committed_position']==blocked['position'],'A7 unpulled input open blocks before every import and keeps last fence')
        lost.send('fault.revoke','repair',fault_id='input');check(lost.send('step','retry')['reason']=='StepComplete','unknown input repaired through staged topology then guard retry');lost.close();clients.remove(lost)
        pulled=client(normal_cfg,'pulled-open');start(pulled);pulled.send('fault.apply','key-open',fault_id='key',edge_id='wire.key3');g=pulled.send('step','pull-step')
        check(g['reason']=='StepComplete' and node(g,'pad.3')['state_code']==7,'A7 Pad-side weak pullup survives contact branch open');pulled.close();clients.remove(pulled)
        clock=client(normal_cfg,'clock-open');before=start(clock);clock.send('fault.apply','clock-open',fault_id='clock',edge_id='wire.clock');g=clock.send('step','clock-blocked')
        check(g['reason']=='UnknownInput' and g['input_writes']==before['input_writes'],'clock edge open guards without bool coercion');clock.close();clients.remove(clock)
        window=client(normal_cfg,'transient');start(window);window.send('fault.apply','arm-window',fault_id='window',edge_id='wire.led0',trigger=dict(type='transient',start_tick='55556',end_tick='92593'))
        on=window.send('step','window-on');off=window.send('step','window-off');audit=window.send('journal')
        ticks=[r['position']['tick'] for r in audit['records'] if r['kind']=='fault_boundary']
        check(on['board']['wires']['wire.led0'] is False and off['board']['wires']['wire.led0'] is True and ticks==['55556','92593'],'transient half-open interval and phase-zero boundary exact on real RTL');window.close();clients.remove(window)
        report.update(status='passed',check_count=len(checks),A4=True,A7=True,A9=True,A10_replay=True,
            normal_replays=normal_replays,fault_replays=fault_replays,runner_sha256=sha(exe),source_sha256=sha(ROOT/'tests/virtual/k2/counter.sv'),
            generated_adapter_sha256=sha(h.work/'k2_counter/generated_adapter.cpp'),board_sha256=sha(ROOT/'registry/boards/tang-nano-9k.virtual.json'))
    except Exception as e:report.update(status='failed',error=str(e));raise
    finally:
        for c in clients:
            if c.process.poll() is None:c.kill()
        write(ROOT/'.work/k45/real-results.json',report)
    print(f'PASS {len(checks)} real Verilator K4/K5 checks; normal and fault runs each replayed from zero 3 times')
if __name__=='__main__':main()
