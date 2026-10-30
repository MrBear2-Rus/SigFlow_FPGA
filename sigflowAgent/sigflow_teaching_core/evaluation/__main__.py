"""Print a JSON evaluation report; exit 0 pass, 1 failing cases, 2 invalid input."""

import argparse
import json

from . import evaluate, load_suite


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', required=True)
    args = parser.parse_args()
    try:
        report = evaluate(load_suite(args.suite))
    except ValueError:
        print(json.dumps({'error': 'invalid_suite'}))
        return 2
    print(json.dumps(report, ensure_ascii=True, indent=2))
    return 1 if report['failed'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
