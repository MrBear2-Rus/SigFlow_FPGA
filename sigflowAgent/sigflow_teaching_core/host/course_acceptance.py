"""Check service-admitted course locators independently against the shipped DB."""
from .acceptance_support import require
from ..rag.citations import resolve_evidence_citations


def verify_course_background(background, db_path):
    require(background.get('status') in ('ok', 'partial') and background.get('references')
            and background.get('answerability') == 'not_assessed', 'course_background_unavailable')
    refs = background['references']
    require(type(refs) is list and 1 <= len(refs) <= 8, 'course_metadata_mismatch')
    versions = {ref['index_version'] for ref in refs}
    require(len(versions) == 1, 'course_index_mismatch')
    ids = [ref['passage_id'] for ref in refs]
    rows = resolve_evidence_citations(ids, ids, index_version=refs[0]['index_version'], db_path=db_path)
    for ref, row in zip(refs, rows):
        require(all(ref[key] == row[column] for key, column in (
            ('passage_id', 'chunk_id'), ('parent_id', 'parent_id'), ('title', 'title'),
            ('section', 'section'), ('course_version', 'course_version'),
            ('source_content_hash', 'content_hash'), ('index_version', 'index_version'))), 'course_metadata_mismatch')
        spans = row['source_spans']
        urls = list(dict.fromkeys(s['source_url'] + (s.get('anchor') or '') for s in spans))
        require(ref['source_url'] == urls[0] and ref['source_urls'] == urls[:8]
                and len(ref['source_spans']) == len(spans), 'course_locator_mismatch')
        for actual, stored in zip(ref['source_spans'], spans):
            require(all(actual[key] == stored[key] for key in (
                'document_id', 'source_url', 'line_start', 'line_end', 'text_hash', 'document_hash'))
                and actual['source_text_hash'] == stored.get('source_text_hash', stored['text_hash'])
                and actual['normalization'] == stored.get('normalization')
                and actual['locator_kind'] == stored.get('locator_kind', 'pinned_source'), 'course_locator_mismatch')
    return dict(status=background['status'], reference_count=len(refs), index_versions=sorted(versions),
                metadata_verified=True, answerability='not_assessed')
