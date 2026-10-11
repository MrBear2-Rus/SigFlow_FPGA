"""Read-only Gateway connectivity probe configured by environment variables."""

import argparse
import json
import os
import sys

from .gateway import GatewayClient, GatewayConfig, GatewayError


def main(argv=None):
    parser = argparse.ArgumentParser(description='Read-only Gateway connectivity/report probe')
    parser.add_argument('--report', action='store_true',
                        help='Read an existing bound Job report; does not validate evidence or execute Jobs')
    args = parser.parse_args(argv)
    try:
        binding = {}
        if args.report:
            for key, env_key in (('job_id', 'SIGFLOW_JOB_ID'), ('project_id', 'SIGFLOW_PROJECT_ID'),
                                 ('revision', 'SIGFLOW_REVISION'), ('snapshot_id', 'SIGFLOW_SNAPSHOT_ID')):
                binding[key] = os.environ.get(env_key, '')
                if not binding[key]:
                    raise GatewayError('INVALID_ARGUMENT')
        try:
            port = int(os.environ.get('SIGFLOW_GATEWAY_PORT', ''))
        except ValueError:
            raise ValueError('invalid_gateway_port') from None
        client = GatewayClient(GatewayConfig(port, os.environ.get('SIGFLOW_GATEWAY_TOKEN', ''),
                                             os.environ.get('SIGFLOW_GATEWAY_INSTANCE', '')))
        if args.report:
            report = client.job_report(**binding)
            result = dict(binding, instance_id=client.config.instance_id,
                          state=report['state'], completeness=report['completeness'],
                          diagnostic_count=len(report.get('diagnostics', [])),
                          artifact_count=len(report.get('artifacts', [])), evidence_validated=False)
        else:
            client.health()
            caps = client.capabilities()
            result = {'instance_id': caps.instance_id, 'available_ids': caps.available_ids}
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2
    print(json.dumps(result))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
