"""Pinned, allow-listed official source parser for the first tool-doc course batch.

Only the files named in the source plan are read.  The generated JSONL resources
keep each source span tied to the exact upstream file and line interval.
"""
from __future__ import annotations

import hashlib
import json
import re
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any

from .database import RESOURCE_ROOT

PROJECT_ROOT = Path(__file__).resolve().parents[2]
RAW_ROOT = PROJECT_ROOT / "data" / "external_sources"
RESOURCE_DIR = RESOURCE_ROOT
RETRIEVED_AT = "2026-10-13"
COMMITS = {
    "lowrisc": "9c15ff5dce23eef969e00ab3715153967419eadd",
    "yosys": "647d61dd9212365a3cd44db219660b8f90b95cbd",
    "verilator": "ea338be98e1e838d3518809ce8899f85a009963c",
}
BASE_FILES = {
    "lowrisc/9c15ff5/VerilogCodingStyle.md",
    "lowrisc/9c15ff5/README.md",
    "lowrisc/9c15ff5/LICENSE",
    "yosys/0.47/docs/source/appendix/primer.rst",
    "yosys/0.47/COPYING",
    "yosys/0.47/passes/cmds/check.cc",
    "yosys/0.47/frontends/verilog/verilog_frontend.cc",
    "verilator/v5.052/LICENSE",
    "verilator/v5.052/docs/guide/warnings.rst",
    "verilator/v5.052/docs/guide/languages.rst",
}
INCLUDE_RE = re.compile(r"^\s*\.\. include::\s+([^\s]+)")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.+?)\s*$")
RST_OPTION_RE = re.compile(r"^\.\. option::\s+(.+?)\s*$")
RST_UNDERLINE_RE = re.compile(r"^([=\-~^`:+#*])\1{2,}\s*$")


