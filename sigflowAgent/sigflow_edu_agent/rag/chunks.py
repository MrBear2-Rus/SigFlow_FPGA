"""Lossless source-backed evidence units; parents remain the only FTS rows."""
from __future__ import annotations
import hashlib
import json
import re
from typing import Any

CHUNK_FORMAT = "source-units-v1"


def digest(text: str) -> str:
    return "sha256:" + hashlib.sha256(text.encode("utf-8")).hexdigest()


def markdown_headings(lines: list[str]):
    fenced = None
    for index, line in enumerate(lines, 1):
        match = re.match(r"^\s*(\x60{3,}|~{3,})", line)
        if match:
            mark = match[1][0]
            if fenced is None:
                fenced = mark
            elif fenced == mark:
                fenced = None
            continue
        if fenced is None and (m := re.match(r"^(#{1,6})\s+(.+?)\s*$", line)):
            yield index, len(m[1]), m[2]


def semantic_ranges(lines: list[str], *, format_hint: str = "markdown") -> list[tuple[int, int]]:
    """Inclusive ranges at block boundaries; code/list units may exceed target."""
    if not lines:
        return []
    bullets = [i for i, s in enumerate(lines) if re.match(r"^-  \|", s)]
    options = [i for i,s in enumerate(lines) if re.match(r"^\s{4}-[A-Za-z]\S*\s*$",s)]
    if bullets or len(options)>2:
        starts = [0] + [i for i in (bullets or options) if i]
        return [(a, starts[k+1]-1 if k+1<len(starts) else len(lines)-1) for k,a in enumerate(starts)]
    blocks=[];i=0
    while i<len(lines):
        start=i
        if not lines[i].strip():i+=1;continue
        fence_pattern=r"^\s*(\x60{3,}|~{3,})" if format_hint=="markdown" else r"^\s*(\x60{3,})"
        fence=re.match(fence_pattern,lines[i])
        directive=re.match(r"^(\s*)\.\.\s+(?:code|code-block|list-table|table|warning|note|important)::",lines[i])
        item=re.match(r"^(\s*)[-*+]\s+",lines[i])
        if fence:
            mark=fence[1][0];i+=1
            while i<len(lines):
                closing=re.match(r"^\s*("+re.escape(mark)+r"{3,})",lines[i]);i+=1
                if closing:break
        elif directive:
            indent=len(directive[1]);i+=1
            while i<len(lines) and (not lines[i].strip() or len(lines[i])-len(lines[i].lstrip())>indent):i+=1
        elif item:
            indent=len(item[1]);i+=1
            while i<len(lines):
                s=lines[i]
                level=len(s)-len(s.lstrip())
                if not s.strip() or level>indent or (indent>0 and level==indent) or re.match(r"^\s*[-*+]\s+",s):i+=1;continue
                break
        else:
            i+=1
            while i<len(lines) and lines[i].strip() and not re.match(
                    (r"^\s*(?:\x60{3,}|~{3,}|\.\.\s+(?:code|code-block)::)" if format_hint=="markdown"
                     else r"^\s*(?:\x60{3,}|\.\.\s+(?:code|code-block)::)"),lines[i]):i+=1
        blocks.append((start,i-1,bool(fence or directive or item or '|' in lines[start])))
    units=[];k=0
    while k<len(blocks):
        a,b,special=blocks[k]
        if k+1<len(blocks) and blocks[k+1][2] and not special:k+=1;b=blocks[k][1];special=True
        if special and k+1<len(blocks) and not blocks[k+1][2]:
            following="\n".join(lines[blocks[k+1][0]:blocks[k+1][1]+1]).strip()
            if re.match(r"(?i)(note|however|this |these |the above|in this|otherwise|unless|except|注意|但是|因此|否则)",following):k+=1;b=blocks[k][1]
        units.append((a,b));k+=1
    merged=[]
    for a,b in units:
        if merged and len("\n".join(lines[merged[-1][0]:b+1]).encode("utf-8"))<=1200:merged[-1]=(merged[-1][0],b)
        else:merged.append((a,b))
    if merged:
        merged[0]=(0,merged[0][1])
        merged=[(a,merged[k+1][0]-1 if k+1<len(merged) else len(lines)-1) for k,(a,b) in enumerate(merged)]
    return merged


