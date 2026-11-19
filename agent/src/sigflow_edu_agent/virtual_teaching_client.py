"""Opt-in client for the trusted virtual-lab teaching API.

Only public projections cross this port. Private fault plans and rubric are
never made available to chat/model code.
"""
import json
from urllib.parse import urlsplit
from urllib.request import Request,urlopen
class VirtualTeachingClient:
    def __init__(self,url,bearer,run_token):
        target=urlsplit(url)
        if target.scheme!='http' or target.hostname!='127.0.0.1' or target.port is None or target.path!='/api/v1/virtual-teaching/call':raise ValueError('virtual teaching requires canonical loopback endpoint')
        if not bearer or not run_token:raise ValueError('virtual teaching requires trusted session credentials')
        self._url,self._bearer,self._run_token=url,bearer,run_token
    def _call(self,capability,**values):
        payload=json.dumps(dict(capability=capability,run_token=self._run_token,**values)).encode('utf-8');request=Request(self._url,data=payload,headers={'Authorization':'Bearer '+self._bearer,'Content-Type':'application/json'},method='POST')
        with urlopen(request,timeout=600) as response:result=json.load(response)
        if result.get('protocol_version')!='sigflow.virtual-teaching.v1' or result.get('ok') is not True:raise ValueError('virtual teaching request rejected')
        return result['data']
    def session_read(self):return self._call('virtual.session.read')
    def evidence_query(self):return self._call('virtual.evidence.query')
    def hint_request(self,level):return self._call('teaching.hint.request',level=level)
    def grade_submit(self,root_cause,evidence_ids,repair):return self._call('teaching.grade.submit',root_cause=root_cause,evidence_ids=evidence_ids,repair=repair)
