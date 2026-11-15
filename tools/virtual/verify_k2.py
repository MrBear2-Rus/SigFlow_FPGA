"""K2 closeout: generated adapters, real RTL, durable recovery and live trace."""
import argparse,json,subprocess,time
from pathlib import Path
from generated_runner_test_support import ROOT,Harness,Client,sha,write,real_config,synthetic_config
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--build-dir',type=Path,default=ROOT/'build-m0');args=parser.parse_args()
    h=Harness(args.build_dir,ROOT/'.work/k23/generated');checks=[];clients=[];modules=[]
    report=dict(stage='K2-generated-runner-closeout',status='running',checks=checks,commands=h.commands,modules=modules)
    def check(yes,name):
        if not yes:raise AssertionError(name)
        checks.append(name)
    try:
        exe,manifest=h.build_runner(ROOT/'tests/virtual/k2/counter.sv','k2_counter')
        check(json.loads(h.run([exe,'--self-test-adapter'],'counter-self-test').stdout)['status']=='passed','generated counter adapter checks exact refs, thread ownership and final once')
        config=real_config(h.work);config_path=write(h.work/'counter-config.json',config)
        c=Client(h,exe,config_path);clients.append(c);send=c.send
        initial=send('get');check(initial['eval_calls']=='1' and initial['durability']=='durable','actual RTL initializes with durable commit')
        check(initial['trace']['revision']=='1' and initial['trace']['tail_event_seq']==initial['position']['event_seq'],'initial state and trace share commit fence')
        send('set','reset-release',target='reset_p4',state=2);pending=send('get')
        check(pending['input_writes']==initial['input_writes'] and pending['eval_calls']==initial['eval_calls'],'paused input never writes or evaluates')
        edge=send('step','s1');check(edge['position']['tick']=='18519','Step reaches first 27MHz rising edge')
        send('set','c1',target='key_p3',state=13);send('set','c2',target='key_p3',state=2);send('step','s2')
        audit=send('journal');check(audit['acks']['c1']['status']=='superseded' and audit['acks']['c2']['status']=='applied','pause intent coalescing persisted')
        send('set','c3',target='key_p3',state=13);pressed=send('step','s3')
        check(sum((not pressed['outputs'][f'led_n[{i}]'])<<i for i in range(6))==1,'generated model counts through CST pad wiring')
        send('set','c4',target='key_p3',state=2);send('step','s4');before=send('get')
        send('set','f1',target='reset_p4',state=44);blocked=send('step','s5')
        check(blocked['reason']=='UnknownInput' and blocked['input_writes']==before['input_writes'] and blocked['eval_calls']==before['eval_calls'],'CON asynchronous reset guarded before every DUT import')
        check(blocked['trace']==before['trace'] and blocked['position']==before['position'],'blocked transaction publishes neither trace nor state')
        send('set','repair',target='reset_p4',state=2);send('step','s6')
        before=send('get');duration=1000
        pulse=dict(target='key_p3',pressed_state=13,released_state=2,duration_ticks=str(duration))
        check(send('pulse','pulse',**pulse)['status']=='pending','pulse staged while paused');time.sleep(.02)
        check(send('get')['position']==before['position'],'wall time does not consume pulse duration')
        check(send('set','clash',target='key_p3',state=13)['error']=='E_STIMULUS_CONFLICT','pulse/manual ownership conflict explicit')
        send('step','pulse-step');audit=send('journal')
        released=next(r for r in audit['records'] if r.get('status')=='pulse_released')
        check(int(released['position']['tick'])==int(before['position']['tick'])+duration,'pulse releases at exact simulation tick')
        replay=send('replay',records=audit['records'])
        check(replay['status']=='passed' and replay['circuit_digest']==audit['digests']['circuit'] and replay['control_digest']==audit['digests']['control'],'actual RTL replay verifies both digests')
        bad=json.loads(json.dumps(audit['records']));bad[0]['source_sha256']='0'*64
        check(send('replay',records=bad)['status']=='rejected','replay fingerprint mismatch rejected')
        before=send('get');send('play','play');time.sleep(.02);paused=send('pause','pause');time.sleep(.02)
        check(int(paused['position']['tick'])>int(before['position']['tick']) and send('get')['trace']==paused['trace'],'Play advances, Pause ACK freezes trace')
        stopped=send('stop','stop');check(stopped['final_calls']==1 and send('stop','stop')==stopped,'Stop finalizes once and is idempotent')
        recovered=send('recover',path=config['journal_path'])
        check(recovered['status']=='passed' and recovered['truncated_bytes']==0 and recovered['circuit_digest']==stopped['digests']['circuit'],'durable on-disk journal replays actual RTL')
        check(recovered['trace_revision']==stopped['trace']['revision'],'recovered trace excludes unmarked batches')
        write(h.work/'counter-transcript.json',c.transcript);c.close();clients.remove(c)
        modules.append(dict(top='k2_counter',runner_sha256=sha(exe),generated_source_sha256=sha(h.work/'k2_counter/generated_adapter.cpp'),real_board_profile=True))
        # Kill a running durable worker and restore its last marker, without a
        # clean final() or synthetic hand-constructed journal transaction.
        crash_config=real_config(h.work,False);crash_config['durability']='durable';crash_config['journal_path']=(h.work/'crash.journal').as_posix()
        Path(crash_config['journal_path']).unlink(missing_ok=True)
        crash_path=write(h.work/'crash-config.json',crash_config);crash=Client(h,exe,crash_path);clients.append(crash)
        crash.send('play','crash-play');time.sleep(.03);crash.kill();clients.remove(crash)
        recovery_config=dict(crash_config,durability='memory');recover_path=write(h.work/'recover-config.json',recovery_config)
        recovered_client=Client(h,exe,recover_path);clients.append(recovered_client)
        recovered=recovered_client.send('recover',path=crash_config['journal_path'])
        check(recovered['status']=='passed' and recovered['matched_commits']>=1,'forced process kill restores only checksummed committed prefix')
        recovered_client.close();clients.remove(recovered_client)
        stopped_config=dict(recovery_config,clock=dict(recovery_config['clock'],enabled=False))
        stopped_client=Client(h,exe,write(h.work/'clock-stopped.json',stopped_config));clients.append(stopped_client)
        start=time.monotonic();noedge=stopped_client.send('step','no-edge')
        check(noedge['error']=='E_NO_CLOCK_EDGE' and time.monotonic()-start<2,'generated runner stopped clock returns bounded error')
        stopped_client.close();clients.remove(stopped_client)
        # Entire second adapter/entry is generated; its expanded IO bank is an
        # explicit synthetic test device, never a Tang Nano production profile.
        exe2,m2=h.build_runner(ROOT/'tests/virtual/k2/shifted.sv','k2_shifted')
        check(json.loads(h.run([exe2,'--self-test-adapter'],'shifted-self-test').stdout)['status']=='passed','second generated adapter validates scalar/vector refs including 64 bits')
        cfg2=synthetic_config(h.work,m2,'pulse_clock');path2=write(h.work/'shifted-config.json',cfg2)
        two=Client(h,exe2,path2,True);clients.append(two)
        before=two.send('get');two.send('set','offset',target='keys_data[4]',state=15);two.send('set','zero',target='one_flag[0]',state=15);two.send('set','wide',target='wide_bits[63]',state=15)
        check(two.send('get')['outputs']==before['outputs'],'multi-bit inputs remain staged until step')
        result=two.send('step','shift-step')
        check(result['outputs']['lamps_up[7]'] and not result['outputs']['lamps_up[4]'],'nonzero descending input to ascending output uses bit_slot')
        check(not result['outputs']['one_result[0]'] and not result['outputs']['wide_out[63]'] and result['outputs']['wide_out[62]'],'[0:0] and uint64 bit 63 masks remain exact')
        second_audit=two.send('journal');check(two.send('replay',records=second_audit['records'])['status']=='passed','second generated module replays without handwritten C++ glue')
        two.close();clients.remove(two);modules.append(dict(top='k2_shifted',runner_sha256=sha(exe2),generated_source_sha256=sha(h.work/'k2_shifted/generated_adapter.cpp'),synthetic_profile=True))
        exe3,m3=h.build_runner(ROOT/'tests/virtual/k2/nonconvergent.sv','k2_nonconvergent')
        cfg3=synthetic_config(h.work,m3,'pulse_clock');path3=write(h.work/'nonconvergent-config.json',cfg3)
        start=time.monotonic();unstable=h.run([exe3,path3,'--allow-synthetic-test-profile'],'nonconvergent-run',timeout=10,required=False)
        check(unstable.returncode!=0 and 'E_NON_CONVERGENT' in unstable.stderr and time.monotonic()-start<5,'real combinational nonconvergence maps to bounded E_NON_CONVERGENT')
        report.update(status='passed',check_count=len(checks),verilator_sha256=sha(h.verilator),G2=True,
                      generation='Production VerilatedAdapterCodeGen; generic config-driven SessionRunnerMain; no fixture C++ glue compiled',
                      durability='OS FlushFileBuffers/fsync, chained checksums and marker recovery',
                      scope='G2 and K2a/K2b/K2c; GUI controls/full peripheral and open-fault M0 are later stages')
    except Exception as e:report.update(status='failed',error=str(e),G2=False);raise
    finally:
        for client in clients:
            if client.process.poll() is None:client.kill()
        write(ROOT/'.work/k23/generated-runner-results.json',report)
    print(f'PASS: {len(checks)} generated real-Verilated runner checks, 2 generic adapters plus real nonconvergence and crash recovery.')
if __name__=='__main__':main()
