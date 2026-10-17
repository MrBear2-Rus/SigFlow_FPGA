"""Reproducible offline checks and externally pinned frozen-probe inputs.

Review IDs are declarations from the selected manifest, not authenticated teacher
scores. This module never upgrades development evidence to a team acceptance.
"""
import argparse,hashlib,json,re
from pathlib import Path
from . import evaluate,load_suite,_validate,_source_hash
from ..patterns.support import support_matrix

def _pairs(pairs):
    result={}
    for key,value in pairs:
        if key in result:raise ValueError('invalid_frozen_suite')
        result[key]=value
    return result

def _read(path,limit):
    try:
        with Path(path).open('rb') as stream:raw=stream.read(limit+1)
        if not raw or len(raw)>limit:raise ValueError()
        return raw
    except (OSError,ValueError):raise ValueError('invalid_frozen_suite') from None

def _json(raw):
    try:return json.loads(raw,object_pairs_hook=_pairs,parse_constant=lambda _:(_ for _ in ()).throw(ValueError()))
    except (ValueError,UnicodeError,RecursionError):raise ValueError('invalid_frozen_suite') from None

def load_frozen_suite(manifest_path,suite_path,*,expected_manifest_sha256):
    """Paths are trusted operator inputs; no path from a manifest is followed."""
    if type(expected_manifest_sha256) is not str or re.fullmatch('[a-f0-9]{64}',expected_manifest_sha256) is None:
        raise ValueError('invalid_frozen_suite')
    raw=_read(manifest_path,16384)
    if hashlib.sha256(raw).hexdigest()!=expected_manifest_sha256:raise ValueError('invalid_frozen_suite')
    manifest=_json(raw)
    if (type(manifest) is not dict or set(manifest)!={'schema_version','suite_version','suite_sha256','reviewers'}
            or manifest['schema_version']!='edu.frozen-probe-suite.v1'
            or type(manifest['suite_version']) is not str or re.fullmatch('[A-Za-z0-9_.-]{1,128}',manifest['suite_version']) is None
            or type(manifest['suite_sha256']) is not str or re.fullmatch('[a-f0-9]{64}',manifest['suite_sha256']) is None
            or type(manifest['reviewers']) is not list or not 2<=len(manifest['reviewers'])<=8
            or not all(type(r) is str and re.fullmatch('[A-Za-z0-9_-]{1,128}',r) for r in manifest['reviewers'])
            or len(set(manifest['reviewers']))!=len(manifest['reviewers'])):
        raise ValueError('invalid_frozen_suite')
    raw_suite=_read(suite_path,65536)
    if hashlib.sha256(raw_suite).hexdigest()!=manifest['suite_sha256']:raise ValueError('invalid_frozen_suite')
    suite=_json(raw_suite);_validate(suite)
    return suite

def build_handoff(development_path,frozen_path):
    report=evaluate(load_suite(development_path))
    # An empty legacy JSONL is explicitly missing, never a successful zero-case run.
    try:
        with Path(frozen_path).open('rb') as stream:raw=stream.read(1024*1024+1)
        if len(raw)>1024*1024:raise ValueError('invalid_frozen_suite')
    except OSError:raw=b''
    return dict(schema_version='edu.u2-handoff.v1',source_sha256=_source_hash(),development=report,
        detector_support=support_matrix(),frozen_dataset=dict(status='missing' if not raw.strip() else 'unvalidated',
            size_bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest()),
        real_model='not_run',teacher_review='not_run',team_gate='pending_external_signoff',
        unresolved=['reviewed_reference_material','dynamic_stimulus_contract','team_frozen_dataset',
                    'two_teacher_scores','GUI_signoff','native_target_OS'])

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--development-suite',required=True,type=Path)
    parser.add_argument('--frozen-fixtures',required=True,type=Path)
    parser.add_argument('--frozen-manifest',type=Path)
    parser.add_argument('--frozen-suite',type=Path)
    parser.add_argument('--manifest-sha256')
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args(argv)
    if any((args.frozen_manifest,args.frozen_suite,args.manifest_sha256)) and not all((args.frozen_manifest,args.frozen_suite,args.manifest_sha256)):
        parser.error('all_frozen_inputs_required')
    try:
        report=build_handoff(args.development_suite,args.frozen_fixtures)
        if args.frozen_manifest:
            suite=load_frozen_suite(args.frozen_manifest,args.frozen_suite,expected_manifest_sha256=args.manifest_sha256)
            report['frozen_probes']=evaluate(suite)
            report['frozen_probes']['manifest_sha256']=args.manifest_sha256
            report['frozen_probes']['review_authenticity']='external_signoff_required'
        args.output.write_text(json.dumps(report,ensure_ascii=True,indent=2)+'\n',encoding='utf-8')
        return int(bool(report['development']['failed'] or report.get('frozen_probes',{}).get('failed',0)))
    except (ValueError,OSError):
        print('{"error":"invalid_handoff_input"}');return 2

if __name__=='__main__':raise SystemExit(main())
