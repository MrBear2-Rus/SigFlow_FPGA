"""Owned real-Core receipt protocol probe, using explicitly synthetic content.

No EDA job or model call; no real GUI, reviewed answer or L4 HTTP activation.
"""
import argparse,hashlib,json,os,secrets,tempfile,time
from pathlib import Path
from .closed_loop import Child,HTTP,check_ready,close_children
from .acceptance_support import RunFailure,require
from .reference import ReferenceGate,ReviewedReference
from ..clients.gateway import GatewayConfig
from ..clients.receipts import GatewayReceiptClient
from ..runtime.gateway_session import SessionBinding

def exercise(gateway_path,directory):
    project=directory/'project';(project/'rtl').mkdir(parents=True)
    (project/'rtl/top.v').write_text('module top(output wire q); assign q=0; endmodule\n')
    (project/'sigflow.project').write_text(json.dumps(dict(build=dict(top_module=['top']),paths=dict(source_files=['rtl/top.v']))))
    instance='inst-'+secrets.token_hex(12);nonce=secrets.token_hex(32)
    agent_token,ui_token=secrets.token_urlsafe(32),secrets.token_urlsafe(32)
    child=Child([str(gateway_path)],os.environ.copy(),cwd=directory)
    try:
        child.send(dict(project_path=str(project),instance_id=instance,nonce=nonce,agent_token=agent_token,ui_token=ui_token))
        ready=child.ready(time.monotonic()+20);check_ready(ready,instance,nonce,gateway=True)
        deadline=time.monotonic()+20
        gateway,ui=HTTP(ready['port'],agent_token,deadline),HTTP(ready['port'],ui_token,deadline)
        route='/api/v1/projects/'+ready['project_id'];revision=gateway.request('GET',route+'/context')['revision']
        binding=SessionBinding(instance,'receipt-probe','integration',ready['project_id'],revision,'probe-snapshot','probe-job','job-report','probe-issue')
        text='Synthetic receipt protocol fixture; no reviewed course answer.'
        item=ReviewedReference('fixture-reference',text,hashlib.sha256(text.encode()).hexdigest(),('fixture-reviewer1','fixture-reviewer2'))
        gate=ReferenceGate(binding,'probe-run',lambda _:item);challenge=gate.request(7)
        body={k:challenge[k] for k in ('session_id','revision','level','policy','challenge_id','action_id','state_version')}
        body.update(issue=binding.issue_id,ttl_seconds=300)
        gateway.request('POST',route+'/ui-receipts',body,status=403)
        ui.request('POST',route+'/ui-receipts',body|dict(state_version='7junk'),status=400)
        receipt=ui.request('POST',route+'/ui-receipts',body,status=201)
        require(receipt['state_version']=='7','receipt_issued_version_invalid')
        rid=receipt['receipt_id'];path='/api/v1/ui-receipts/'+rid+'/consume'
        consume=dict(project_id=binding.project_id,run_id='probe-run',action_id=challenge['action_id'],expected_state_version='7')
        for version in ('7junk','+7','-1','18446744073709551616'):
            gateway.request('POST',path,consume|dict(expected_state_version=version),status=400)
        gateway.request('POST',path,consume|dict(expected_state_version='0'),status=409)
        gateway.request('POST',path,consume|dict(action_id='wrong-action'),status=409)
        gateway.request('POST',path,consume|dict(project_id=''),status=403)
        port=GatewayReceiptClient(GatewayConfig(ready['port'],agent_token,instance))
        require(gate.confirm(rid,challenge['challenge_id'],7,port)['content']==text,'receipt_protocol_failed')
        replay=port.consume(rid,binding.project_id,'probe-run',challenge['action_id'],7)
        require(replay['replayed'] is True,'receipt_replay_invalid')
        gateway.request('POST',path,consume|dict(run_id='another-run'),status=409)
        legacy=body.copy();legacy.pop('state_version');legacy.pop('action_id')
        legacy_receipt=ui.request('POST',route+'/ui-receipts',legacy,status=201)
        require(port.lookup(legacy_receipt['receipt_id'])['state_version']=='0','receipt_default_version_invalid')
        require(port.consume(legacy_receipt['receipt_id'],binding.project_id,'legacy-run','legacy-action',0)['consumed'] is True,
                'receipt_default_version_compatibility_failed')
        require(gateway.request('GET',route+'/jobs')['jobs']==[],'receipt_probe_started_jobs')
        return dict(status='passed',real_core=True,agent_issuance='denied',issued_version='verified',
            malformed_versions='denied',wrong_version='denied',wrong_action='denied',empty_project='denied',
            same_run_replay='verified',cross_run_replay='denied',synthetic_reference_fixture=True,
            default_zero_version_compatibility='verified',
            teaching_HTTP_L4='disabled',real_reviewed_reference='not_available',GUI_signoff='not_run',job_count=0,model_calls=0)
    finally:close_children(((child,10),))

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gateway',required=True,type=Path);parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args(argv)
    try:
        require(args.gateway.is_file(),'gateway_missing')
        with tempfile.TemporaryDirectory(prefix='sigflow-u2-receipts-') as owned:
            report=exercise(args.gateway.resolve(),Path(owned))
        report['cleanup']='completed'
    except RunFailure as error:report=dict(status='failed',error=str(error))
    except Exception:report=dict(status='failed',error='receipt_probe_failed')
    try:args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    except OSError:print('{"status":"failed","error":"report_write_failed"}');return 1
    print(json.dumps(report));return int(report['status']!='passed')

if __name__=='__main__':raise SystemExit(main())
