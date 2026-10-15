from __future__ import annotations

import json
import re
import sqlite3
from pathlib import Path
from typing import Any

from .database import DEFAULT_DB_PATH, _normalize_alias, open_database


HAN_AND_WORDS_RE = re.compile(r"[\u3400-\u9fffA-Za-z0-9_]+")
CODE_IDENTIFIER_RE = re.compile(r"%Warning-[A-Za-z0-9_]+|[A-Za-z_][A-Za-z0-9_]*")
FTS_STOP_WORDS = {
    "a", "about", "after", "also", "an", "and", "are", "as", "at", "be", "because", "before", "being",
    "between", "but", "by", "can", "could", "does", "for", "from", "have", "how", "i", "if", "in", "into",
    "is", "it", "me", "my", "of", "on", "or", "our", "please", "should", "so", "that", "the", "their",
    "them", "then", "these", "this", "those", "through", "to", "use", "using", "was", "we", "were", "what",
    "when", "where", "which", "while", "who", "why", "will", "with", "would", "your", "official", "source",
    "sources", "document", "documents", "cite", "cites", "citation", "citations", "reference", "references",
    "question", "following", "example", "examples", "please", "code", "documentation", "guide", "version",
    "begin", "end", "if", "else", "always", "logic", "wire", "reg", "module", "assign",
    "systemverilog", "verilog", "best", "way", "practice", "practices",
    "lowrisc", "yosys", "verilator", "primer", "coding", "style", "rules", "rule",
}
CJK_QUERY_STOP_NGRAMS = {
    "下面的", "下面为", "为什么", "是什么", "怎么修", "怎样修", "如何修", "分别查", "分别引", "分别检",
    "请分别", "官方资", "官方文", "官方原", "给出原", "引用原", "原文引", "资料中", "这两个", "是不是",
    "我看到", "我作为", "应该遵", "怎么理", "怎么修", "没有实", "不要声", "发生了", "有什么", "怎么区",
}
INTERNAL_CANDIDATE_LIMIT = 512
DIAGNOSTIC_RANKING_LIMIT = 10


def _query_concepts(connection: sqlite3.Connection, query: str) -> list[str]:
    normalized_query = _normalize_alias(query)
    rows = connection.execute(
        "SELECT concept_id, alias, alias_norm FROM concept_aliases ORDER BY length(alias_norm) DESC, concept_id"
    ).fetchall()
    matches: list[str] = []
    for row in rows:
        alias = row["alias_norm"]
        if alias and _term_matches(row["alias"], query) and row["concept_id"] not in matches:
            matches.append(row["concept_id"])
    # In HDL, these two assignment operators form a common explicit contrast.
    # Resolve it only when the surrounding question is about assignment or
    # combinational logic, so an ordinary comparison does not become a hit.
    if ("<=" in query and "=" in query
            and re.search(r"assignment|赋值|combinational|组合逻辑", query, flags=re.IGNORECASE)
            and "blocking_nonblocking" not in matches):
        matches.append("blocking_nonblocking")
    # A common latch question describes the incomplete branch without using
    # the word "latch": a value is assigned in an if branch inside
    # always_comb, so it must retain its previous value when the condition is
    # false. Resolve that intent from the code construct and assignment
    # relationship, not from a particular passage or test wording.
    conditional_cue = (
        re.search(r"\bif\s*\(", query, flags=re.IGNORECASE)
        or re.search(r"只有.{0,24}(?:为真|成立|满足)|(?:条件|分支).{0,20}(?:为真|成立|赋值)|当.{0,20}(?:为真|成立)", query)
    )
    assignment_cue = re.search(r"\bassign(?:ed|ment|s)?\b|赋值|执行", query, flags=re.IGNORECASE)
    if re.search(r"\balways_comb\b", query, flags=re.IGNORECASE) and conditional_cue and assignment_cue:
        for concept_id in ("latch", "complete_assignment", "combinational_logic"):
            if concept_id not in matches:
                matches.append(concept_id)
    # An explicitly intentional latch design calls for the source's
    # always_latch guidance. This is a query expansion, not a passage ID rule.
    if ("latch" in matches and re.search(r"故意|有意|intentional|deliberate", query, flags=re.IGNORECASE)
            and "sequential_logic" not in matches):
        matches.append("sequential_logic")
    if (re.search(r"仿真", query) and re.search(r"处理顺序|事件顺序|执行顺序", query)
            and re.search(r"结果.{0,8}(?:不同|不一致)|(?:不同|不一致).{0,8}结果", query)
            and "simulation_semantics" not in matches):
        matches.append("simulation_semantics")
    explicit_warning_code = any(
        token.isupper() and len(token) >= 5 and token not in {"ASIC", "CPU", "EDA", "FPGA", "GPU", "HDL", "IEEE", "RTL", "VLSI"}
        for token in CODE_IDENTIFIER_RE.findall(query)
    )
    if (re.search(r"\bwarning\b|\bdiagnostic\b|\berror\b|告警|诊断", query, flags=re.IGNORECASE)
            or explicit_warning_code
            and "tool_diagnostic" not in matches):
        if "tool_diagnostic" not in matches:
            matches.append("tool_diagnostic")
    return matches


