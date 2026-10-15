"""Optional course retrieval and packing; engineering evidence keeps priority."""
from dataclasses import asdict, replace
from pathlib import Path
import re
from urllib.parse import urlparse

from .context.builder import build_context
from .context.models import ContextItem
from .domain import CourseBundle, CourseMaterial, CourseReference, CourseSpan, Evidence, CardContext


class SQLiteCourseProvider:
    def __init__(self, db_path: Path | None = None):
        from .rag.database import configured_database
        self.db_path = Path(db_path).resolve() if db_path is not None else configured_database()

    def prepare(self, question: str, concept_ids: tuple[str, ...]) -> CourseBundle:
        from .rag.agent_adapter import prepare_course_context
        prepared = prepare_course_context(question, [c for c in concept_ids if c != 'unknown'],
                                          db_path=self.db_path, candidate_limit=20)
        child_ids=[p.reference.passage_id for p in prepared.passages
                   if p.reference.parent_id and p.reference.parent_id!=p.reference.passage_id]
        if child_ids:
            from .rag.citations import resolve_evidence_citations
            resolve_evidence_citations(child_ids,child_ids,index_version=prepared.index_version,db_path=self.db_path)
        return CourseBundle(prepared.status, prepared.passages, 'evidence-v2', question,
            prepared.required_sources,prepared.required_evidence_keys,prepared.candidate_ids,
            prepared.unsupported_concept_ids,prepared.selection_diagnostics)


def _https(value: str) -> bool:
    parsed = urlparse(value)
    return (parsed.scheme == 'https' and bool(parsed.hostname)
            and not parsed.username and not parsed.password)

def _locator(value: str, kind: str) -> bool:
    if _https(value):
        return kind != 'authored_note'
    parsed=urlparse(value)
    return (kind=='authored_note' and parsed.scheme=='sigflow-note' and not parsed.netloc
            and parsed.path.startswith('/team-note:') and '..' not in parsed.path)


def pack_courses(evidence: Evidence, bundle: CourseBundle):
    """Validate the provider result, retain whole fragments, return admitted references."""
    if (not isinstance(bundle, CourseBundle) or bundle.status not in {'ok', 'no_match'}
            or type(bundle.materials) is not tuple
            or len(bundle.materials) > (512 if bundle.interface_version=='evidence-v2' else 3)
            or (bundle.status == 'ok') != bool(bundle.materials)):
        raise ValueError('invalid_course_bundle')
    seen, versions, items, refs = set(), set(), [], {}
    for material in bundle.materials:
        if not isinstance(material, CourseMaterial) or not isinstance(material.reference, CourseReference):
            raise ValueError('invalid_course_material')
        ref = material.reference
        fields = (ref.passage_id, ref.index_version, ref.title, ref.section, ref.source_url,
                  ref.material_kind, ref.material_note)
        if (not all(type(v) is str and v.strip() and len(v.encode('utf-8')) <= 2048 for v in fields)
                or len(ref.passage_id) > 240 or ref.passage_id in seen
                or ref.material_kind not in {'authored_note', 'source_excerpt', 'translated_source'}
                or not (_locator(ref.source_url,ref.material_kind) if bundle.interface_version=='evidence-v2' else _https(ref.source_url) or _locator(ref.source_url,ref.material_kind))
                or type(ref.source_urls) is not tuple or len(ref.source_urls) > 8
                or not all(type(u) is str and len(u) <= 2048 and (_https(u) or _locator(u,ref.material_kind)) for u in ref.source_urls)
                or type(material.text) is not str or not material.text.strip()):
            raise ValueError('invalid_course_reference')
        if ref.material_kind == 'translated_source':
            if (type(ref.translation_id) is not str or not ref.translation_id.strip()
                    or type(ref.source_content_hash) is not str
                    or not re.fullmatch(r'sha256:[0-9a-f]{64}', ref.source_content_hash)
                    or not ref.course_version):
                raise ValueError('invalid_course_translation_link')
        elif ref.translation_id is not None:
            raise ValueError('unexpected_course_translation_link')
        if bundle.interface_version=='evidence-v2':
            if (type(ref.source_spans) is not tuple or not ref.source_spans
                    or not all(isinstance(s,CourseSpan) and type(s.line_start) is int
                        and type(s.line_end) is int and 1<=s.line_start<=s.line_end
                        and all(re.fullmatch(r'sha256:[0-9a-f]{64}',h) for h in
                                (s.text_hash,s.source_text_hash,s.document_hash))
                        for s in ref.source_spans)):
                raise ValueError('invalid_course_chunk_locator')
        seen.add(ref.passage_id)
        versions.add(ref.index_version)
        item = ContextItem('course:' + ref.passage_id, 'course', evidence.project_id,
                           evidence.revision, material.text)
        items.append(item)
        refs[item.item_id] = ref
    if len(versions) > 1:
        raise ValueError('mixed_course_versions')
    if bundle.interface_version=='evidence-v2':
        return _pack_evidence_v2(evidence,bundle,items,refs)
    if not items:
        return evidence, (), 'no_match', ()
    original_bytes = sum(len(item.text.encode('utf-8')) for item in evidence.context_items)
    packed = build_context(evidence.context_items + tuple(items), evidence.project_id, evidence.revision,
                           max_bytes=8192, max_tokens=max(4096, original_bytes))
    kept = {item.item_id for item in packed.items}
    if not {item.item_id for item in evidence.context_items} <= kept:
        raise ValueError('course_displaced_engineering_evidence')
    admitted = tuple(refs[item.item_id] for item in packed.items if item.item_id in refs)
    status = 'ok' if len(admitted) == len(items) else 'partial' if admitted else 'omitted'
    return replace(evidence, context_items=packed.items, omitted=evidence.omitted + packed.omitted), admitted, status, packed.omitted


