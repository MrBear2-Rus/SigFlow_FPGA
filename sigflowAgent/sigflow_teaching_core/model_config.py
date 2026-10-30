"""Interactive credential setup and a single explicit real DeepSeek smoke call."""
from dataclasses import asdict
import argparse
import getpass
import json
import os
import sys
import warnings

from .credentials import CredentialError,save_config
from .deepseek import DeepSeekConfig,DeepSeekProvider
from .demo import FixtureSource
from .domain import CardContext,ProviderError
from .pedagogy import checked_card


def _output(value,key):
    def scrub(item):
        if type(item) is str:
            return item.replace(key,'[redacted]')
        if type(item) is dict:
            return {name:scrub(part) for name,part in item.items()}
        if type(item) in (list,tuple):
            return [scrub(part) for part in item]
        return item
    print(json.dumps(scrub(value),ensure_ascii=False,indent=2))


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    commands=parser.add_subparsers(dest='command',required=True)
    configure=commands.add_parser('configure',help='Hidden key input; save current-user encrypted configuration')
    configure.add_argument('--model',default='deepseek-flash')
    commands.add_parser('status',help='Show configuration status; no API request')
    commands.add_parser('test',help='Send one teaching fixture to the real DeepSeek API; no EDA')
    args=parser.parse_args(argv)
    try:
        if args.command=='configure':
            with warnings.catch_warnings():
                warnings.simplefilter('error',getpass.GetPassWarning)
                key=getpass.getpass('DeepSeek API Key (hidden): ')
            save_config(DeepSeekConfig(key,args.model))
            print(json.dumps(dict(saved=True,storage='windows-current-user-dpapi',
                environment_override=any(k in os.environ for k in ('DEEPSEEK_API_KEY','DEEPSEEK_MODEL','DEEPSEEK_BASE_URL')))))
            return 0
        provider=DeepSeekProvider.from_settings()
        if args.command=='status':
            source='environment' if any(k in os.environ for k in ('DEEPSEEK_API_KEY','DEEPSEEK_MODEL','DEEPSEEK_BASE_URL')) else 'encrypted_file'
            _output(dict(configured=True,model=provider.config.model,source=source),provider.config.api_key)
            return 0
        context=CardContext('如何核对这个教学示例中的存储行为？',1,FixtureSource().collect('demo','r1'))
        value=provider.generate(context)  # Exactly one request; failure never becomes a rule fallback.
        card=checked_card(value,context,'model')
        if card is None:
            raise ProviderError('invalid_output')
        _output(dict(status='passed',model=provider.config.model,producer='model',
                     evidence_origin='fixture',requests=1,card=asdict(card)),provider.config.api_key)
        return 0
    except getpass.GetPassWarning:
        print('hidden_input_unavailable',file=sys.stderr)
        return 2
    except (EOFError,KeyboardInterrupt):
        print('configuration_cancelled',file=sys.stderr)
        return 2
    except CredentialError as error:
        print(str(error),file=sys.stderr)
        return 2
    except ProviderError as error:
        print(error.code,file=sys.stderr)
        return 3 if args.command=='test' else 2


if __name__=='__main__':
    raise SystemExit(main())