def _source_label_norms(connection: sqlite3.Connection) -> set[str]:
    """Return labels that identify sources but do not describe passage content."""
    labels: set[str] = set()
    for row in connection.execute("SELECT source_id, title, author_or_source FROM sources"):
        prefix = row["source_id"].split("_", 1)[0]
        for value in (row["source_id"], prefix, row["title"], row["author_or_source"]):
            normalized = _normalize_alias(value)
            if normalized:
                labels.add(normalized)
        if "/" in row["author_or_source"]:
            normalized = _normalize_alias(row["author_or_source"].split("/", 1)[0])
            if normalized:
                labels.add(normalized)
    # These source display names are stable aliases used in local questions.
    labels.update({"lowriscstyleguide", "lowrisccodingstyle", "yosysprimer", "verilatorwarnings"})
    return labels


def _source_mentions(connection: sqlite3.Connection, query: str) -> list[dict[str, str]]:
    normalized_query = _normalize_alias(query)
    mentions: list[tuple[int, dict[str, str]]] = []
    for row in connection.execute("SELECT source_id, title, author_or_source FROM sources"):
        source_id = row["source_id"]
        aliases = {source_id, source_id.split("_", 1)[0], row["title"], row["author_or_source"]}
        if "/" in row["author_or_source"]:
            aliases.add(row["author_or_source"].split("/", 1)[0])
        if source_id == "lowrisc_style":
            aliases.update(("lowrisc", "lowRISC style guide", "lowRISC coding style"))
        elif source_id == "yosys_047":
            aliases.update(("Yosys", "Yosys primer"))
        elif source_id == "verilator_5052":
            aliases.update(("Verilator", "Verilator warnings"))

        matched: list[tuple[int, str]] = []
        for alias in aliases:
            normalized_alias = _normalize_alias(alias)
            if len(normalized_alias) < 4 or normalized_alias not in normalized_query:
                continue
            position = _normalize_alias(query[: max(0, query.casefold().find(alias.casefold()))])
            matched.append((len(position), alias))
        if matched:
            mentions.append((min(position for position, _ in matched), {"source_id": source_id, "name": row["title"]}))
    mentions.sort(key=lambda item: (item[0], item[1]["source_id"]))
    return [item[1] for item in mentions]


def _query_terms(
    connection: sqlite3.Connection,
    query: str,
    direct_concepts: list[str],
    source_mentions: list[dict[str, str]],
) -> dict[str, list[str]]:
    normalized_query = _normalize_alias(query)
    source_names = {mention["source_id"].split("_", 1)[0] for mention in source_mentions}
    source_label_norms = _source_label_norms(connection)
    alias_rows = connection.execute(
        "SELECT concept_id, alias, alias_norm FROM concept_aliases ORDER BY length(alias_norm) DESC, concept_id"
    ).fetchall()
    concept_alias_norms = {row["alias_norm"] for row in alias_rows if row["alias_norm"]}
    concept_terms = []
    observed_concept_terms = []
    matched_aliases: dict[str, list[str]] = {concept_id: [] for concept_id in direct_concepts}
    all_aliases: dict[str, list[str]] = {concept_id: [] for concept_id in direct_concepts}
    for row in alias_rows:
        concept_id = row["concept_id"]
        if concept_id not in direct_concepts:
            continue
        all_aliases[concept_id].append(row["alias"])
        if _term_matches(row["alias"], query):
            matched_aliases[concept_id].append(row["alias"])
    uses_chinese = bool(re.search(r"[\u3400-\u9fff]", query))
    for concept_id in direct_concepts:
        aliases = matched_aliases[concept_id]
        if aliases:
            observed_concept_terms.extend(aliases[:2])
        if not aliases:
            aliases = [
                alias for alias in all_aliases[concept_id]
                if bool(re.search(r"[\u3400-\u9fff]", alias)) == uses_chinese
            ][:2]
        concept_terms.extend(aliases[:2])

    mapped_terms = []
    for row in connection.execute("SELECT title_zh, terms_json, questions_json FROM passage_mappings"):
        values = [row["title_zh"]] + json.loads(row["terms_json"]) + json.loads(row["questions_json"])
        for value in values:
            normalized = _normalize_alias(value)
            if len(normalized) < 4 or not _term_matches(value, query):
                continue
            value_tokens = [_normalize_alias(token) for token in HAN_AND_WORDS_RE.findall(value.casefold())]
            if value_tokens and all(token in FTS_STOP_WORDS for token in value_tokens):
                continue
            if normalized in concept_alias_norms:
                continue
            if normalized in source_label_norms or any(name and normalized == name for name in source_names):
                continue
            mapped_terms.append(value)

    code_identifiers = []
    natural_terms = []
    common_acronyms = {"ASIC", "CPU", "EDA", "FPGA", "GPU", "HDL", "IEEE", "RTL", "VLSI"}
    for token in CODE_IDENTIFIER_RE.findall(query):
        normalized = _normalize_alias(token)
        if not normalized or normalized in source_names:
            continue
        is_warning_code = token.startswith("%Warning-") or (
            token.isupper() and len(token) >= 5 and token not in common_acronyms
            and bool(re.fullmatch(r"[A-Z][A-Z0-9_]+", token))
        )
        if (len(normalized) >= 3 and token.casefold() not in FTS_STOP_WORDS) or is_warning_code:
            destination = code_identifiers if is_warning_code or "_" in token else natural_terms
            destination.append(token)

    for token in HAN_AND_WORDS_RE.findall(query.casefold()):
        if any("\u3400" <= character <= "\u9fff" for character in token):
            continue
        normalized = _normalize_alias(token)
        if len(normalized) < 3 or token in FTS_STOP_WORDS or normalized in source_names:
            continue
        if token not in code_identifiers:
            natural_terms.append(token)

    if ("latch" in direct_concepts
            and re.search(r"故意|有意|intentional|deliberate", query, flags=re.IGNORECASE)
            and "always_latch" not in code_identifiers):
        code_identifiers.append("always_latch")

    cjk_terms = []
    for run in re.findall(r"[\u3400-\u9fff]{3,}", query):
        for index in range(len(run) - 2):
            ngram = run[index:index + 3]
            if ngram not in CJK_QUERY_STOP_NGRAMS:
                cjk_terms.append(ngram)

    terms: list[str] = []
    seen: set[str] = set()
    for value in concept_terms + mapped_terms + code_identifiers + natural_terms + cjk_terms:
        normalized = _normalize_alias(value)
        if normalized and normalized not in seen:
            seen.add(normalized)
            terms.append(value)
    scoring_terms: list[str] = []
    scoring_seen: set[str] = set()
    for value in observed_concept_terms + mapped_terms + code_identifiers + natural_terms[:16]:
        normalized = _normalize_alias(value)
        if normalized and normalized not in scoring_seen:
            scoring_seen.add(normalized)
            scoring_terms.append(value)
    return {
        "technical_terms": terms,
        "concept_terms": list(dict.fromkeys(concept_terms)),
        "observed_concept_terms": list(dict.fromkeys(observed_concept_terms)),
        "mapping_terms": list(dict.fromkeys(mapped_terms)),
        "code_identifiers": list(dict.fromkeys(code_identifiers)),
        "natural_terms": list(dict.fromkeys(natural_terms)),
        "scoring_terms": scoring_terms,
        "fallback_cjk_terms": list(dict.fromkeys(cjk_terms)),
    }