def build_chunks(parent: dict[str, Any], spans: list[dict[str, Any]],
                 documents: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    """Disjoint source partition. Normalized C++ pieces retain raw log locators."""
    all_lines=[];mapping=[]
    for si,span in enumerate(spans):
        if si:all_lines.append("");mapping.append(None)
        doc=documents[span["document_id"]]
        source=doc["lines"][span["line_start"]-1:span["line_end"]]
        lines=span["text"].splitlines()
        if span.get("normalization")=="decoded_cpp_help_log_strings":
            decoded=[]
            for raw_line,s in enumerate(source,span["line_start"]):
                m=re.search(r'\blog\("((?:\\.|[^"\\])*)"\);',s)
                if m:
                    piece=json.loads('"'+m[1]+'"')
                    decoded.extend((v,raw_line) for v in piece.splitlines())
            while decoded and not decoded[0][0].strip():decoded.pop(0)
            while decoded and not decoded[-1][0].strip():decoded.pop()
            if "\n".join(v for v,_ in decoded).strip()!=span["text"].strip():raise ValueError("chunk_help_normalization_mismatch")
            # First/last strip in help extraction affects whitespace, not line identity.
            for n,line in enumerate(lines):all_lines.append(line);mapping.append((si,decoded[n][1]))
        else:
            offset=0
            while offset<len(source) and source[offset]=="" and source[offset:offset+len(lines)]!=lines:offset+=1
            if source[offset:offset+len(lines)]!=lines:raise ValueError("chunk_source_line_mismatch: "+parent["passage_id"])
            for n,line in enumerate(lines):all_lines.append(line);mapping.append((si,span["line_start"]+offset+n))
    rst=any(documents[s["document_id"]].get("source_path","").endswith(".rst") for s in spans)
    ranges=[(0,len(all_lines)-1)] if len(parent["content"].encode("utf-8"))<=2048 else semantic_ranges(
        all_lines,format_hint="rst" if rst else "markdown")
    chunks=[]
    for a,b in ranges:
        selected=[];i=a
        while i<=b:
            if mapping[i] is None:i+=1;continue
            si,start=mapping[i];j=i
            while j+1<=b and mapping[j+1] is not None and mapping[j+1][0]==si:j+=1
            end=mapping[j][1];original=spans[si];doc=documents[original["document_id"]]
            text="\n".join(all_lines[i:j+1]);raw="\n".join(doc["lines"][start-1:end])
            note=parent["content_type"]=="authored_note"
            url=("sigflow-note:///"+original["document_id"]) if note else original["source_url"]
            anchor=f"#L{start}-L{end}" if "github.com/" in url else original.get("anchor") or ""
            selected.append(dict(document_id=original["document_id"],source_url=url,anchor=anchor,line_start=start,line_end=end,
                text=text,text_hash=digest(text),source_text_hash=digest(raw),document_hash=doc["document_hash"],
                normalization=original.get("normalization"),locator_kind="local_authored_note" if note else "pinned_source",
                heading_path=original["heading_path"]))
            i=j+1
        text="\n\n".join(s["text"] for s in selected)
        if not text.strip():continue
        identity=json.dumps([parent["passage_id"],[(s["document_id"],s["line_start"],s["line_end"],s["text_hash"]) for s in selected]],ensure_ascii=False,separators=(",",":"))
        cid=parent["passage_id"]+"--c"+hashlib.sha256(identity.encode("utf-8")).hexdigest()[:12]
        chunks.append(dict(chunk_id=cid,parent_id=parent["passage_id"],ordinal=len(chunks),content=text,
            content_hash=digest(text),source_spans=selected,indivisible_over_target=len(text.encode("utf-8"))>2048))
    return chunks
