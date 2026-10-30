"""Deterministic, evidence-bounded natural-language context for future U2 use."""
from __future__ import annotations

from collections import defaultdict

from .models import MemoryErrorBase, Scope, identifier


def _concept_label(service, concept_id):
    return getattr(service, "concept_labels", {}).get(concept_id, concept_id)


def _is_relevant(row, concepts):
    payload = row["payload"]
    if set(payload.get("concept_ids", ())) & concepts:
        return True
    fact = payload.get("u2") or {}
    return fact.get("concept_id") in concepts


def _refs(rows):
    result = {"event_ids": [], "run_ids": [], "issue_ids": [], "occurrence_ids": [],
              "session_ids": [], "evidence_refs": [], "card_ids": [], "checkpoint_refs": []}
    for row in rows:
        for field, key in (("event_id", "event_ids"), ("run_id", "run_ids"),
                           ("session_id", "session_ids"),
                           ("issue_id", "issue_ids"), ("occurrence_id", "occurrence_ids")):
            value = row.get(field)
            if value and value not in result[key]:
                result[key].append(value)
        for value in row["payload"].get("evidence_refs", ()):
            if value not in result["evidence_refs"]:
                result["evidence_refs"].append(value)
        fact = row["payload"].get("u2") or {}
        for field, key in (("card_id", "card_ids"), ("checkpoint_ref", "checkpoint_refs")):
            value = fact.get(field)
            if value and value not in result[key]:
                result[key].append(value)
    for values in result.values():
        values.sort()
    return result