def _query_trigrams(terms: list[str]) -> list[str]:
    grams: list[str] = []
    for term in terms:
        for token in HAN_AND_WORDS_RE.findall(term.casefold()):
            if len(token) >= 3:
                for index in range(len(token) - 2):
                    gram = token[index:index + 3]
                    if gram not in grams:
                        grams.append(gram)
    return grams[:96]


def _trigram_query(terms: list[str]) -> str:
    grams = _query_trigrams(terms)
    return " OR ".join('"' + gram.replace('"', '""') + '"' for gram in grams)


def _term_matches(term: str, candidate: str) -> bool:
    normalized = _normalize_alias(term)
    if not normalized:
        return False
    if any("\u3400" <= character <= "\u9fff" for character in term):
        return normalized in _normalize_alias(candidate)
    term_tokens = [_normalize_alias(token) for token in HAN_AND_WORDS_RE.findall(term.casefold())]
    candidate_tokens = {_normalize_alias(token) for token in HAN_AND_WORDS_RE.findall(candidate.casefold())}
    def singular(token: str) -> str:
        if len(token) > 4 and token.endswith("ies"):
            return token[:-3] + "y"
        if len(token) > 4 and token.endswith(("ches", "shes", "xes", "zes", "sses")):
            return token[:-2]
        if len(token) > 3 and token.endswith("s") and not token.endswith("ss"):
            return token[:-1]
        return token
    candidate_roots = {singular(token) for token in candidate_tokens}
    return bool(term_tokens) and all(singular(token) in candidate_roots for token in term_tokens)


def _filtered_clause(course_id: str | None, language: str | None) -> tuple[str, list[Any]]:
    clauses: list[str] = []
    params: list[Any] = []
    if course_id:
        clauses.append("p.course_id = ?")
        params.append(course_id)
    if language:
        if language == "zh-CN":
            # Chinese queries can match locally mapped English source passages;
            # authored notes retain their declared language.
            clauses.append("(p.language = ? OR (p.language = 'en' AND p.content_type = 'source_excerpt'))")
            params.append(language)
        else:
            clauses.append("p.language = ?")
            params.append(language)
    return (" AND " + " AND ".join(clauses) if clauses else "", params)


def _fetch_passage_rows(connection: sqlite3.Connection, passage_ids: list[str]) -> dict[str, sqlite3.Row]:
    if not passage_ids:
        return {}
    placeholders = ",".join("?" for _ in passage_ids)
    rows = connection.execute(f"SELECT * FROM passages WHERE passage_id IN ({placeholders})", passage_ids).fetchall()
    return {row["passage_id"]: row for row in rows}


def _passage_payload(row: sqlite3.Row, match_reason: str, matched_concepts: list[str]) -> dict[str, Any]:
    return {
        "passage_id": row["passage_id"],
        "course_id": row["course_id"],
        "course_version": row["course_version"],
        "title": row["title"],
        "section": row["section"],
        "concept_ids": json.loads(row["concept_ids_json"]),
        "content": row["content"],
        "content_type": row["content_type"],
        "reviewed_summary": row["reviewed_summary"],
        "evidence_status": row["evidence_status"],
        "evidence_note": row["evidence_note"],
        "source_id": row["source_id"],
        "source_url": row["source_url"],
        "anchor": row["anchor"],
        "author_or_source": row["author_or_source"],
        "license_or_usage_note": row["license_or_usage_note"],
        "retrieved_at": row["retrieved_at"],
        "content_hash": row["content_hash"],
        "language": row["language"],
        "difficulty": row["difficulty"],
        "match_reason": match_reason,
        "matched_concept_ids": matched_concepts,
    }


