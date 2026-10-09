"""Read-only Gateway connectivity probe configured by environment variables."""

import json
import os
import sys

from .gateway import GatewayClient, GatewayConfig


def main():
    try:
        try:
            port = int(os.environ.get('SIGFLOW_GATEWAY_PORT', ''))
        except ValueError:
            raise ValueError('invalid_gateway_port') from None
        client = GatewayClient(GatewayConfig(port, os.environ.get('SIGFLOW_GATEWAY_TOKEN', ''),
                                             os.environ.get('SIGFLOW_GATEWAY_INSTANCE', '')))
        client.health()
        caps = client.capabilities()
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2
    print(json.dumps({'instance_id': caps.instance_id, 'available_ids': caps.available_ids}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
