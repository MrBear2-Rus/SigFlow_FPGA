"""Agent-token lookup/consume port. Issuance belongs exclusively to Core UI auth."""
import re
from .gateway import GatewayClient,GatewayConfig,GatewayError

class GatewayReceiptClient:
    def __init__(self,config:GatewayConfig):
        self.config=config;self.client=GatewayClient(config)

    @staticmethod
    def _id(value):
        if type(value) is not str or re.fullmatch(r'ui-receipt-[1-9][0-9]{0,19}',value) is None:
            raise GatewayError('INVALID_ARGUMENT')
        return value

    def lookup(self,receipt_id):
        rid=self._id(receipt_id);self.client.health()
        value=self.client._exchange('GET','/api/v1/ui-receipts/'+rid)
        if value.get('receipt_id')!=rid:raise GatewayError('INVALID_RESPONSE')
        return value

    def consume(self,receipt_id,project_id,run_id,action_id,state_version):
        rid=self._id(receipt_id)
        for value in (project_id,run_id,action_id):self.client._identifier(value)
        if type(state_version) is not int or not 0<=state_version<2**63:
            raise GatewayError('INVALID_ARGUMENT')
        self.client.health()
        value=self.client._exchange('POST','/api/v1/ui-receipts/'+rid+'/consume',dict(
            project_id=project_id,run_id=run_id,action_id=action_id,expected_state_version=str(state_version)))
        if (value.get('receipt_id')!=rid or value.get('action_id')!=action_id
                or value.get('status')!='consumed' or value.get('consumed') is not True
                or type(value.get('replayed')) is not bool):raise GatewayError('INVALID_RESPONSE')
        return value
