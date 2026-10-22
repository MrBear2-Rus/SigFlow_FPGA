"""Pinned reviewed material for trusted composition, never ordinary retrieval.

Reviewer names are labels only. The mandatory trusted verifier must authenticate
the live review/teacher authority, its scope and revocation on every call.
"""
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import time

from .reference import POLICY, ReviewedReference
from ..runtime.gateway_session import SessionBinding


_ENTRY_KEYS = frozenset(('reference_id','project_id','revision','issue_id','policy',
    'sha256','reviewers','not_before_epoch','expires_at_epoch'))


def _hash(value):
    return hashlib.sha256(value).hexdigest()


def _linked(path):
    # Path.is_junction is available on Python 3.12+, while U2 supports 3.11.
    return path.is_symlink() or getattr(path,'is_junction',lambda:False)()


def _pairs(pairs):
    value={}
    for key,item in pairs:
        if key in value:
            raise ValueError()
        value[key]=item
    return value


class ReviewedMaterialProvider:
    def __init__(self, manifest_path, manifest_sha256, trusted_directory, verify_review):
        if (type(manifest_sha256) is not str
                or re.fullmatch(r'[a-f0-9]{64}',manifest_sha256) is None
                or not callable(verify_review)):
            raise ValueError('invalid_material_config')
        try:
            root=Path(trusted_directory).absolute()
            if _linked(root) or not root.is_dir():
                raise ValueError()
            self._root=root.resolve(strict=True)
            self._manifest=Path(manifest_path).absolute()
        except Exception:
            raise ValueError('invalid_material_config') from None
        self._pinned, self._verify=manifest_sha256, verify_review

    @staticmethod
    def _read(path, max_bytes):
        # Refuse links before opening and check descriptor identity afterwards.
        # A trusted directory must also be protected against hostile OS writers.
        if _linked(path):
            raise ValueError()
        before=path.stat(follow_symlinks=False)
        if not stat.S_ISREG(before.st_mode) or before.st_size>max_bytes:
            raise ValueError()
        with path.open('rb') as stream:
            current=os.fstat(stream.fileno())
            if (current.st_dev,current.st_ino)!=(before.st_dev,before.st_ino):
                raise ValueError()
            raw=stream.read(max_bytes+1)
        after=path.stat(follow_symlinks=False)
        if (_linked(path) or len(raw)>max_bytes
                or (after.st_dev,after.st_ino)!=(current.st_dev,current.st_ino)):
            raise ValueError()
        return raw

    def _load(self):
        raw=self._read(self._manifest,262144)
        if _hash(raw)!=self._pinned:
            raise ValueError()
        document=json.loads(raw.decode('utf-8'),object_pairs_hook=_pairs)
        if (type(document) is not dict or set(document)!={'schema_version','references'}
                or document['schema_version']!='u2.reviewed-materials.v1'
                or type(document['references']) is not list or not 1<=len(document['references'])<=128):
            raise ValueError()
        identities=set()
        for entry in document['references']:
            if (type(entry) is not dict or set(entry)!=_ENTRY_KEYS
                    or entry['policy']!=POLICY
                    or any(type(entry[k]) is not str or not 1<=len(entry[k])<=256
                           for k in ('project_id','revision','issue_id'))
                    or type(entry['reference_id']) is not str
                    or re.fullmatch(r'[A-Za-z0-9_-]{1,128}',entry['reference_id']) is None
                    or type(entry['sha256']) is not str
                    or re.fullmatch(r'[a-f0-9]{64}',entry['sha256']) is None
                    or type(entry['reviewers']) is not list or not 2<=len(entry['reviewers'])<=8
                    or any(type(r) is not str or re.fullmatch(r'[A-Za-z0-9_-]{1,128}',r) is None
                           for r in entry['reviewers'])
                    or len(set(entry['reviewers']))!=len(entry['reviewers'])
                    or any(type(entry[k]) is not int or not 0<=entry[k]<2**63
                           for k in ('not_before_epoch','expires_at_epoch'))
                    or entry['not_before_epoch']>=entry['expires_at_epoch']):
                raise ValueError()
            identity=tuple(entry[k] for k in ('project_id','revision','issue_id','policy'))
            if identity in identities:
                raise ValueError()
            identities.add(identity)
        return document['references']

    def _review(self, entry):
        now=time.time()
        if (not entry['not_before_epoch']<=now<entry['expires_at_epoch']
                or self._verify(deepcopy(entry)) is not True):
            raise ValueError()
        # Authentication can itself block across the validity deadline.
        if not entry['not_before_epoch']<=time.time()<entry['expires_at_epoch']:
            raise ValueError()

    def __call__(self, binding):
        try:
            if not isinstance(binding,SessionBinding):
                raise ValueError()
            scope=(binding.project_id,binding.revision,binding.issue_id,POLICY)
            entries=self._load()
            selected=[e for e in entries if tuple(e[k] for k in
                ('project_id','revision','issue_id','policy'))==scope]
            if len(selected)!=1:
                raise ValueError()
            entry=selected[0]
            self._review(entry)
            path=self._root/(entry['reference_id']+'.txt')
            if path.resolve(strict=True).parent!=self._root:
                raise ValueError()
            raw=self._read(path,32768)
            if not raw or _hash(raw)!=entry['sha256']:
                raise ValueError()
            item=ReviewedReference(entry['reference_id'],raw.decode('utf-8'),entry['sha256'],
                                   tuple(entry['reviewers']))
            # Revalidate after body I/O, including authority revoked mid-read.
            if self._load()!=entries or path.resolve(strict=True).parent!=self._root:
                raise ValueError()
            self._review(entry)
            return item
        except Exception:
            raise ValueError('reference_material_unavailable') from None