def _related_concepts(connection: sqlite3.Connection, direct_concepts: list[str]) -> list[str]:
    related: list[str] = []
    if not direct_concepts:
        return related
    rows = connection.execute(
        f"SELECT concept_id, related_json FROM concepts WHERE concept_id IN ({','.join('?' for _ in direct_concepts)})",
        direct_concepts,
    ).fetchall()
    direct_set = set(direct_concepts)
    for row in rows:
        for concept_id in json.loads(row["related_json"]):
            if concept_id not in direct_set and concept_id not in related:
                related.append(concept_id)
    return related


def _retrieve_course_passages(
    query: str,
    concept_ids: list[str] | None = None,
    course_id: str | None = None,
    language: str | None = None,
    top_k: int = 3,
    *,
    db_path: Path = DEFAULT_DB_PATH,
) -> dict[str, Any]:
    """Retrieve grounded course excerpts and a separate practice list."""
    if not isinstance(query, str) or not query.strip():
        raise ValueError("query must be a non-empty string")
    if isinstance(top_k, bool) or not isinstance(top_k, int) or not 1 <= top_k <= 20:
        raise ValueError("candidate limit must be an integer between 1 and 20")
    if concept_ids is not None and (not isinstance(concept_ids, list) or not all(isinstance(c, str) and c for c in concept_ids)):
        raise ValueError("concept_ids must be a list of non-empty strings or None")
    if language is not None and language not in {"zh-CN", "en"}:
        raise ValueError("language must be `zh-CN`, `en`, or None")

    db_path = Path(db_path)
    connection = open_database(db_path)
    try:
        version_row = connection.execute("SELECT value FROM index_meta WHERE key='index_version'").fetchone()
        index_version = version_row[0] if version_row else None
        tokenizer_row = connection.execute("SELECT value FROM index_meta WHERE key='fts_tokenizer'").fetchone()
        tokenizer = tokenizer_row[0] if tokenizer_row else None
        known_concepts = {row[0] for row in connection.execute("SELECT concept_id FROM concepts")}
        explicit = list(dict.fromkeys(concept_ids or []))
        source_mentions = _source_mentions(connection, query)
        requested_sources = list(dict.fromkeys(item["source_id"] for item in source_mentions))
        unknown = [concept_id for concept_id in explicit if concept_id not in known_concepts]
        if unknown:
            raise ValueError("unknown concept_ids: " + ", ".join(unknown))
        direct = list(dict.fromkeys(explicit + _query_concepts(connection, query)))
        if direct:
            placeholders = ",".join("?" for _ in direct)
            supported = {row[0] for row in connection.execute(
                "SELECT DISTINCT pc.concept_id FROM passage_concepts pc "
                "JOIN passages p ON p.passage_id = pc.passage_id "
                f"WHERE p.evidence_status = 'grounded' AND pc.concept_id IN ({placeholders})",
                direct,
            )}
            unsupported = [concept_id for concept_id in direct if concept_id not in supported]
            if unsupported and (not supported or set(explicit).intersection(unsupported)):
                return {
                    "status": "no_match", "passages": [], "related_practices": [],
                    "index_version": index_version,
                    "retrieved_passage_ids": [], "matched_concept_ids": direct,
                    "unsupported_concept_ids": unsupported, "agent_context": "",
                    "candidate_rankings": [],
                    "query_analysis": {"requested_sources": source_mentions, "technical_terms": [], "code_identifiers": []},
                    "source_coverage": [
                        {"source_id": item["source_id"], "status": "no_relevant_candidate", "candidate_count": 0, "returned_count": 0}
                        for item in source_mentions
                    ],
                    "missing_sources": requested_sources,
                    "fts_tokenizer": connection.execute(
                        "SELECT value FROM index_meta WHERE key = 'fts_tokenizer'"
                    ).fetchone()[0],
                }
        related = _related_concepts(connection, direct)
        term_groups = _query_terms(connection, query, direct, source_mentions)
        technical_terms = term_groups["technical_terms"]
        fts_query = _trigram_query(technical_terms)
        candidates: dict[str, dict[str, Any]] = {}
        filter_sql, filter_params = _filtered_clause(course_id, language)
        filter_sql += " AND p.evidence_status = 'grounded'"

        def candidate_for(passage_id: str) -> dict[str, Any]:
            return candidates.setdefault(passage_id, {
                "direct_concepts": set(), "related_concepts": set(), "mapping_terms": set(), "fts_rank": None,
            })

        # Named sources are isolated during candidate generation. Each source
        # contributes only passages actually supported by concepts, exact local
        # mappings, or FTS text matches; mentioning a source alone never creates a hit.
        source_scopes: list[str | None] = requested_sources or [None]
        source_label_norms = _source_label_norms(connection)
        concept_alias_norms = {
            row[0] for row in connection.execute("SELECT DISTINCT alias_norm FROM concept_aliases") if row[0]
        }
        source_names = {mention["source_id"].split("_", 1)[0] for mention in source_mentions}
        for source_id in source_scopes:
            scoped_filter = filter_sql + (" AND p.source_id = ?" if source_id else "")
            scoped_params = filter_params + ([source_id] if source_id else [])
            for ids, destination in ((direct, "direct_concepts"), (related, "related_concepts")):
                if not ids:
                    continue
                sql = (
                    "SELECT p.passage_id, pc.concept_id FROM passages p "
                    "JOIN passage_concepts pc ON pc.passage_id = p.passage_id "
                    f"WHERE pc.concept_id IN ({','.join('?' for _ in ids)}){scoped_filter}"
                )
                for row in connection.execute(sql, ids + scoped_params):
                    candidate_for(row["passage_id"])[destination].add(row["concept_id"])

            # Mappings are exact phrase routes. Generic prompt words and source
            # names are excluded upstream so they cannot fan out into false hits.
            mapping_sql = (
                "SELECT pm.passage_id, pm.terms_json, pm.questions_json FROM passage_mappings pm "
                "JOIN passages p ON p.passage_id = pm.passage_id "
                f"WHERE p.evidence_status = 'grounded'{_filtered_clause(course_id, language)[0]}"
                + (" AND p.source_id = ?" if source_id else "")
            )
            mapping_params = _filtered_clause(course_id, language)[1] + ([source_id] if source_id else [])
            for row in connection.execute(mapping_sql, mapping_params):
                mapped = json.loads(row["terms_json"]) + json.loads(row["questions_json"])
                matched_terms = {
                    term for term in mapped
                    if len(_normalize_alias(term)) >= 4
                    and _normalize_alias(term) not in source_label_norms
                    and _normalize_alias(term) not in concept_alias_norms
                    and _normalize_alias(term) not in source_names
                    and _term_matches(term, query)
                }
                if matched_terms:
                    candidate_for(row["passage_id"])["mapping_terms"].update(matched_terms)

            if fts_query:
                weights = "8.0, 3.0, 2.0, 3.0, 1.0"
                fts_sql = (
                    f"SELECT p.passage_id, p.source_id, bm25(passage_fts, {weights}) AS fts_rank "
                    "FROM passage_fts JOIN passages p ON p.passage_id = passage_fts.passage_id "
                    f"WHERE passage_fts MATCH ?{scoped_filter} ORDER BY fts_rank LIMIT ?"
                )
                for row in connection.execute(fts_sql, [fts_query] + scoped_params + [INTERNAL_CANDIDATE_LIMIT]):
                    item = candidate_for(row["passage_id"])
                    if item["fts_rank"] is None or row["fts_rank"] < item["fts_rank"]:
                        item["fts_rank"] = row["fts_rank"]

        passage_rows = _fetch_passage_rows(connection, list(candidates))
        mappings_by_id: dict[str, dict[str, Any]] = {}
        if candidates:
            placeholders = ",".join("?" for _ in candidates)
            for row in connection.execute(
                f"SELECT * FROM passage_mappings WHERE passage_id IN ({placeholders})", list(candidates)
            ):
                mappings_by_id[row["passage_id"]] = row

        fts_ranks = [item["fts_rank"] for item in candidates.values() if item["fts_rank"] is not None]
        min_fts = min(fts_ranks, default=0.0)
        max_fts = max(fts_ranks, default=0.0)
        source_priority = {
            row["source_id"]: (0 if row["source_type"] == "public_course" else 1)
            for row in connection.execute("SELECT source_id, source_type FROM sources")
        }
        for passage_id, item in candidates.items():
            passage = passage_rows[passage_id]
            mapping = mappings_by_id.get(passage_id)
            mapping_text = ""
            if mapping is not None:
                mapping_text = " ".join(
                    [mapping["title_zh"]] + json.loads(mapping["terms_json"]) + json.loads(mapping["questions_json"])
                )
            body_text = " ".join((passage["title"], passage["section"], passage["content"]))
            all_text = body_text + " " + mapping_text
            scoring_terms = term_groups["scoring_terms"]
            cjk_terms = term_groups["fallback_cjk_terms"]
            body_hits = [term for term in scoring_terms if _term_matches(term, body_text)]
            mapped_hits = sorted(item["mapping_terms"])
            exact_hits = list(dict.fromkeys(body_hits + mapped_hits))
            cjk_hits = [term for term in cjk_terms if _term_matches(term, body_text)]
            direct_hits = sorted(item["direct_concepts"])
            related_hits = sorted(item["related_concepts"])
            concept_score = (
                len(direct_hits) / max(1, len(direct)) if direct_hits
                else 0.58 * len(related_hits) / max(1, len(related)) if related_hits
                else 0.0
            )
            if item["fts_rank"] is None or max_fts == min_fts:
                bm25_score = 0.0
            else:
                bm25_score = (max_fts - item["fts_rank"]) / (max_fts - min_fts)
            lexical_hits = len(set(body_hits)) if scoring_terms else len(set(cjk_hits))
            lexical_denominator = 3.0 if scoring_terms else 4.0
            content_score = 0.62 * min(1.0, lexical_hits / lexical_denominator) + 0.38 * bm25_score
            exact_denominator = max(1, min(3, len(scoring_terms))) if scoring_terms else 4
            exact_score = min(1.0, (len(set(exact_hits)) if scoring_terms else len(set(cjk_hits))) / exact_denominator)
            title_code_hits = [
                code for code in term_groups["code_identifiers"] if _term_matches(code, passage["title"])
            ]
            code_score = 1.0 if title_code_hits else 0.0
            source_score = 1.0 if requested_sources and passage["source_id"] in requested_sources else 0.0
            title_text = " ".join((passage["title"], passage["section"], mapping["title_zh"] if mapping else ""))
            local_title_terms = [passage["title"]]
            if mapping is not None:
                local_title_terms.extend([mapping["title_zh"]] + json.loads(mapping["terms_json"]))
            title_terms = list(dict.fromkeys(
                term_groups["observed_concept_terms"] + term_groups["code_identifiers"]
                + [term for term in local_title_terms if _term_matches(term, query)]
            ))
            title_hits = [term for term in title_terms if _term_matches(term, title_text)]
            title_score = min(1.0, len(title_hits) / max(1, min(2, len(title_terms))))
            score = (
                0.17 * content_score + 0.30 * concept_score + 0.13 * exact_score
                + 0.25 * code_score + 0.12 * title_score + 0.03 * source_score
            )
            diagnostic_penalty = (
                0.20 if not requested_sources and "tool_diagnostic" not in direct
                and "tool_diagnostic" in json.loads(passage["concept_ids_json"]) else 0.0
            )
            authored_note_penalty = (
                0.16 if passage["content_type"] == "authored_note"
                and any(passage_rows[other_id]["source_id"] == passage["source_id"]
                        and passage_rows[other_id]["content_type"] == "source_excerpt"
                        for other_id in candidates if other_id != passage_id)
                else 0.0
            )
            reason = (
                "exact_concept" if direct_hits else "local_bilingual_mapping" if item["mapping_terms"]
                else "related_concept" if related_hits else "fts_text"
            )
            item.update({
                "source_id": passage["source_id"],
                "matched": sorted(set(direct_hits + related_hits)),
                "direct_hits": direct_hits,
                "related_hits": related_hits,
                "exact_term_hits": sorted(set(exact_hits)),
                "fallback_cjk_hits": sorted(set(cjk_hits)),
                "code_title_hits": title_code_hits,
                "title_term_hits": title_hits,
                "reason": reason,
                "ranking_score": score - diagnostic_penalty - authored_note_penalty - 0.002 * source_priority.get(passage["source_id"], 2),
                "score_components": {
                    "content_relevance": content_score,
                    "concept_relevance": concept_score,
                    "exact_terminology": exact_score,
                    "code_identifier_title_match": code_score,
                    "title_relevance": title_score,
                    "source_constraint": source_score,
                    "tool_diagnostic_intent_penalty": diagnostic_penalty,
                    "authored_note_priority_penalty": authored_note_penalty,
                    "fts_bm25": item["fts_rank"],
                },
            })

        # FTS can return a row from a shared short trigram. Keep it only if its
        # actual text or local metadata contains at least one extracted term;
        # two independent hits are required when there is no concept/mapping route.
        filtered_candidates: dict[str, dict[str, Any]] = {}
        for passage_id, item in candidates.items():
            has_semantic_route = bool(item["direct_hits"] or item["related_hits"] or item["mapping_terms"])
            code_hit = any(_term_matches(term, " ".join((passage_rows[passage_id]["title"], passage_rows[passage_id]["content"])))
                           for term in term_groups["code_identifiers"])
            minimum_hits = 1 if code_hit else 2
            lexical_hit = (
                len(item["exact_term_hits"]) >= minimum_hits
                if term_groups["scoring_terms"]
                else len(item["fallback_cjk_hits"]) >= 3
            )
            if has_semantic_route or lexical_hit:
                filtered_candidates[passage_id] = item
        candidates = filtered_candidates

        ordered_all_ids = sorted(
            candidates,
            key=lambda passage_id: (
                -candidates[passage_id]["ranking_score"],
                -len(candidates[passage_id]["direct_hits"]),
                -len(candidates[passage_id]["exact_term_hits"]),
                candidates[passage_id]["fts_rank"] if candidates[passage_id]["fts_rank"] is not None else 0.0,
                1 if passage_id.split(":", 2)[-1].startswith("section-") else 0,
                passage_id,
            ),
        )
        ordered_all_ids = ordered_all_ids[:INTERNAL_CANDIDATE_LIMIT]
        ordered_ids = list(ordered_all_ids)
        if len(requested_sources) > 1:
            reserved: list[str] = []
            for source_id in requested_sources:
                best = next((pid for pid in ordered_all_ids if candidates[pid]["source_id"] == source_id), None)
                if best is not None and best not in reserved:
                    reserved.append(best)
            ordered_ids = reserved + [pid for pid in ordered_all_ids if pid not in reserved]
        ordered_ids = ordered_ids[:top_k]
        score_rank_by_id = {passage_id: rank for rank, passage_id in enumerate(ordered_all_ids, 1)}

        candidate_rankings = [
            {
                "rank": rank,
                "score_rank": score_rank_by_id[passage_id],
                "passage_id": passage_id,
                "source_id": candidates[passage_id]["source_id"],
                "match_reason": candidates[passage_id]["reason"],
                "matched_concept_ids": candidates[passage_id]["matched"],
                "exact_term_hits": candidates[passage_id]["exact_term_hits"],
                "fallback_cjk_hits": candidates[passage_id]["fallback_cjk_hits"],
                "code_title_hits": candidates[passage_id]["code_title_hits"],
                "title_term_hits": candidates[passage_id]["title_term_hits"],
                "ranking_score": candidates[passage_id]["ranking_score"],
                "score_components": candidates[passage_id]["score_components"],
            }
            for rank, passage_id in enumerate(ordered_all_ids[:DIAGNOSTIC_RANKING_LIMIT], 1)
        ]
        if len(requested_sources) > 1:
            candidate_rankings = [
                {
                    "rank": rank,
                    "score_rank": score_rank_by_id[passage_id],
                    "passage_id": passage_id,
                    "source_id": candidates[passage_id]["source_id"],
                    "match_reason": candidates[passage_id]["reason"],
                    "matched_concept_ids": candidates[passage_id]["matched"],
                    "exact_term_hits": candidates[passage_id]["exact_term_hits"],
                    "fallback_cjk_hits": candidates[passage_id]["fallback_cjk_hits"],
                    "code_title_hits": candidates[passage_id]["code_title_hits"],
                    "title_term_hits": candidates[passage_id]["title_term_hits"],
                    "ranking_score": candidates[passage_id]["ranking_score"],
                    "score_components": candidates[passage_id]["score_components"],
                }
                for rank, passage_id in enumerate(ordered_ids + [pid for pid in ordered_all_ids if pid not in ordered_ids], 1)
                if rank <= DIAGNOSTIC_RANKING_LIMIT
            ]
        candidate_counts = {source_id: 0 for source_id in requested_sources}
        returned_counts = {source_id: 0 for source_id in requested_sources}
        for item in candidates.values():
            if item["source_id"] in candidate_counts:
                candidate_counts[item["source_id"]] += 1
        for passage_id in ordered_ids:
            source_id = candidates[passage_id]["source_id"]
            if source_id in returned_counts:
                returned_counts[source_id] += 1
        source_coverage = [
            {
                "source_id": source_id,
                "status": "covered" if returned_counts[source_id] else "omitted_by_top_k" if candidate_counts[source_id] else "no_relevant_candidate",
                "candidate_count": candidate_counts[source_id],
                "returned_count": returned_counts[source_id],
            }
            for source_id in requested_sources
        ]
        missing_sources = [
            item["source_id"] for item in source_coverage if item["status"] == "no_relevant_candidate"
        ]
        passage_rows = _fetch_passage_rows(connection, ordered_ids)
        mapping_by_id: dict[str, dict[str, Any]] = {}
        translations_by_id: dict[str, list[dict[str, Any]]] = {passage_id: [] for passage_id in ordered_ids}
        if ordered_ids:
            placeholders = ",".join("?" for _ in ordered_ids)
            for row in connection.execute(
                "SELECT * FROM passage_mappings WHERE passage_id IN (" + placeholders + ")", ordered_ids
            ):
                mapping_by_id[row["passage_id"]] = {
                    "title_zh": row["title_zh"], "terms": json.loads(row["terms_json"]),
                    "questions_zh": json.loads(row["questions_json"]),
                    "concept_ids": json.loads(row["concept_ids_json"]), "review_status": row["review_status"],
                }
            for row in connection.execute(
                "SELECT * FROM passage_translations WHERE passage_id IN (" + placeholders + ") ORDER BY translation_id",
                ordered_ids,
            ):
                translations_by_id[row["passage_id"]].append({
                    "translation_id": row["translation_id"], "source_content_hash": row["source_content_hash"],
                    "translated_text": row["translated_text"], "generation_method": row["generation_method"],
                    "review_status": row["review_status"],
                })
        source_refs: dict[str, list[dict[str, Any]]] = {passage_id: [] for passage_id in ordered_ids}
        source_spans: dict[str, list[dict[str, Any]]] = {passage_id: [] for passage_id in ordered_ids}
        if ordered_ids:
            placeholders = ",".join("?" for _ in ordered_ids)
            for ref in connection.execute(
                "SELECT passage_id, source_id, source_url, anchor, reference_kind "
                f"FROM passage_sources WHERE passage_id IN ({placeholders}) ORDER BY passage_id, reference_order",
                ordered_ids,
            ):
                source_refs[ref["passage_id"]].append(
                    {
                        "source_id": ref["source_id"],
                        "source_url": ref["source_url"],
                        "anchor": ref["anchor"],
                        "reference_kind": ref["reference_kind"],
                    }
                )
            for span in connection.execute(
                "SELECT ps.*, sd.chapter, sd.source_url, sd.document_hash, sd.title AS document_title "
                "FROM passage_spans ps JOIN source_documents sd ON sd.document_id = ps.document_id "
                f"WHERE ps.passage_id IN ({placeholders}) ORDER BY ps.passage_id, ps.span_order",
                ordered_ids,
            ):
                source_spans[span["passage_id"]].append({
                    "document_id": span["document_id"], "document_title": span["document_title"],
                    "chapter": span["chapter"], "source_url": span["source_url"], "anchor": span["anchor"],
                    "line_start": span["line_start"],
                    "line_end": span["line_end"], "heading_path": json.loads(span["heading_path_json"]),
                    "text": span["text"], "text_hash": span["text_hash"],
                    "document_hash": span["document_hash"], "character_count": span["character_count"],
                    "source_text_hash": span["source_text_hash"], "normalization": span["normalization"],
                })
        passages = []
        for passage_id in ordered_ids:
            payload = _passage_payload(
                passage_rows[passage_id], candidates[passage_id]["reason"], candidates[passage_id]["matched"]
            )
            payload["source_references"] = source_refs[passage_id]
            payload["source_spans"] = source_spans[passage_id]
            if payload["content_type"] == "authored_note":
                for span in payload["source_spans"]:
                    span["source_url"] = "sigflow-note:///" + span["document_id"]
                    span["anchor"] = f"#L{span['line_start']}-L{span['line_end']}"
                    span["locator_kind"] = "local_authored_note"
                if payload["source_spans"]:
                    payload["source_url"] = payload["source_spans"][0]["source_url"]
                    payload["anchor"] = payload["source_spans"][0]["anchor"]
                for ref in payload["source_references"]:
                    ref["reference_kind"] = "supporting_official_reference"
            payload["retrieval_mapping"] = mapping_by_id.get(passage_id)
            payload["translations"] = translations_by_id[passage_id]
            payload["ranking_score"] = candidates[passage_id]["ranking_score"]
            payload["score_components"] = candidates[passage_id]["score_components"]
            passages.append(payload)
        if connection.execute("SELECT 1 FROM sqlite_master WHERE name='evidence_chunks'").fetchone():
            for payload in passages:
                payload["evidence_chunks"] = [
                    {**dict(chunk), "source_spans": json.loads(chunk["spans_json"])}
                    for chunk in connection.execute(
                        "SELECT * FROM evidence_chunks WHERE parent_id=? ORDER BY ordinal",
                        (payload["passage_id"],))]

        practice_concepts = list(dict.fromkeys(direct + related))
        practices: list[dict[str, Any]] = []
        if practice_concepts:
            practice_rows = connection.execute(
                f"SELECT pr.*, GROUP_CONCAT(pc.concept_id) AS matched "
                "FROM practices pr JOIN practice_concepts pc ON pc.practice_id = pr.practice_id "
                f"WHERE pc.concept_id IN ({','.join('?' for _ in practice_concepts)}) "
                "GROUP BY pr.practice_id",
                practice_concepts,
            ).fetchall()
            direct_set = set(direct)
            ordered_practices = sorted(
                practice_rows,
                key=lambda row: (
                    0 if set(row["matched"].split(",")) & direct_set else 1,
                    row["practice_id"],
                ),
            )[:top_k]
            practices = [
                {
                    "practice_id": row["practice_id"],
                    "title": row["title"],
                    "source": row["source"],
                    "source_url": row["source_url"],
                    "concept_ids": sorted(set(row["matched"].split(","))),
                    "language": row["language"],
                    "difficulty": row["difficulty"],
                    "note": row["note"],
                }
                for row in ordered_practices
            ]

        result = {
            "index_version": index_version,
            "status": "ok" if passages else "no_match",
            "passages": passages,
            "related_practices": practices,
            "retrieved_passage_ids": [row["passage_id"] for row in passages],
            "matched_concept_ids": direct,
            "fts_tokenizer": tokenizer,
            "candidate_rankings": candidate_rankings,
            "candidate_count": len(candidates),
            "candidate_limit": INTERNAL_CANDIDATE_LIMIT,
            "query_analysis": {
                "requested_sources": source_mentions,
                "technical_terms": term_groups["scoring_terms"],
                "code_identifiers": term_groups["code_identifiers"],
                "natural_terms": term_groups["natural_terms"],
            },
            "source_coverage": source_coverage,
            "missing_sources": missing_sources,
        }
        result["agent_context"] = format_agent_context(result)
        return result
    finally:
        connection.close()


