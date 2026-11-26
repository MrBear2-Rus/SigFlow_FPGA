"""Explicit, inference-backed compatibility verification; publish a separate v2 index."""
import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'embedding_module/src')]


def migrate(model: Path, source: Path, output: Path) -> dict:
    import numpy as np
    from sigflow_embedding import get_provider
    from sigflow_teaching_core.rag.gemma_index import new_index_file

    if output.exists():
        raise FileExistsError('Use a new output path; existing indexes are never upgraded in place')
    provider = get_provider(model)
    original_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    connection = sqlite3.connect(source.resolve().as_uri() + '?mode=ro', uri=True)
    try:
        metadata = dict(connection.execute('SELECT key, value FROM meta'))
        contract = json.loads(metadata['encoder_contract'])
        for key, value in contract.items():
            if key in provider.contract and value != provider.contract[key]:
                raise ValueError('Historical contract mismatch: ' + key)
        rows = connection.execute('SELECT record, vector FROM units ORDER BY ordinal').fetchall()
        maximum_error = 0.0
        for number, (raw, blob) in enumerate(rows, 1):
            record = json.loads(raw)
            if record['unit_id'] != record['parent_passage_id']:
                raise ValueError('Only full-passage legacy indexes are supported')
            vector = provider.encode_documents([record['content']], [record['title']])[0]
            stored = np.frombuffer(blob, dtype='<f4')
            if stored.shape != (768,) or not np.allclose(vector, stored, rtol=1e-5, atol=1e-6):
                raise ValueError('Encoding differs for ' + record['unit_id'])
            maximum_error = max(maximum_error, float(np.max(np.abs(vector - stored))))
            if number % 50 == 0:
                print(f'verified documents {number}/{len(rows)}', flush=True)
        verification = {'source_sha256': original_hash, 'document_count': len(rows),
                        'maximum_absolute_error': maximum_error,
                        'method': 'all documents re-encoded; rtol=1e-5, atol=1e-6',
                        'new_fingerprint': provider.fingerprint}
        with new_index_file(output) as target:
            connection.backup(target)
            target.execute("UPDATE meta SET value=? WHERE key='encoder_contract'",
                           (json.dumps(provider.contract),))
            target.execute('INSERT OR REPLACE INTO meta VALUES (?, ?)',
                           ('legacy_verification', json.dumps(verification)))
    finally:
        connection.close()
    if hashlib.sha256(source.read_bytes()).hexdigest() != original_hash:
        raise ValueError('Historical index changed during verification')
    return verification


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--dependencies', type=Path)
    args = parser.parse_args()
    if args.dependencies:
        sys.path.insert(0, str(args.dependencies))
    import torch
    torch.set_num_threads(4)
    print(json.dumps(migrate(args.model, args.source, args.output), indent=2))


if __name__ == '__main__':
    main()