def digest_bytes(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def digest_text(value: str) -> str:
    return digest_bytes(value.encode("utf-8"))


def _safe_source_path(value: str) -> str:
    path = PurePosixPath(value.replace("\\", "/"))
    if path.is_absolute() or ".." in path.parts or not path.parts:
        raise ValueError(f"unsafe official source path: {value}")
    return path.as_posix()


def _source_url(source_path: str) -> str:
    parts = PurePosixPath(source_path).parts
    if parts[0] == "lowrisc":
        return f"https://github.com/lowRISC/style-guides/blob/{COMMITS['lowrisc']}/" + "/".join(parts[2:])
    if parts[0] == "yosys":
        return f"https://github.com/YosysHQ/yosys/blob/{COMMITS['yosys']}/" + "/".join(parts[2:])
    repository_path = "/".join(parts[2:])
    return f"https://github.com/verilator/verilator/blob/{COMMITS['verilator']}/" + repository_path


def _raw_path(source_path: str) -> Path:
    safe = _safe_source_path(source_path)
    target = (RAW_ROOT / Path(*PurePosixPath(safe).parts)).resolve()
    if not target.is_relative_to(RAW_ROOT.resolve()) or not target.is_file():
        raise ValueError(f"allow-listed official source file is missing: {safe}")
    return target


def _read_source(source_path: str) -> tuple[str, list[str], bytes]:
    data = _raw_path(source_path).read_bytes()
    text = data.decode("utf-8")
    return text, text.splitlines(), data


def _warning_include_paths() -> set[str]:
    path = "verilator/v5.052/docs/guide/warnings.rst"
    _, lines, _ = _read_source(path)
    result = set()
    for line in lines:
        match = INCLUDE_RE.match(line)
        if not match:
            continue
        include = PurePosixPath(match.group(1))
        if include.is_absolute() or ".." not in include.parts:
            raise ValueError(f"unexpected Verilator include outside guide-relative path: {match.group(1)}")
        name = include.name
        if not re.fullmatch(r"ex_[A-Za-z0-9_]+\.rst", name):
            raise ValueError(f"unexpected Verilator generated include: {name}")
        result.add("verilator/v5.052/docs/gen/" + name)
    return result


def fixed_source_paths() -> list[str]:
    paths = sorted(BASE_FILES | _warning_include_paths())
    for path in paths:
        _raw_path(path)
    return paths


def _license_fields(source_path: str) -> tuple[str, str, str]:
    if source_path.startswith("lowrisc/"):
        return ("CC-BY-4.0", "explicit_repository_readme", "lowrisc/9c15ff5/README.md",
                "README.md in the pinned repository states all files use CC BY 4.0; preserve attribution and license notice.")
    if source_path.startswith("yosys/"):
        if source_path.endswith("/COPYING"):
            return ("ISC", "explicit_license_file", "yosys/0.47/COPYING", "Pinned Yosys COPYING file states the ISC license.")
        if source_path.endswith("/passes/cmds/check.cc") or source_path.endswith("/frontends/verilog/verilog_frontend.cc"):
            return ("ISC", "explicit_file_notice", source_path,
                    "The source file itself carries the ISC permission and warranty notice; only its help string is normalized into the course passage.")
        return ("ISC", "repository_inherited_judgment", "yosys/0.47/COPYING",
                "This documentation file has no per-file license header; applicability of the pinned repository ISC COPYING to the prose is an inherited-license judgment, pending review.")
    if source_path.endswith("/LICENSE"):
        return ("LGPL-3.0-only OR Artistic-2.0", "explicit_license_file", "verilator/v5.052/LICENSE", "Pinned Verilator LICENSE declares the project dual license.")
    if "/docs/guide/" in source_path:
        return ("LGPL-3.0-only OR Artistic-2.0", "explicit_file_spdx", source_path,
                "This guide file itself declares SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0.")
    return ("LGPL-3.0-only OR Artistic-2.0", "repository_inherited_unconfirmed", "verilator/v5.052/LICENSE", "Generated docs/gen snippet has no per-file license header; repository-level license inheritance is not confirmed for generated excerpts. Cached locally; do not redistribute until reviewed.")


def source_file_catalog() -> list[dict[str, Any]]:
    records = []
    for source_path in fixed_source_paths():
        _, lines, data = _read_source(source_path)
        stat = _raw_path(source_path).stat()
        created_at = getattr(stat, "st_birthtime", stat.st_ctime)
        local_file_created_at = datetime.fromtimestamp(created_at, timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
        parts = PurePosixPath(source_path).parts
        repo = {"lowrisc": "lowRISC/style-guides", "yosys": "YosysHQ/yosys", "verilator": "verilator/verilator"}[parts[0]]
        version = {"lowrisc": COMMITS["lowrisc"], "yosys": "0.47", "verilator": "v5.052"}[parts[0]]
        license_id, license_basis, license_evidence_path, license_note = _license_fields(source_path)
        records.append({
            "source_path": source_path, "repository_path": "/".join(parts[2:]),
            "source_id": {"lowrisc": "lowrisc_style", "yosys": "yosys_047", "verilator": "verilator_5052"}[parts[0]],
            "repository": repo, "version": version, "commit": COMMITS[parts[0]],
            "source_url": _source_url(source_path), "retrieved_at": RETRIEVED_AT,
            "retrieved_at_precision": "date_only; exact network transfer time was not captured",
            "local_file_created_at_utc": local_file_created_at,
            "sha256": digest_bytes(data), "bytes": len(data), "line_count": len(lines),
            "license_id": license_id, "license_basis": license_basis,
            "license_evidence_path": license_evidence_path, "license_note": license_note,
            "indexed": not (source_path.endswith("_fixed.rst") or source_path.endswith("/LICENSE") or source_path.endswith("/COPYING") or source_path.endswith("/README.md")),
            "review_status": "source_provenance_verified_pending_teacher_review",
        })
    return records


def _slug(value: str) -> str:
    value = value.casefold().replace("&", " and ").replace("'", "")
    value = re.sub(r"[^a-z0-9]+", "-", value).strip("-")
    return (value or "topic")[:70].strip("-")


def _markdown_sections(lines: list[str]) -> list[tuple[list[str], int, int, str]]:
    from .chunks import markdown_headings
    headings = list(markdown_headings(lines))
    return _leaf_heading_sections(headings, len(lines))


def _rst_sections(lines: list[str]) -> list[tuple[list[str], int, int, str]]:
    headings: list[tuple[int, int, str]] = []
    for index in range(1, len(lines)):
        title, underline = lines[index - 1].strip(), lines[index].strip()
        if title and RST_UNDERLINE_RE.fullmatch(underline) and len(underline) >= len(title):
            char = underline[0]
            rank = "=-~^`:+#*".index(char) + 1
            if len(title) < 160:
                headings.append((index, rank, title))
    return _leaf_heading_sections(headings, len(lines))


def _leaf_heading_sections(
    headings: list[tuple[int, int, str]], line_count: int
) -> list[tuple[list[str], int, int, str]]:
    """Return disjoint heading-owned body ranges, including parent introductions.

    Historical name retained for import compatibility. A parent stops before
    its first child; descendants are never duplicated in the FTS parent row.
    """
    result = []
    stack: list[tuple[int, int, str]] = []
    for index, (start, depth, title) in enumerate(headings):
        while stack and stack[-1][1] >= depth:
            stack.pop()
        stack.append((start, depth, title))

        end = headings[index + 1][0] - 1 if index + 1 < len(headings) else line_count
        result.append(([item[2] for item in stack], start, end, title))
    return result


def _concepts(title: str, text: str, source_id: str) -> list[str]:
    value = (title + " " + text[:3000]).casefold()
    if source_id == "yosys_047" and title.casefold().startswith("read_verilog "):
        return ["tool_command"]
    if source_id == "yosys_047" and title.casefold().startswith("check "):
        return ["multiple_driver", "undriven_signal", "tool_command", "tool_diagnostic"]
    if title.casefold().strip() == "module instantiation":
        return ["port_connection"]
    if re.fullmatch(r"(?:lexing and )?parsing", title.casefold().strip()):
        return ["syntax_parsing"]
    ids: list[str] = []
    warning_code = title.strip().upper()
    is_warning_entry = source_id == "verilator_5052" and bool(
        re.fullmatch(r"[A-Z][A-Z0-9]{2,}", warning_code)
    )
    if is_warning_entry:
        ids.append("tool_diagnostic")
        # Warning descriptions contain many unrelated examples and cross-links.
        # Classify by the diagnostic's own code so an incidental phrase (for
        # example, "syntax errors" in UNUSEDSIGNAL's example) cannot create a
        # misleading concept mapping.
        warning_concepts = {
            "WIDTH": ("bit_width",), "WIDTHCONCAT": ("bit_width",),
            "WIDTHEXPAND": ("bit_width", "truncation_extension"),
            "WIDTHTRUNC": ("bit_width", "truncation_extension"),
            "WIDTHXZEXPAND": ("bit_width", "truncation_extension"),
            "ENUMITEMWIDTH": ("bit_width",),
            "LATCH": ("latch", "combinational_logic", "complete_assignment"),
            "NOLATCH": ("latch",), "ALWCOMBORDER": ("latch", "combinational_logic"),
            "CASEINCOMPLETE": ("latch", "complete_assignment"),
            "BLKANDNBLK": ("blocking_nonblocking", "combinational_logic", "sequential_logic"),
            "BLKLOOPINIT": ("blocking_nonblocking", "reset"),
            "BLKSEQ": ("blocking_nonblocking", "sequential_logic"),
            "FINALDLY": ("blocking_nonblocking",), "INITIALDLY": ("blocking_nonblocking",),
            "COMBDLY": ("combinational_logic",), "ALWNEVER": ("combinational_logic",),
            "DETECTARRAY": ("combinational_logic",), "DIDNOTCONVERGE": ("combinational_logic",),
            "UNOPTFLAT": ("combinational_logic",),
            "MULTIDRIVEN": ("multiple_driver",),
            "UNDRIVEN": ("undriven_signal",),
            "MULTIDRIVENPROC": ("multiple_driver", "sequential_logic"),
            "PINMISSING": ("port_connection",), "PINNOCONNECT": ("port_connection",),
            "PINCONNECTEMPTY": ("port_connection",), "NULLPORT": ("port_connection",),
            "CDCRSTLOGIC": ("reset",), "PROCASSINIT": ("reset",), "SYNCASYNCNET": ("reset",),
            "FSMMULTI": ("fsm",),
        }
        ids.extend(warning_concepts.get(warning_code, ()))
    else:
        rules = [
            (r"\bbit[- ]?width\b|\bwidths?\s+(?:of (?:number |numeric )?literals?|mismatch|warning|error|overflow|truncation|extension)\b|\btruncat\w*\b|\b(?:zero|sign)(?:ed)?[- ](?:extend|expand)\w*\b", "bit_width"),
            (r"\btruncat\w*\b|\b(?:zero|sign)(?:ed)?[- ](?:extend|expand)\w*\b", "truncation_extension"),
            (r"\bsignedness\b|\bsigned\b|\bunsigned\b", "signed_unsigned"),
            (r"\blatch(?:es|ed|ing)?\b|always_latch", "latch"),
            (r"\bcombinational\b|\bcombinatorial\b|always_comb|always\s*@\s*\*", "combinational_logic"),
            (r"\bblocking assignments?\b|\bnon[- ]blocking assignments?\b", "blocking_nonblocking"),
            (r"\breset\b|\brst\b", "reset"),
            (r"\bsequential\b|\bregisters?\b|flip[- ]?flops?\b|clock edge|posedge", "sequential_logic"),
            (r"multiple drivers?|multi[- ]?driven", "multiple_driver"),
            (r"\bundriven\b|\bno (?:driver|source)\b", "undriven_signal"),
            (r"\bport connection\b|\bpin(?: connection)?\b|module port|port width", "port_connection"),
            (r"\bsyntax errors?\b|\bparse errors?\b|\bparser errors?\b", "syntax_error"),
            (r"\bfront[- ]end parser\b", "syntax_parsing"),
            (r"\bwarning\b|\bdiagnostic\b|\bfatal error\b|%warning|%error", "tool_diagnostic"),
            (r"\bfsm\b|finite state machine|state machine", "fsm"),
            (r"complete assignment|incomplete assignment|not assigned in all control paths|assigned on every branch|missing case item", "complete_assignment"),
        ]
        for pattern, concept_id in rules:
            if re.search(pattern, value) and concept_id not in ids:
                ids.append(concept_id)
    fallback = {"lowrisc_style": "verilog_style", "yosys_047": "digital_synthesis", "verilator_5052": "tool_diagnostic"}[source_id]
    return list(dict.fromkeys(ids)) or [fallback]


ZH_TITLES = {
    "summary": "本节要点摘要", "terminology conventions": "术语约定", "default to c-like formatting": "采用接近 C 语言的格式",
    "style guide exceptions": "规范例外", "which verilog to use": "使用哪种 Verilog 语言版本",
    "file extensions": "文件扩展名", "characters": "字符编码约定", "posix file endings": "POSIX 换行符",
    "line length": "行长度限制", "no tabs": "不要使用制表符", "no trailing spaces": "不要保留行尾空格",
    "begin / end": "begin/end 代码块格式", "indented sections": "代码块缩进", "line wrapping": "长行换行",
    "preprocessor directives": "预处理指令格式", "comma-delimited lists": "逗号分隔列表格式", "tabular alignment": "表格化对齐",
    "expressions": "表达式格式", "array dimensions in declarations": "声明中的数组维度", "parameterized types": "参数化类型格式",
    "labels": "标签命名和格式", "case items": "case 分支格式", "function and task calls": "函数和任务调用格式",
    "macro calls": "宏调用格式", "line continuation": "续行格式", "space around keywords": "关键字周围的空格",
    "ternary expressions": "三目表达式格式", "comments": "注释格式", "declarations": "声明格式",
    "basic template": "基本代码模板", "parameterized objects (modules, etc.)": "模块等参数化对象命名",
    "macro definitions": "宏定义格式", "suffixes": "后缀命名约定", "enumerations": "枚举定义格式",
    "use descriptive names": "使用有含义的名称", "prefixes": "前缀命名约定", "hierarchical consistency": "层次命名一致性",
    "clocks": "时钟信号约定", "resets": "复位信号约定", "preferred systemverilog constructs": "优先使用的 SystemVerilog 构造",
    "package dependencies": "package 依赖顺序", "module declaration": "模块声明格式", "module instantiation": "模块实例化格式",
    "constants": "常量定义格式", "always be explicit about the widths of number literals.": "数字字面量必须显式写明位宽",
    "port connections on module instances must always match widths correctly.": "模块实例端口连接的位宽必须匹配",
    "do not use multi-bit signals in a boolean context.": "布尔条件中不要直接使用多位信号",
    "bit slicing": "位选和切片格式", "handling width overflow": "位宽溢出的处理",
    "blocking and non-blocking assignments": "阻塞赋值与非阻塞赋值", "delay modeling": "延迟建模",
    "sequential logic (latches)": "时序逻辑中的锁存器", "sequential logic (registers)": "时序逻辑中的寄存器",
    "catching errors where invalid values are consumed": "在无效值被使用时捕获错误",
    "specific guidance on case statements and ternaries": "case 语句和三目表达式的具体约定",
    "dynamic array indexing": "动态数组索引", "combinational logic": "组合逻辑", "wildcards in case items": "case 分支中的通配符",
    "generate constructs": "generate 结构", "signed arithmetic": "有符号算术", "number formatting": "数字格式化",
    "functions and tasks": "函数和任务", "floating begin-end blocks": "多余的 begin/end 块",
    "hierarchical references": "层次化引用", "declare all signals": "显式声明所有信号",
    "use `logic` for synthesis": "综合代码使用 logic 类型", "logical vs. bitwise": "逻辑运算与按位运算",
    "packed ordering": "packed 维度顺序", "unpacked ordering": "unpacked 维度顺序",
    "finite state machines": "有限状态机约定", "active-low signals": "低有效信号命名",
    "differential pairs": "差分信号命名", "delays": "延迟信号命名", "wildcard import of packages": "避免通配符导入 package",
    "a note on security critical applications": "安全关键应用说明", "basic style elements": "基本风格要素",
    "construct naming": "语言结构命名", "suffixes for signals and types": "信号和类型的后缀",
    "language features": "语言特性选择",
    "system level": "系统级抽象", "high level": "高层抽象", "behavioural level": "行为级抽象",
    "register-transfer level (rtl)": "寄存器传输级（RTL）抽象", "logical gate level": "逻辑门级抽象",
    "physical gate level": "物理门级抽象", "switch level": "开关级抽象", "yosys": "Yosys 综合工具",
    "structural verilog": "结构化 Verilog 描述", "expressions in verilog": "Verilog 表达式",
    "behavioural modelling": "行为建模", "functions and tasks": "函数和任务建模",
    "conditionals, loops and generate-statements": "条件语句、循环和 generate 语句",
    "arrays and memories": "数组和存储器", "standards compliance": "语言标准符合性",
    "optimizations": "综合优化", "technology mapping": "目标架构技术映射",
    "script-based synthesis flows": "基于脚本的综合流程", "lexing and parsing": "词法分析与语法解析",
    "multi-pass compilation": "多遍编译流程", "static single assignment (ssa) form": "静态单赋值（SSA）形式",
    "verilog 2001 (ieee 1364-2001) support": "Verilog-2001（IEEE 1364-2001）支持范围",
    "verilog 2005 (ieee 1364-2005) support": "Verilog-2005（IEEE 1364-2005）支持范围",
    "systemverilog 2005 (ieee 1800-2005) support": "SystemVerilog-2005（IEEE 1800-2005）支持范围",
    "systemverilog 2012 (ieee 1800-2012) support": "SystemVerilog-2012（IEEE 1800-2012）支持范围",
    "systemverilog 2017 (ieee 1800-2017) support": "SystemVerilog-2017（IEEE 1800-2017）支持范围",
    "systemverilog 2023 (ieee 1800-2023) support": "SystemVerilog-2023（IEEE 1800-2023）支持范围",
    "verilog ams support": "Verilog-AMS 支持范围", "synthesis directive assertion support": "综合指令中的断言支持",
    "synthesis subset": "可综合语言子集", "signal naming": "信号命名规则", "bind": "bind 语句支持范围",
    "class": "类语法支持范围", "dotted cross-hierarchy references": "跨层次点号引用支持",
    "latches": "锁存器支持范围", "structures and unions": "结构体和联合体支持范围",
    "unknown states": "X/Z 未知状态语义", "tri/inout": "tri 与 inout 支持范围",
    "gate primitives": "门级原语支持范围", "specify blocks": "specify 块支持范围",
    "array initialization": "数组初始化支持范围", "array out of bounds": "数组越界行为",
    "assertions": "断言支持范围", "coverage": "功能覆盖率支持范围", "encrypted verilog": "加密 Verilog 支持范围",
}

WARNING_TITLES_ZH = {
    "WIDTH": "位宽不匹配", "WIDTHTRUNC": "位宽截断", "WIDTHEXPAND": "零扩展位宽告警",
    "WIDTHXZEXPAND": "X/Z 扩展位宽告警", "PINMISSING": "实例端口未连接", "PINNOCONNECT": "引脚未连接",
    "MULTIDRIVEN": "信号存在多个驱动源", "MULTIDRIVENPROC": "过程块之间存在多个驱动源",
    "UNDRIVEN": "信号没有驱动源", "LATCH": "推断出锁存器", "NOLATCH": "预期的锁存器未推断",
    "CASEINCOMPLETE": "case 分支不完整", "CASEOVERLAP": "case 分支重叠", "BLKANDNBLK": "阻塞与非阻塞赋值混用",
    "ASSIGNDLY": "连续赋值中的延迟", "NEEDTIMINGOPT": "需要选择时序处理选项", "GENCLK": "历史时钟生成告警",
    "UNUSEDSIGNAL": "信号未使用", "UNUSEDPARAM": "参数未使用", "UNUSEDGENVAR": "genvar 未使用",
    "UNUSEDLOOP": "循环变量未使用", "INFINITELOOP": "检测到无限循环", "UNOPTFLAT": "平坦化优化未完成",
    "DEPRECATED": "使用了已弃用构造", "MULTITOP": "检测到多个顶层模块", "MODMISSING": "模块定义缺失",
    "EOFNEWLINE": "文件末尾缺少换行", "MISINDENT": "缩进不一致", "SIMILARNAME": "标识符名称相似",
}


def _mapping(title: str, text: str, concepts: list[str], source_id: str, code: str | None = None) -> dict[str, Any]:
    title_lower = title.casefold().strip()
    concept_names = {
        "bit_width": ("位宽", "宽度匹配", "位宽不匹配"), "signed_unsigned": ("有符号无符号", "signed unsigned"),
        "latch": ("锁存器", "锁存", "latch"),
        "combinational_logic": ("组合逻辑", "组合逻辑赋值", "组合过程", "combinational logic"), "reset": ("复位", "重置"),
        "sequential_logic": ("时序逻辑", "寄存器", "sequential logic"), "multiple_driver": ("多重驱动",),
        "undriven_signal": ("未驱动", "无驱动源"), "tool_command": ("工具命令",),
        "syntax_parsing": ("语法解析", "前端解析"),
        "port_connection": ("端口连接", "引脚连接"), "syntax_error": ("语法错误",),
        "tool_diagnostic": ("工具告警", "诊断信息"), "fsm": ("有限状态机", "状态机"),
        "complete_assignment": ("赋值不完整", "不完整赋值", "完整赋值", "分支覆盖"),
        "blocking_nonblocking": ("阻塞赋值", "非阻塞赋值", "阻塞与非阻塞赋值", "blocking assignment", "non-blocking assignment"),
        "verilog_style": ("Verilog 编码风格", "代码格式"),
        "digital_synthesis": ("逻辑综合", "综合流程"),
    }
    title_zh = ZH_TITLES.get(title_lower)
    if source_id == "lowrisc_style":
        title_zh = title_zh or (concept_names.get(concepts[0], ("RTL 编码",))[0] + "相关约定")
        chinese_title = "lowRISC 编码规范｜" + title_zh
    elif source_id == "yosys_047":
        if title_lower.startswith("check "):
            title_zh = "check 命令：检查当前设计中的结构问题"
        elif title_lower.startswith("read_verilog "):
            title_zh = "read_verilog 命令：读取 Verilog 文件的选项"
        title_zh = title_zh or (concept_names.get(concepts[0], ("综合流程",))[0] + "主题")
        chinese_title = "Yosys 0.47｜" + title_zh
    else:
        if code:
            title_zh = WARNING_TITLES_ZH.get(code, "告警说明")
            chinese_title = f"Verilator v5.052 告警 {code}｜{title_zh}"
        elif title_lower in {"internal error", "unsupported: ...."}:
            chinese_title = "Verilator v5.052｜" + ("内部错误" if title_lower == "internal error" else "不支持的构造")
        else:
            title_zh = title_zh or "语言特性支持范围"
            chinese_title = "Verilator v5.052｜" + title_zh
    terms = [title]
    questions = [f"{name} 是什么意思？" for concept_id in concepts for name in concept_names.get(concept_id, ())[:1]][:2]
    if "complete_assignment" in concepts:
        questions.extend(["赋值不完整为什么会推断锁存器？", "为什么某些分支没有赋值会保留旧值？"])
    if "blocking_nonblocking" in concepts:
        questions.extend(["阻塞赋值和非阻塞赋值有什么区别？", "blocking and non-blocking assignments 有什么区别？"])
    if source_id == "verilator_5052" and code:
        terms.extend([code, "%Warning-" + code])
        questions.insert(0, f"Verilator 报 {code} 告警是什么意思？")
    if not questions:
        questions = [f"{title} 要求什么？", f"什么时候需要注意 {title}？"]
    if "bit_width" in concepts:
        questions.extend(["为什么这里会有位宽告警？", "位宽不一致会截断还是扩展？"])
    if re.search(r"syntax|read_verilog|语法", title_lower + text[:200].casefold()):
        questions.extend(["Yosys 为什么解析不了这段 Verilog？", "read_verilog 能检查语法吗？"])
    return {"title_zh": chinese_title, "terms": list(dict.fromkeys(terms + [t for c in concepts for t in concept_names.get(c, ())])),
            "questions_zh": list(dict.fromkeys(questions))[:4], "concept_ids": concepts}


def _source_record(source_path: str, start: int, end: int, *, normalized_text: str | None = None) -> dict[str, Any]:
    chapter = "external:" + source_path.replace("/", ":").replace(".", "-")
    _, lines, _ = _read_source(source_path)
    source_text = "\n".join(lines[start - 1:end])
    return {"chapter": chapter, "source_path": source_path, "line_start": start, "line_end": end,
            "source_text_hash": digest_text(source_text), "anchor": f"#L{start}-L{end}",
            **({"normalized_text": normalized_text, "normalization": "decoded_cpp_help_log_strings"} if normalized_text is not None else {})}


def _add_passage(rows: list[dict[str, Any]], span_rows: list[dict[str, Any]], docs: dict[str, dict[str, Any]],
                 *, source_id: str, topic: str, source_path: str, title: str, heading: list[str],
                 ranges: list[dict[str, Any]], difficulty: str = "intermediate", code: str | None = None,
                 identities: dict | None = None) -> None:
    if not ranges:
        return
    main_url = _source_url(source_path)
    source_texts = []
    for item in ranges:
        source_texts.append((item.get("normalized_text") or "\n".join(docs[item["source_path"]]["lines"][item["line_start"] - 1:item["line_end"]])).strip("\n"))
    complete_text = "\n\n".join(source_texts).strip()
    concepts = _concepts(title, complete_text, source_id)
    title_slug = _slug(code or title)
    passage_id = f"{source_id}:{topic}:{title_slug}"
    identity_key = (source_id, source_path, ranges[0]["line_start"], title)
    established = (identities or {}).get(identity_key)
    if established:
        passage_id = established
    elif passage_id in (identities or {}).values():
        passage_id += "-" + str(ranges[0]["line_start"])
    existing = {p["passage_id"] for p in rows}
    if passage_id in existing:
        passage_id += "-" + str(ranges[0]["line_start"])
    span_records = []
    supporting = []
    source_ranges = []
    for order, item in enumerate(ranges):
        source_file = item["source_path"]
        document = docs[source_file]
        span = {key: item[key] for key in ("chapter", "source_path", "line_start", "line_end", "source_text_hash")}
        span.update({"span_order": order, "anchor": item["anchor"], "source_url": document["source_url"],
                     "source_file_sha256": document["sha256"], "license_id": document["license_id"],
                     "license_basis": document["license_basis"], "license_evidence_path": document["license_evidence_path"],
                     "license_note": document["license_note"], "review_status": document["review_status"],
                     "heading_path": heading if source_file == source_path else heading + ["示例与诊断片段"]})
        if "normalized_text" in item:
            span["normalized_text"] = item["normalized_text"]
            span["normalization"] = item["normalization"]
        span_records.append(span)
        source_ranges.append({key: span[key] for key in (
            "source_path", "source_url", "line_start", "line_end", "anchor", "source_text_hash",
            "source_file_sha256", "license_id", "license_basis", "license_evidence_path", "review_status")})
        ref = {"source_id": source_id, "source_url": document["source_url"], "anchor": item["anchor"]}
        if ref not in supporting:
            supporting.append(ref)
    primary_ranges = [r for r in ranges if r["source_path"] == source_path]
    first_line, last_line = min(r["line_start"] for r in primary_ranges), max(r["line_end"] for r in primary_ranges)
    language = "en"
    retrieved = RETRIEVED_AT
    row = {
        "passage_id": passage_id, "course_id": "fpga-verilog", "course_version": {
            "lowrisc_style": "lowRISC style-guides@" + COMMITS["lowrisc"],
            "yosys_047": "Yosys 0.47@" + COMMITS["yosys"],
            "verilator_5052": "Verilator v5.052@" + COMMITS["verilator"],
        }[source_id],
        "title": title, "section": " > ".join(heading), "content": complete_text,
        "character_count": len(complete_text), "byte_count": len(complete_text.encode("utf-8")),
        "content_type": "allowed_excerpt", "source_id": source_id, "source_url": main_url,
        "anchor": None, "author_or_source": docs[source_path]["repository"],
        "license_or_usage_note": docs[source_path]["license_note"], "retrieved_at": retrieved,
        "license_id": docs[source_path]["license_id"], "license_basis": docs[source_path]["license_basis"],
        "review_status": "official_source_pending_teacher_review", "source_ranges": source_ranges,
        "language": language, "difficulty": difficulty, "concept_ids": concepts,
        "content_hash": digest_text(complete_text),
        "supporting_sources": supporting,
        "search_text": " ".join([_mapping(title, "", concepts, source_id, code)["title_zh"]] +
                                  _mapping(title, "", concepts, source_id, code)["terms"] +
                                  _mapping(title, "", concepts, source_id, code)["questions_zh"]),
        "source_location": {"path": source_path, "line_start": first_line, "line_end": last_line},
    }
    mapping = _mapping(title, "", concepts, source_id, code)
    row["mapping"] = mapping
    rows.append(row)
    span_rows.append({"passage_id": passage_id, "evidence_status": "grounded", "spans": span_records})


def _lines_without_includes(source_path: str, start: int, end: int, lines: list[str], docs: dict[str, dict[str, Any]], heading: list[str]) -> list[dict[str, Any]]:
    ranges = []
    segment_start = start
    for line_number in range(start, end + 1):
        match = INCLUDE_RE.match(lines[line_number - 1])
        if not match:
            continue
        if segment_start <= line_number - 1:
            ranges.append(_source_record(source_path, segment_start, line_number - 1))
        include_path = PurePosixPath(match.group(1))
        include_name = "verilator/v5.052/docs/gen/" + include_path.name
        if include_name not in docs:
            raise ValueError(f"unregistered include path: {match.group(1)}")
        if include_name.endswith("_fixed.rst"):
            segment_start = line_number + 1
            continue
        included_lines = docs[include_name]["lines"]
        ranges.append(_source_record(include_name, 1, len(included_lines)))
        segment_start = line_number + 1
    if segment_start <= end:
        ranges.append(_source_record(source_path, segment_start, end))
    return [r for r in ranges if r["line_start"] <= r["line_end"]
            and "\n".join(docs[r["source_path"]]["lines"][r["line_start"] - 1:r["line_end"]]).strip()]


def _cpp_help(source_path: str, marker: str, title: str) -> tuple[str, int, int]:
    _, lines, _ = _read_source(source_path)
    start = next(i for i, line in enumerate(lines) if marker in line)
    end = next((i for i in range(start + 1, len(lines)) if "void execute(" in lines[i]), len(lines))
    literal_re = re.compile(r'\blog\("((?:\\.|[^"\\])*)"\);')
    pieces = []
    for line in lines[start:end]:
        match = literal_re.search(line)
        if match:
            pieces.append(json.loads('"' + match.group(1) + '"'))
    result = "".join(pieces).strip()
    if len(result) < 100 or title not in result:
        raise ValueError(f"failed to extract {title} command help from {source_path}")
    first = start + 1
    last = end
    while last > first and not literal_re.search(lines[last - 1]):
        last -= 1
    return result, first, last


def build_generated_resources() -> dict[str, Any]:
    """Create provenance, passage, mapping, and translation resource files."""
    sources = source_file_catalog()
    source_by_path = {row["source_path"]: row for row in sources}
    docs: dict[str, dict[str, Any]] = {}
    for row in sources:
        text, lines, data = _read_source(row["source_path"])
        docs[row["source_path"]] = {**row, "lines": lines, "text": text, "data": data,
                                     "document_id": "official:" + row["source_path"].replace("/", ":").replace(".", "-")}

    passages: list[dict[str, Any]] = []
    spans: list[dict[str, Any]] = []
    registry_path = RESOURCE_DIR / "external_passage_ids.jsonl"
    registry = [json.loads(line) for line in registry_path.read_text(encoding="utf-8").splitlines()
                if line.strip()] if registry_path.is_file() else []
    identities = {(row["source_id"], row["source_location"]["path"],
                   row["source_location"]["line_start"], row["title"]): row["passage_id"]
                  for row in registry}
    for source_path in ("lowrisc/9c15ff5/VerilogCodingStyle.md",):
        _, lines, _ = _read_source(source_path)
        for heading, start, end, title in _markdown_sections(lines):
            if title.casefold() == "summary":
                toc = next((index for index in range(start, end + 1)
                            if lines[index - 1].strip() == "**Table of Contents**"), None)
                if toc is not None:
                    end = toc - 1
            excerpt = "\n".join(lines[start - 1:end]).strip()
            # Heading-only navigation containers have no evidence body.
            if not any(line.strip() for line in lines[start:end]):
                continue
            _add_passage(passages, spans, docs, identities=identities, source_id="lowrisc_style", topic="verilog",
                         source_path=source_path, title=title, heading=heading,
                         ranges=[_source_record(source_path, start, end)])

    primer = "yosys/0.47/docs/source/appendix/primer.rst"
    _, lines, _ = _read_source(primer)
    for heading, start, end, title in _rst_sections(lines):
        excerpt = "\n".join(lines[start - 1:end]).strip()
        if any(line.strip() and not RST_UNDERLINE_RE.fullmatch(line.strip())
               for line in lines[start+1:end]):
            _add_passage(passages, spans, docs, identities=identities, source_id="yosys_047", topic="primer",
                         source_path=primer, title=title, heading=heading,
                         ranges=[_source_record(primer, start, end)])

    for command, path, marker, display in (
        ("check", "yosys/0.47/passes/cmds/check.cc", "void help() override", "check"),
        ("read-verilog", "yosys/0.47/frontends/verilog/verilog_frontend.cc", "void help() override", "read_verilog"),
    ):
        help_text, start, end = _cpp_help(path, marker, display)
        _add_passage(passages, spans, docs, identities=identities, source_id="yosys_047", topic="command",
                     source_path=path, title=f"{display} 命令帮助", heading=["命令", display],
                     ranges=[_source_record(path, start, end, normalized_text=help_text)])

    warnings = "verilator/v5.052/docs/guide/warnings.rst"
    _, warning_lines, _ = _read_source(warnings)
    options = [(i, match.group(1).strip()) for i, line in enumerate(warning_lines, 1)
               if (match := RST_OPTION_RE.match(line))]
    # The preamble contains actual disabling/diagnostic-format rules, not just
    # navigation. Preserve heading-owned bodies before the first option entry.
    prefix_end=options[0][0]-1 if options else len(warning_lines)
    for heading,start,end,title in _rst_sections(warning_lines[:prefix_end]):
        body=warning_lines[start+1:end]
        if not any(s.strip() and not RST_UNDERLINE_RE.fullmatch(s.strip())
                   and not re.match(r"^\.\. _.*:$",s.strip()) for s in body):
            continue
        _add_passage(passages,spans,docs,identities=identities,
                     source_id="verilator_5052",topic="warnings",source_path=warnings,
                     title=title,heading=heading,
                     ranges=[_source_record(warnings,start,end)])
    for index, (start, name) in enumerate(options):
        end = options[index + 1][0] - 1 if index + 1 < len(options) else len(warning_lines)
        code_match = re.search(r"(?:%Warning-|%Error-)?([A-Z][A-Z0-9]{2,})", name)
        code = code_match.group(1) if code_match else None
        ranges = _lines_without_includes(warnings, start, end, warning_lines, docs, ["Warnings", name])
        if len(ranges) and sum((r["line_end"] - r["line_start"] + 1) for r in ranges) > 1:
            _add_passage(passages, spans, docs, identities=identities, source_id="verilator_5052", topic="warnings",
                         source_path=warnings, title=name, heading=["Warnings", name], ranges=ranges,
                         difficulty="intermediate", code=code)

    languages = "verilator/v5.052/docs/guide/languages.rst"
    _, lines, _ = _read_source(languages)
    for heading, start, end, title in _rst_sections(lines):
        excerpt = "\n".join(lines[start - 1:end]).strip()
        if any(line.strip() and not RST_UNDERLINE_RE.fullmatch(line.strip())
               for line in lines[start+1:end]):
            _add_passage(passages, spans, docs, identities=identities, source_id="verilator_5052", topic="languages",
                         source_path=languages, title=title, heading=heading,
                         ranges=[_source_record(languages, start, end)])

    # File provenance and passage metadata are stored separately from raw text.
    _write_jsonl(RESOURCE_DIR / "external_source_files.jsonl", sources)
    external_documents = []
    for path, doc in docs.items():
        if not any(span["passage_id"] and any(s["chapter"] == "external:" + path.replace("/", ":").replace(".", "-") for s in span["spans"]) for span in spans):
            continue
        external_documents.append({"document_id": doc["document_id"], "chapter": "external:" + path.replace("/", ":").replace(".", "-"),
            "source_id": doc["source_id"], "source_url": doc["source_url"], "title": PurePosixPath(path).name,
            "source_path": path, "sha256": doc["sha256"], "retrieved_at": RETRIEVED_AT})
    _write_jsonl(RESOURCE_DIR / "external_official_documents.jsonl", external_documents)
    _write_jsonl(RESOURCE_DIR / "external_official_passages.jsonl", passages)
    _write_jsonl(RESOURCE_DIR / "external_official_spans.jsonl", spans)
    _write_jsonl(RESOURCE_DIR / "external_official_mappings.jsonl", [
        {"passage_id": row["passage_id"], **row["mapping"]} for row in passages])
    _write_jsonl(RESOURCE_DIR / "external_official_translations.jsonl", _translation_records(passages))
    _write_jsonl(RESOURCE_DIR / "external_exclusions.jsonl", [
        {"source_path": row["source_path"], "reason": "snippet is a complete fixed solution; protected by L4 pending authorization",
         "discovery": "included by warnings.rst but excluded from student retrieval"}
        for row in sources if row["source_path"].endswith("_fixed.rst")])
    return {"source_files": len(sources), "indexed_source_files": sum(bool(r["indexed"]) for r in sources),
            "passages": len(passages), "documents": len(external_documents),
            "translations": len(_translation_records(passages))}


def _write_jsonl(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n" for row in rows), encoding="utf-8")


def _translation_records(passages: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Full, source-linked core translations; drafts are explicitly unreviewed."""
    zh = {
        "lowrisc_style:verilog:preferred-systemverilog-constructs": """### 优先使用的 SystemVerilog 构造

请优先使用以下 SystemVerilog 构造，而不是对应的 Verilog-2001 写法：

- 必须使用 `always_comb`，而不是 `always @*`。
- 优先使用 `logic`，而不是 `reg` 和 `wire`。
- 优先使用模块顶层的 `parameter` 声明，而不是全局 `` `define``。
""",
        "lowrisc_style:verilog:port-connections-on-module-instances-must-always-match-widths-correctl": """#### 模块实例端口连接的位宽必须始终正确匹配

在实际可行时，建议显式写出位宽，而不是依赖 Verilog 的隐式零扩展和截断操作。

示例：

&#x1f44d;
```systemverilog {.good}
my_module i_module (
  .thirty_two_bit_input({16'd0, sixteen_bit_word})
);
```

&#x1f44e;
```systemverilog {.bad}
my_module i_module (
  // Incorrectly implicitly extends from 16 bit to 32 bit
  .thirty_two_bit_input(sixteen_bit_word)
);
```
""",
        "lowrisc_style:verilog:sequential-logic-latches": """### 时序逻辑（锁存器）

***不鼓励使用锁存器；尽可能使用触发器。***

除非绝对必要，否则使用触发器/寄存器，而不是锁存器。

如果必须使用锁存器，请使用 `always_latch`，而不是 `always`，并使用非阻塞赋值（`<=`）。绝不要使用阻塞赋值（`=`）。
""",
        "lowrisc_style:verilog:function-and-task-calls": """#### 函数和任务调用

***函数名或任务名与左括号之间不得有空格。***

例如：

&#x1f44d;
```systemverilog {.good}
process_packet(pkt);
```

&#x1f44e;
```systemverilog {.bad}
process_packet (pkt);  // There must not be a space before "("
```
""",
        "lowrisc_style:verilog:active-low-signals": """### 低有效信号

***后缀 `_n` 表示信号为低有效。***

如果使用低有效信号，其名称必须带 `_n` 后缀。否则，所有信号都按高有效处理。
""",
        "lowrisc_style:verilog:declare-all-signals": """### 声明所有信号

***不要依赖推断出的网络。***

所有信号在使用前都必须显式声明。所有已声明信号都必须指定数据类型。正确的设计不包含推断网络。
""",
        "lowrisc_style:verilog:delays": """### 延迟

***延迟一个时钟周期的信号应以 `_q` 为后缀。***

如果一个信号只是另一个信号延迟一个周期后的版本，应使用 `_q` 后缀表示这种关系。

如果另一个信号又延迟一个时钟周期，后续信号应使用 `_q2` 后缀标识，再后面依次使用 `_q3` 等。

示例：

```systemverilog
always_ff @(posedge clk) begin
  data_valid_q <= data_valid_d;
  data_valid_q2 <= data_valid_q;
  data_valid_q3 <= data_valid_q2;
end
```
""",
        "yosys_047:primer:functions-and-tasks": """函数和任务
~~~~~~~~~~~~~~~~~~~

Verilog 支持函数（Functions）和任务（Tasks），用于将会在多个位置使用的语句组合起来（类似命令式编程语言中的过程）。通过将函数或任务调用替换为其函数体或任务体，这两种构造都可以很容易地展开实现。
""",
        "yosys_047:primer:switch-level": """开关级
~~~~~~~~

电路的开关级表示是一种网表，其中单个晶体管作为单元。Verilog 和 VHDL 都可以进行开关级建模，但现代设计中很少使用这种方式；在现代数字 ASIC 或 FPGA 流程中，物理逻辑门通常被视为逻辑电路的基本构建单元。
""",
        "yosys_047:primer:technology-mapping": """技术映射
~~~~~~~~~~~~~~~~~~

技术映射是将设计转换为由目标架构可用单元组成的网表的过程。在 ASIC 流程中，这可能是使用晶圆厂提供的、针对特定工艺的单元库。在 FPGA 流程中，这可能会使用 LUT 单元以及专用乘法器等特殊功能单元。在粗粒度流程中，目标甚至可能是更复杂的特殊功能单元。

如果一个开放且与厂商无关的工具支持多种不同类型的目标架构，它就尤其有价值。
""",
        "yosys_047:primer:system-level": """系统级
~~~~~~~~~~~~

系统级抽象只关注系统中最大的构建模块，例如 CPU 和计算核心。在这一层，电路通常使用 C/C++ 或 Matlab 等传统编程语言描述。有时也会使用专门面向系统级电路仿真的软件库，例如 SystemC。

通常不会使用综合工具，将系统级表示自动转换为更低层次的表示。不过，确实存在可用于连接系统级构建模块的系统级设计工具。

IEEE 1685-2009 标准定义了 IP-XACT 文件格式，可用于表示系统级设计以及可供此类设计使用的构建模块。:cite:p:`IP-XACT`
""",
        "yosys_047:command:check": """check [options] [selection]

该 pass 会识别当前设计中的以下问题：

  - 组合环路
  - 同一条 wire 存在两个或更多相互冲突的驱动源
  - 被使用但没有驱动源的 wire

选项：

    -noinit
        同时检查设置了 `init` 属性的 wire

    -initdrv
        同时检查设置了 `init` 属性、但未由 FF 单元类型驱动的 wire

    -mapped
        同时检查尚未映射为目标架构单元的内部单元

    -allow-tbuf
        修改 `-mapped` 的行为，使其仍允许 `$_TBUF_` 单元

    -assert
        如果当前设计中发现任何问题，则产生运行时错误

    -force-detailed-loop-check
        检测组合环路时，对所有支持详细连通性模型的内部单元使用该模型。对于预计开销较大的单元，此选项会禁用退回到更简单、覆盖范围更宽的近似模型。
""",
        "yosys_047:command:read-verilog": """read_verilog [options] [filename]

从 Verilog 文件向当前设计载入模块。支持 Verilog-2005 的一个较大子集。

    -sv
        启用 SystemVerilog 特性支持。（只支持 SystemVerilog 的一个较小子集。）

    -formal
        启用 SystemVerilog 断言及部分 Yosys 扩展；将隐式定义的 `-D SYNTHESIS` 替换为 `-D FORMAL`。

    -nosynthesis
        不添加隐式的 `-D SYNTHESIS`。

    -noassert
        忽略 `assert()` 语句。

    -noassume
        忽略 `assume()` 语句。

    -norestrict
        忽略 `restrict()` 语句。

    -assume-asserts
        将所有 `assert()` 语句按 `assume()` 语句处理。

    -assert-assumes
        将所有 `assume()` 语句按 `assert()` 语句处理。

    -nodisplay
        禁止输出来自 display 系统任务（例如 `$display`）的内容。这不会影响之后 `sim` 命令的输出。

    -debug
        等价于 `-dump_ast1 -dump_ast2 -dump_vlog1 -dump_vlog2 -yydebug`。

    -dump_ast1
        转储简化前的抽象语法树。

    -dump_ast2
        转储简化后的抽象语法树。

    -no_dump_ptr
        转储内容不包含十六进制内存地址（便于比较差异）。

    -dump_vlog1
        将简化前的 AST 转储为 Verilog 代码。

    -dump_vlog2
        将简化后的 AST 转储为 Verilog 代码。

    -dump_rtlil
        转储生成的 RTLIL 网表。

    -yydebug
        启用解析器调试输出。

    -nolatches
        通常，锁存器会综合为逻辑环路。此选项禁止这种处理，并在锁存器保持条件下将输出设为 `x`。

        也可以在相应模块或 always 块上设置 `nolatches` 属性，以实现相同效果。

    -nomem2reg
        在某些条件下，为确保正确处理复杂边界情况，存储器会在简化早期转换为寄存器。此选项会禁用该行为。

        也可以在相应模块或寄存器上设置 `nomem2reg` 属性。

        这有潜在危险。前端通常有充分理由将数组转换为寄存器列表。禁止这一步很可能导致综合结果错误。

    -mem2reg
        始终将存储器转换为寄存器。也可以在相应模块或寄存器上设置 `mem2reg` 属性。

    -nomeminit
        不推断 `$meminit` 单元，而是在前端直接将带初始化内容的存储器转换为寄存器。

    -ppdump
        转储预处理后的 Verilog 代码。

    -nopp
        不运行预处理器。

    -nodpi
        禁用 DPI-C 支持。

    -noblackbox
        不自动为空模块添加 `(* blackbox *)` 属性。

    -lib
        只创建空的 blackbox 模块。此选项隐含 `-DBLACKBOX`。带有 `(* whitebox *)` 属性的模块会保留；`(* lib_whitebox *)` 会按 `(* whitebox *)` 处理。

    -nowb
        从所有模块中删除 `(* whitebox *)` 和 `(* lib_whitebox *)` 属性。

    -specify
        解析并导入 specify 块。

    -noopt
        不在高层前端执行基础优化（例如常量折叠）。

    -icells
        将以 `$` 开头的单元类型解释为内部单元类型。

    -pwires
        为每个模块参数添加一个 wire。

    -nooverwrite
        忽略模块重新定义。默认行为是：如果已有同名模块不是 black box，则报错；否则覆盖它。

    -overwrite
        覆盖已有的同名模块。

    -defer
        只读取抽象语法树，并将实际编译推迟到之后的 `hierarchy` 命令。适用于模块默认参数会产生无效或不可综合代码的情况。

    -noautowire
        将 ``default_nettype`` 的默认值从 `wire` 改为 `none`。

    -setattr <attribute_name>
        为所有载入的模块设置指定属性，属性值为 1。

    -Dname[=definition]
        定义预处理器符号 `name`，并设置可选值 `definition`。

    -Idir
        将 `dir` 加入搜索 include 文件时使用的目录。

命令 `verilog_defaults` 可用于为后续的 `read_verilog` 调用登记默认选项。

注意，Verilog 前端对有效 Verilog 输入的处理能力较好，但错误报告能力较弱。通常建议使用仿真器（例如 Icarus Verilog）检查代码语法，而不要依赖 `read_verilog` 完成这项检查。

`read_verilog` 是否自动定义 `SYNTHESIS` 或 `FORMAL`，取决于是否以 `-formal` 模式运行；除非使用 `-nosynthesis`，否则会定义其中一个。此外，`read_verilog` 始终定义宏 `YOSYS`。

Yosys Verilog 前端支持的非标准 Verilog 特性列表见 Yosys README。""",
        "verilator_5052:warnings:widthtrunc": """.. option:: WIDTHTRUNC

   `WIDTH` 告警的更细分类，用于值被截断的情况。参见 :option:`WIDTH`。
""",
        "verilator_5052:warnings:widthexpand": """.. option:: WIDTHEXPAND

   `WIDTH` 告警的更细分类，用于值被零扩展的情况。参见 :option:`WIDTH`。
""",
        "verilator_5052:warnings:widthxzexpand": """.. option:: WIDTHXZEXPAND

   `WIDTH` 告警的更细分类，用于值因 X/Z 而扩展的情况。参见 :option:`WIDTH`。
""",
        "verilator_5052:warnings:genclk": """.. option:: GENCLK

   历史告警，自 5.000 版本起不再发出。

   该告警表示指定的信号是在模型内部生成并用作时钟。
""",
        "verilator_5052:warnings:needtimingopt": """.. option:: NEEDTIMINGOPT

   遇到与时序相关的构造（例如事件控制或延迟），但没有指定 Verilator 应如何处理它时会报错（既没有提供 :vlopt:`--timing`，也没有提供 :vlopt:`--no-timing` 选项）。
""",
        "verilator_5052:warnings:pinnoconnect": """.. option:: PINNOCONNECT

   .. TODO better example

   警告某个实例有一个引脚没有连接到其他信号。

   默认关闭，因为这是代码风格告警；设计仍然可以正确仿真。
""",
        "verilator_5052:warnings:unusedgenvar": """.. option:: UNUSEDGENVAR

   .. TODO better example

   警告指定的 genvar 从未被使用/消费。参见类似的 :option:`UNUSEDSIGNAL`。
""",
        "verilator_5052:warnings:unusedparam": """.. option:: UNUSEDPARAM

   .. TODO better example

   警告指定的参数从未被使用/消费。参见类似的 :option:`UNUSEDSIGNAL`。
""",
        "verilator_5052:languages:bind": """Bind
----

Verilator 只支持将 bind 绑定到目标模块名，不支持绑定到实例路径。
""",
        "verilator_5052:languages:assertions": """断言
----------

Verilator 对断言和由断言驱动的功能覆盖率提供部分支持。
""",
        "verilator_5052:languages:class": """类
-----

Verilator 对类的支持有限，但仍在积极开发中。Verilator 支持成员、方法、类继承（class extend）和类参数。
""",
        "verilator_5052:languages:coverage": """覆盖率
--------

Verilator 对 SystemVerilog 功能覆盖率提供部分支持，包括 ``covergroup``、``coverpoint``、bins、交叉覆盖率和转换覆盖率。参见 :ref:`Covergroup Coverage`。
""",
    }
    rows = {row["passage_id"]: row for row in passages}
    if len(zh) < 20 or len(zh) > 30 or not set(zh) <= set(rows):
        raise ValueError("core translation set must contain 20-30 known official passages")
    result = []
    for passage_id, translated_text in zh.items():
        result.append({
            "translation_id": "zh-CN:" + passage_id + ":v1",
            "passage_id": passage_id,
            "source_content_hash": rows[passage_id]["content_hash"],
            "translated_text": translated_text.strip(),
            "generation_method": "Codex model draft translated from the pinned official English passage; no teacher review",
            "review_status": "machine_translation_unreviewed",
            "created_at": RETRIEVED_AT,
        })
    return result


if __name__ == "__main__":  # pragma: no cover - command line resource generation
    print(json.dumps(build_generated_resources(), ensure_ascii=False, indent=2))