def get_teaching_context(service, scope: Scope, concepts, *, max_chars=600):
    service.repo.check_scope(scope)
    if type(max_chars) is not int or not 1 <= max_chars <= 5000:
        raise MemoryErrorBase("max_chars must be 1..5000")
    if isinstance(concepts, str):
        concepts = (concepts,)
    if not isinstance(concepts, (tuple, list)) or len(concepts) > 16:
        raise MemoryErrorBase("concepts must be a bounded sequence")
    normalized = tuple(sorted(set(identifier(value) for value in concepts)))
    unknown = set(normalized) - service.concepts
    if unknown:
        raise MemoryErrorBase("unknown UA-09 concept ID")
    if not normalized:
        return {"text": "", "references": _refs([]), "concepts": []}

    # A Scope authorizes the current learner/project/instance. Context can include
    # that same scope's prior sessions, but never another learner or project.
    all_rows = service.repo.events(scope.profile_id, project_id=scope.project_id,
                                  instance_id=scope.instance_id)
    links = {row["occurrence_id"]: row for row in all_rows
             if row["event_type"] == "u2_observation_enriched"}
    relevant = [row for row in all_rows if _is_relevant(row, set(normalized))
                or (row.get("occurrence_id") in links
                    and _is_relevant(links[row["occurrence_id"]], set(normalized)))]
    if not relevant:
        return {"text": "", "references": _refs([]), "concepts": list(normalized)}

    by_run = defaultdict(list)
    for row in relevant:
        by_run[row["run_id"]].append(row)
    sentences = ["【相关教学历史】"]
    references = []
    labels = "、".join(f"“{_concept_label(service, concept)}”" for concept in normalized)
    runs = set(by_run)
    sessions = {row["session_id"] for row in relevant}
    sentence = (f"该学生在当前工程实例中有 {len(runs)} 次与 {labels} 相关的教学运行记录，"
                f"分属 {len(sessions)} 个会话。")
    candidates = [(sentence, relevant)]

    observation_rows = [row for row in relevant
                        if row["event_type"] == "u2_diagnostic_observed"]
    unique_occurrences = {row["occurrence_id"]: row for row in observation_rows if row.get("occurrence_id")}
    if unique_occurrences:
        candidates.append((f"固定报告中记录了 {len(unique_occurrences)} 次诊断观察。",
                           list(unique_occurrences.values())))
        diagnostics = sorted({((row["payload"].get("u2") or {}).get("diagnostic_id"),
                               (row["payload"].get("u2") or {}).get("external_issue_id"))
                              for row in unique_occurrences.values()})
        if diagnostics:
            labels_text = "、".join(f"诊断 {diag}（U2 问题 {issue}）" for diag, issue in diagnostics[:3])
            candidates.append((f"观察到：{labels_text}。", list(unique_occurrences.values())))
        by_diagnostic = defaultdict(set)
        for row in unique_occurrences.values():
            fact = (links.get(row["occurrence_id"], row)["payload"].get("u2") or {})
            diagnostic_id = fact.get("diagnostic_type")
            if diagnostic_id:
                by_diagnostic[diagnostic_id].add(row["occurrence_id"])
        repeated_diagnostics = [(diagnostic, len(occurrences)) for diagnostic, occurrences in by_diagnostic.items()
                                if len(occurrences) > 1]
        if repeated_diagnostics:
            repeated_diagnostics.sort()
            recurrence = "、".join(f"{diagnostic}（{count} 次报告观察）"
                                   for diagnostic, count in repeated_diagnostics[:3])
            candidates.append((f"同类诊断曾再次出现：{recurrence}；记录仍按各自 U2 问题编号区分，没有自动合并。",
                               list(unique_occurrences.values()) + [links[key] for key in unique_occurrences
                                                                   if key in links]))
        by_external_issue = defaultdict(set)
        issue_rows = []
        for row in unique_occurrences.values():
            external_id = (row["payload"].get("u2") or {}).get("external_issue_id")
            if external_id:
                by_external_issue[external_id].add(row["occurrence_id"])
                issue_rows.append(row)
        repeated = [ids for ids in by_external_issue.values() if len(ids) > 1]
        if repeated:
            candidates.append((f"其中同一 U2 问题编号关联到多个不同报告 occurrence，说明该编号下有再次观察记录。",
                               issue_rows))

    generated = [row for row in relevant if row["event_type"] == "u2_card_generated"]
    levels = sorted({(row["payload"].get("u2") or {}).get("card_level")
                     for row in generated if type((row["payload"].get("u2") or {}).get("card_level")) is int})
    if levels:
        levels_text = "、".join(f"L{level}" for level in levels)
        candidates.append((f"U2 生成过 {levels_text} 提示卡片；生成记录不能证明学生实际看到了卡片。", generated))
        # Card text is the bounded, checked U2 Card output, not a report or source body.
        # Keep only the three most recent relevant cards in the learner-facing context.
        for row in sorted(generated, key=lambda item: (item["created_at"], item["event_id"]), reverse=True)[:3]:
            fact = row["payload"].get("u2") or {}
            hint = " ".join((fact.get("card_hint") or "").split())
            question = " ".join((fact.get("card_question") or "").split())
            if hint or question:
                text = "U2 最近生成的提示内容：" + (f"“{hint}”" if hint else "（无提示正文）")
                if question:
                    text += f"追问：“{question}”"
                candidates.append((text + "。", [row]))

    accepted = [row for row in relevant if row["event_type"] == "u2_hint_accepted"]
    if accepted:
        candidates.append((f"记录到 {len(accepted)} 次 U2 已接受的进一步提示请求。", accepted))

    shown = [row for row in relevant if row["event_type"] == "hint_presented"]
    if shown:
        shown_levels = sorted({row["payload"].get("hint_level") for row in shown
                               if type(row["payload"].get("hint_level")) is int})
        levels_text = "、".join(f"L{level}" for level in shown_levels) or "未标注级别"
        candidates.append((f"另有 UI 回执确认 {levels_text} 提示已展示。", shown))
    elif generated:
        candidates.append(("当前关联记录没有 UI 展示回执，无法确认这些卡片是否显示给学生。", generated))

    for concept_id in normalized:
        summary = service.get_concept_summary(scope, concept_id)
        if summary and summary["status"] == "assisted_success":
            candidates.append((f"已有提示帮助下完成相关复测的记录；结论限于已记录的工程检查范围。",
                               [row for row in relevant if row["event_type"] in ("retry_passed", "student_modified")]))
        elif summary and summary["status"] == "independent_success":
            candidates.append((f"已有包含新复测及解释或后续练习证据的记录；这仍不代表完全掌握。",
                               [row for row in relevant if row["event_type"] in ("retry_passed", "student_explanation")]))
        elif summary and summary.get("reason") == "help_requested_delivery_unconfirmed":
            candidates.append(("学生曾请求进一步提示，但没有 UI 回执确认提示已送达；现有记录不足以判断该次复测是否独立完成。",
                               [row for row in relevant if row["event_type"] in ("u2_hint_accepted", "retry_passed")]))
        else:
            candidates.append(("当前记录没有足以支持修复结论的修改后真实复测证据，不能判断问题是否已修复或知识是否掌握。",
                               relevant))
            break

    if any(row["payload"].get("evidence_kind") == "mock" for row in relevant):
        candidates.append(("相关记录中包含 Mock/fixture；这些记录不能作为真实工程结果或真实学习成效。",
                           [row for row in relevant if row["payload"].get("evidence_kind") == "mock"]))
    if any(row["payload"].get("evidence_kind") == "imported_unverified" for row in relevant):
        candidates.append(("相关记录中包含归档导入且未经重新核验的事实，不能据此形成新的真实学习结论。",
                           [row for row in relevant if row["payload"].get("evidence_kind") == "imported_unverified"]))

    # Provenance is mandatory whenever facts are emitted. An insufficient budget
    # yields no text/references instead of an unlabelled fragment of Mock history.
    provenance = []
    if any(row["payload"].get("evidence_kind") == "mock" for row in relevant):
        provenance.append("含 Mock/fixture，仅为模拟记录。")
    if any(row["payload"].get("evidence_kind") == "imported_unverified" for row in relevant):
        provenance.append("含未核验归档，仅为历史记录。")
    prefix = "\n\n".join(provenance)
    if len(prefix) > max_chars:
        return {"text": "", "references": _refs([]), "concepts": list(normalized)}
    accepted_sentences = [prefix] if prefix else []
    used_rows = list(relevant) if prefix else []
    length = len(prefix)
    for text, rows in candidates:
        next_length = length + len(text) + (2 if accepted_sentences else 0)
        if next_length > max_chars:
            continue
        accepted_sentences.append(text)
        used_rows.extend(rows)
        length = next_length
    text = "\n\n".join(accepted_sentences)
    return {"text": text, "references": _refs(used_rows), "concepts": list(normalized)}
