"""Prepare immutable, path-free course data for a trusted teaching host.

Does not import U2, contact a model, update an index, or record student actions.
A single read transaction supplies both text and citation metadata.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

from .citations import CitationValidationError, validate_citations
from .database import DEFAULT_DB_PATH
from .retriever import retrieve_course_passages
from ..domain import CourseReference, CourseSpan, CourseMaterial


@dataclass(frozen=True)
class CoursePassage:
    reference: CourseReference
    text: str


@dataclass(frozen=True)
class PreparedCourse:
    status: str
    index_version: str
    passages: tuple[CoursePassage, ...] = ()
    required_sources: tuple[str, ...] = ()
    required_evidence_keys: tuple[str, ...] = ()
    candidate_ids: tuple[str, ...] = ()
    unsupported_concept_ids: tuple[str, ...] = ()
    selection_diagnostics: dict = field(default_factory=dict)


def _prepare_legacy_context(query: str, concept_ids: list[str] | None = None, *,
                           db_path: Path = DEFAULT_DB_PATH, top_k: int = 3) -> PreparedCourse:
    result = retrieve_course_passages(query, concept_ids, top_k=top_k, db_path=Path(db_path))
    version = result["index_version"]
    rows = result["passages"]
    passages = []
    for row in rows:
        spans = row.get("source_spans", [])
        if not spans:
            continue
        authored = row["content_type"] == "authored_note"
        official_tool = not authored and row["source_id"] in {"lowrisc_style", "yosys_047", "verilator_5052"}
        translation = next((item for item in row.get("translations", [])
                            if item.get("source_content_hash") == row.get("content_hash")
                            and item.get("review_status") == "machine_translation_unreviewed"), None)
        use_translation = bool(official_tool and re.search(r"[\u3400-\u9fff]", query) and translation)
        kind = ("authored_note" if authored else "translated_source" if use_translation else "source_excerpt")
        if authored:
            note = "AI 辅助原创讲解，待团队和教师审阅；不是官方原文或官方翻译。"
        elif use_translation:
            note = (f"机器生成的中文对照译文，未教师审阅、不是官方译文；绑定官方原文 passage {row['passage_id']}，"
                    f"原文内容 hash {translation['source_content_hash']}，资料版本：{row['course_version']}。引用请回到所列官方英文原文。")
        elif official_tool:
            note = (f"官方工具资料英文原文，资料版本：{row['course_version']}；仅描述该版本工具/项目规范，不证明当前工程事实；"
                    "编码风格不自动构成功能判错标准。")
        else:
            note = "从登记的课程正文范围提取的课程原文。"
        raw_ref = row
        # Whitelist public metadata; never forward local_path or the raw result.
        urls = tuple(dict.fromkeys(span["source_url"] + (span.get("anchor") or "") for span in spans))
        urls = urls[:8]
        reference = CourseReference(row["passage_id"], version, raw_ref["title"], raw_ref["section"],
                                    urls[0] if urls else raw_ref["source_url"] + (raw_ref.get("anchor") or ""),
                                    kind, note, urls, row["course_version"],
                                    translation["translation_id"] if use_translation else None,
                                    translation["source_content_hash"] if use_translation else row["content_hash"])
        parts = [f"[{reference.passage_id}] {reference.title}", f"材料属性: {note}",
                 f"课程章节: {row['section']}"]
        retrieval_mapping = row.get("retrieval_mapping") or {}
        if official_tool and retrieval_mapping.get("title_zh"):
            parts.insert(1, f"中文检索标题（仓库映射，非官方标题）: {retrieval_mapping['title_zh']}")
        if use_translation:
            parts.append(f"译文标识: {translation['translation_id']}；机器翻译初稿，未教师审阅，不是官方译文。")
            parts.append(f"译文对应官方原文内容 hash: {translation['source_content_hash']}\n中文对照译文:\n{translation['translated_text']}")
            for span in spans:
                url = span["source_url"] + (span.get("anchor") or "")
                parts.append(f"对应官方原文: {span['document_title']} / {' > '.join(span['heading_path'])}\n"
                             f"原文链接: {url}\n原文行范围: {span['line_start']}-{span['line_end']}")
        else:
            for span in spans:
                url = span["source_url"] + (span.get("anchor") or "")
                label = ("本地 AI 辅助讲解片段，行号仅指登记的本地讲解文档，不是官方原文。"
                         if authored else "从官方命令帮助字符串规范化转写；定位指向原始实现中的帮助文本。"
                         if span.get("normalization") == "decoded_cpp_help_log_strings" else "官方英文原文片段。"
                         if official_tool else "课程原文片段。")
                parts.append(f"来源文件: {span['document_title']} / {' > '.join(span['heading_path'])}\n"
                             f"课程链接: {url}\n原文行范围: {span['line_start']}-{span['line_end']}\n"
                             f"片段类型: {label}\n正文:\n{span['text']}")
        passages.append(CoursePassage(reference, "\n\n".join(parts)))
    return PreparedCourse("ok" if passages else "no_match", version, tuple(passages),
                          selection_diagnostics=_retrieval_diagnostics(result))


def _retrieval_diagnostics(result):
    # Retrieval coverage describes candidates, never semantic sufficiency.
    return {"retrieval_status": result["status"],
            "source_coverage": result.get("source_coverage", []),
            "missing_sources": result.get("missing_sources", []),
            "unsupported_concept_ids": result.get("unsupported_concept_ids", []),
            "answerability": "not_assessed"}


def prepare_course_context(query: str, concept_ids: list[str] | None = None, *,
                           db_path: Path = DEFAULT_DB_PATH, top_k: int = 3,
                           candidate_limit: int | None = None) -> PreparedCourse:
    """Legacy direct callers retain Top-3. Production opts into candidates-v2."""
    if candidate_limit is None:
        return _prepare_legacy_context(query, concept_ids, db_path=db_path, top_k=top_k)
    from .retriever import retrieve_course_candidates, _term_matches
    from .chunks import digest
    result = retrieve_course_candidates(query, concept_ids, limit=candidate_limit, db_path=db_path)
    materials=[];required=[]
    terms=result.get("query_analysis",{}).get("technical_terms",[])
    codes=result.get("query_analysis",{}).get("code_identifiers",[])
    operators=[op for op in ("<=", "=") if op in query]
    wants_example=bool(re.search(r"示例|例子|代码|\bexample|\bcode\b",query,re.I))
    sources=tuple(s["source_id"] for s in result.get("query_analysis",{}).get("requested_sources",[]))
    for rank,row in enumerate(result["passages"],1):
        chunks=row.get("evidence_chunks") or [{
            "chunk_id":row["passage_id"],"parent_id":row["passage_id"],
            "content":row["content"],"content_hash":row["content_hash"],
            "source_spans":row["source_spans"]}]
        explicit_definition=any(row["title"].casefold()==code.casefold() for code in codes)
        reference_codes=set(re.findall(r":option:\x60([A-Z][A-Z0-9_]+)\x60",row["content"]))
        diagnostic_codes={code for code in codes if code.isupper()}
        cross_reference=len(diagnostic_codes)>=2 and diagnostic_codes<=reference_codes
        for chunk in chunks:
            text=chunk["content"];spans=chunk["source_spans"]
            if digest(text)!=chunk["content_hash"] or any(digest(s["text"])!=s["text_hash"] for s in spans):
                raise ValueError("course_chunk_hash_mismatch")
            if not spans:continue
            authored=row["content_type"]=="authored_note"
            whole=text==row["content"]
            translation=next((t for t in row.get("translations",[])
                if t["source_content_hash"]==chunk["content_hash"]
                and t["review_status"]=="machine_translation_unreviewed"),None) if whole else None
            translated=bool(translation and re.search(r"[\u3400-\u9fff]",query) and not authored)
            kind="authored_note" if authored else "translated_source" if translated else "source_excerpt"
            note=("AI 辅助本地讲解，未教师审阅；不是官方原文；本地行号不能作为官方行号。" if authored else
                  "未教师审阅的机器译文，非官方译文；引用对应英文原文。" if translated else
                  "固定版本原文证据；只支持此片段，不证明当前工程；项目编码风格不等于通用功能判错。")
            urls=tuple(dict.fromkeys(s["source_url"]+(s.get("anchor") or "") for s in spans))
            locators=tuple(CourseSpan(s["document_id"],s["source_url"],s["line_start"],s["line_end"],
                s["text_hash"],s.get("source_text_hash",s["text_hash"]),s["document_hash"],
                s.get("normalization"),s.get("locator_kind","pinned_source")) for s in spans)
            ref=CourseReference(chunk["chunk_id"],result["index_version"],row["title"],row["section"],
                urls[0],kind,note,urls[:8],row["course_version"],
                translation["translation_id"] if translated else None,chunk["content_hash"],
                row["passage_id"],locators)
            # Metadata is authoritative in refs; do not repeat long URLs/hashes in
            # every text fragment. Quotes themselves remain byte-for-byte source.
            quote=translation["translated_text"] if translated else text
            label="未审机器译文" if translated else "本地讲解" if authored else "原文"
            rendered=f"[{ref.passage_id}] {row['title']}\n{label}：\n{quote}"
            keys=["source:"+row["source_id"],"parent:"+row["passage_id"]]
            hits=[term for term in terms if _term_matches(term,text)]
            hits.extend(op for op in operators if op in text)
            example=bool(re.search(r"\x60{3}|\.\. (?:code|code-block)::",text))
            # Code examples must match the already-detected process topic.
            # This affects evidence-unit choice only, never parent ranking or
            # FTS filtering. An edge-triggered example cannot substitute for a
            # combinational example simply because both contain <=.
            if "combinational_logic" in result.get("matched_concept_ids",[]):
                example=example and bool(re.search(r"always\s*@\s*\*|\balways_comb\b|\bassign\b",text))
            keys.extend("term:"+term.casefold() for term in hits)
            if explicit_definition:
                key="definition:"+chunk["chunk_id"];keys.append(key);required.append(key)
            if cross_reference:
                key="cross_reference:"+row["passage_id"];keys.append(key);required.append(key)
            materials.append(CourseMaterial(ref,rendered,row["passage_id"],row["source_id"],
                tuple(dict.fromkeys(keys)),row["ranking_score"]+min(0.3,len(hits)*0.05)
                +(0.2 if wants_example and example else 0.0),rank))
    # Candidate evidence stays bounded; no gold, passage special cases, or ID boosts.
    materials.sort(key=lambda m:(-m.selection_score,m.candidate_rank,len(m.text.encode("utf-8")),m.reference.passage_id))
    return PreparedCourse("ok" if materials else "no_match",result["index_version"],tuple(materials),
        sources,tuple(dict.fromkeys(required)),tuple(r["passage_id"] for r in result["passages"]),
        tuple(result.get("unsupported_concept_ids",[])),_retrieval_diagnostics(result))


def resolve_course_refs(prepared: PreparedCourse, cited_passage_ids: Iterable[str], *,
                        admitted_passage_ids: Iterable[str]) -> tuple[CourseReference, ...]:
    """Resolve from frozen DB metadata, restricted to material actually admitted."""
    cited, admitted = list(cited_passage_ids), list(admitted_passage_ids)
    if (any(type(value) is not str for value in cited + admitted)
            or len(cited) != len(set(cited))):
        raise CitationValidationError("invalid course citation IDs")
    by_id = {p.reference.passage_id: p.reference for p in prepared.passages}
    validate_citations(admitted, by_id)
    validate_citations(cited, admitted)
    return tuple(by_id[passage_id] for passage_id in cited)