def retrieve_course_passages(query: str, concept_ids: list[str] | None = None,
                             course_id: str | None = None, language: str | None = None,
                             top_k: int = 3, *, db_path: Path = DEFAULT_DB_PATH) -> dict[str, Any]:
    """Legacy Top-3 view; ranking/filter semantics remain unchanged."""
    if isinstance(top_k, bool) or not isinstance(top_k, int) or not 1 <= top_k <= 3:
        raise ValueError("top_k must be an integer between 1 and 3")
    return _retrieve_course_passages(query, concept_ids, course_id, language, top_k, db_path=db_path)


def retrieve_course_candidates(query: str, concept_ids: list[str] | None = None, *,
                               limit: int = 20, db_path: Path = DEFAULT_DB_PATH) -> dict[str, Any]:
    """v2 candidate interface, independent of final evidence/context size."""
    result = _retrieve_course_passages(query, concept_ids, top_k=limit, db_path=db_path)
    result["retrieval_interface"] = "candidates-v2"
    # A recall pool is not a ready-to-send teaching context. Only the budgeted
    # provider/packer may expose actual Agent evidence.
    result["agent_context"] = ""
    result["context_status"] = "not_packed"
    return result


def format_agent_context(result: dict[str, Any]) -> str:
    """Give the teaching Agent only locally verified source text and DB-backed locators."""
    blocks = []
    for passage in result.get("passages", []):
        if passage.get("evidence_status") != "grounded":
            continue
        parts = [f"[{passage['passage_id']}] {passage['title']}"]
        is_authored_note = passage.get("content_type") == "authored_note"
        is_official_tool = not is_authored_note and passage.get("source_id") in {
            "lowrisc_style", "yosys_047", "verilator_5052"
        }
        if is_authored_note:
            parts.append("材料属性: AI 辅助原创讲解，待团队和教师审阅；不是官方原文或官方翻译。")
        elif is_official_tool:
            parts.append(f"材料属性: 官方工具资料英文原文，资料版本 {passage['course_version']}；不能证明当前工程事实，编码风格不自动构成功能判错标准。")
        else:
            parts.append("材料属性: 从登记的课程正文范围提取的原文摘录。")
        for span in passage.get("source_spans", []):
            heading = " > ".join(span["heading_path"])
            url = span["source_url"] + (span["anchor"] or "")
            text_label = "讲解内容" if is_authored_note else "原文摘录"
            if span.get("normalization") == "decoded_cpp_help_log_strings":
                text_label = "从官方命令帮助字符串规范化转写"
            parts.append(
                f"来源: {span['chapter']} / {heading}\n"
                f"课程链接: {url}\n原文行号: {span['line_start']}-{span['line_end']}\n"
                f"{text_label}:\n{span['text']}"
            )
        blocks.append("\n\n".join(parts))
    return "\n\n---\n\n".join(blocks)