def _pack_evidence_v2(evidence, bundle, items, refs):
    """Budgeted marginal coverage, with source/explicit-definition obligations.

    Uses only query-derived requirements. Neither evaluation gold nor special
    passage IDs enter selection. Candidate ranking/filter weights are untouched.
    """
    from .context.builder import estimate_tokens
    from .context.models import Omission
    from .deepseek import serialize_request
    # Validate engineering data without deleting it to make course text fit.
    build_context(evidence.context_items,evidence.project_id,evidence.revision,
                  max_bytes=1048576,max_tokens=1048576)
    byte_limit,token_limit,request_limit=8192,4096,16384
    engineering_bytes=sum(len(i.text.encode('utf-8')) for i in evidence.context_items)
    used=engineering_bytes
    tokens=sum(estimate_tokens(i.text) for i in evidence.context_items)
    required=set(bundle.required_evidence_keys)|{'source:'+s for s in bundle.required_sources}
    covered=set();chosen=[];omitted=[];attempts=[]
    candidates=list(zip(bundle.materials,items))
    seen_ranges=set()
    while candidates and len(chosen)<8:
        def priority(pair):
            material,item=pair
            keys=set(material.coverage_keys)
            gain=len(keys & (required-covered))
            # Diverse source/section/technical facts, not repeated sibling votes.
            complement=len(keys-covered)
            return (-gain,-complement,-material.selection_score,len(item.text.encode('utf-8')),
                    material.candidate_rank,material.reference.passage_id)
        candidates.sort(key=priority)
        material,item=candidates.pop(0)
        span_keys={(s.document_id,s.line_start,s.line_end,s.text_hash) for s in material.reference.source_spans}
        if span_keys and span_keys<=seen_ranges:
            attempts.append({'id':material.reference.passage_id,'reason':'duplicate_source_span'})
            continue
        size=len(item.text.encode('utf-8'));cost=estimate_tokens(item.text)
        reason=('engineering_over_budget' if engineering_bytes>byte_limit or engineering_bytes>token_limit else
                'byte_budget' if used+size>byte_limit else
                'token_budget' if tokens+cost>token_limit else
                'item_limit' if len(evidence.context_items)+len(chosen)+1>64 else None)
        if reason:
            omitted.append(Omission(item.item_id,reason));attempts.append({'id':material.reference.passage_id,'reason':reason})
            continue
        tentative=tuple(i for _,i in chosen)+ (item,)
        trefs=tuple(m.reference for m,_ in chosen)+(material.reference,)
        probe=replace(evidence,context_items=evidence.context_items+tentative)
        # Worst allowed model-name length and output cap; repair=True adds bytes.
        size_request=len(serialize_request(CardContext(bundle.question,3,probe,True,trefs,'partial'),
                                         model='m'*128,max_tokens=8192))
        # Reserve diagnostics/omissions not yet known; final check below is exact.
        if size_request+1024>request_limit:
            omitted.append(Omission(item.item_id,'request_budget'))
            attempts.append({'id':material.reference.passage_id,'reason':'request_budget'})
            continue
        chosen.append((material,item));used+=size;tokens+=cost
        covered.update(material.coverage_keys);seen_ranges.update(span_keys)
    missing_sources=tuple(s for s in bundle.required_sources if 'source:'+s not in covered)
    missing_keys=tuple(sorted(required-covered))
    admitted=tuple(m.reference for m,_ in chosen)
    status='no_match' if not items else 'omitted' if not chosen else 'partial' if missing_keys or omitted or candidates else 'ok'
    diagnostics={
        **bundle.selection_diagnostics,
        'interface_version':'evidence-v2','missing_required_sources':missing_sources,
        'missing_required_evidence':missing_keys,'answerability':'not_assessed',
        'unsupported_concept_ids':bundle.unsupported_concept_ids,
        'budget_used':{'bytes':used,'estimated_tokens':tokens},
        'budget_remaining':{'bytes':max(0,byte_limit-used),'estimated_tokens':max(0,token_limit-tokens)},
        'budget_limits':{'bytes':byte_limit,'estimated_tokens':token_limit,'request_bytes':request_limit},
        'token_estimator':'utf8_byte_upper_bound',
        'packing_status':status,
        'candidate_not_selected_count':len(candidates),
        'omission_counts':{reason:sum(o.reason==reason for o in omitted) for reason in sorted({o.reason for o in omitted})},
        'debug':{'retrieved_candidates':bundle.retrieved_candidates,
            'selected_evidence':[m.reference.passage_id for m,_ in chosen],
            'packed_evidence':[r.passage_id for r in admitted],'omitted_evidence':attempts,
            'candidate_not_selected':[m.reference.passage_id for m,_ in candidates]},
    }
    result=replace(evidence,context_items=evidence.context_items+tuple(i for _,i in chosen),
                   omitted=evidence.omitted+tuple(omitted),course_diagnostics=diagnostics)
    exact=len(serialize_request(CardContext(bundle.question,3,result,True,admitted,status),
                                model='m'*128,max_tokens=8192))
    diagnostics['budget_used']['request_bytes']=exact
    # The final serializer independently rejects over-limit inputs. Engineering
    # alone may already exceed the request budget and must never be hidden.
    if exact>request_limit and chosen:
        # Re-evaluate with no course references; preserve all engineering evidence.
        removed=tuple(Omission(i.item_id,'request_budget') for _,i in chosen)
        diagnostics['missing_required_sources']=bundle.required_sources
        diagnostics['missing_required_evidence']=tuple(sorted(required))
        diagnostics['debug']['packed_evidence']=[]
        diagnostics['packing_status']='omitted'
        diagnostics['omission_counts']['request_budget']=diagnostics['omission_counts'].get('request_budget',0)+len(removed)
        diagnostics['debug']['omitted_evidence'].extend({'id':r.passage_id,'reason':'request_budget'} for r in admitted)
        diagnostics['budget_used']={'bytes':engineering_bytes,'estimated_tokens':engineering_bytes}
        diagnostics['budget_remaining']={'bytes':max(0,byte_limit-engineering_bytes),'estimated_tokens':max(0,token_limit-engineering_bytes)}
        result=replace(evidence,omitted=evidence.omitted+tuple(omitted)+removed,course_diagnostics=diagnostics)
        diagnostics['budget_used']['request_bytes']=len(serialize_request(
            CardContext(bundle.question,3,result,True,(),'omitted'),model='m'*128,max_tokens=8192))
        return result,(),'omitted',tuple(omitted)+removed
    return result,admitted,status,tuple(omitted)
