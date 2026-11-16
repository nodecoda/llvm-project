#!/usr/bin/env python3
#===-- dsl-surface.py - Extract a TableGen backend's DSL contract ---------===#
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
#===----------------------------------------------------------------------===#
"""Extract the .td "DSL surface" consumed by a TableGen backend.

The problem this solves: a backend reads fields and record names with plain
strings (`getValueAsInt("CostPerUse")`, `getName() == "FixedRegisters"`).
Those strings are the real DSL, but nothing in the .td files says which of them
matter, what their defaults are, or where they are consumed, so target authors
copy patterns instead of reading a specification.

This script derives that specification from the C++ sources themselves:

  1. root record classes  <- getAllDerivedDefinitions("...") / explicit roots
  2. field contract       <- .td declarations + every getValue*("...") read
  3. name-level contract  <- magic record names compared against literals
  4. unused fields        <- declared on a root class but never read here

`--update` writes the generated tables into a markdown document, `--check`
verifies the document is still in sync and that every consumed field has a
human-written semantic annotation.  A backend change that adds a keyword
without a spec therefore fails the check instead of silently drifting.

The experiment starts with `-gen-register-info`; adding another backend means
adding a Backend entry below (and, eventually, its annotations).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[3]
BEGIN_MARKER = "<!-- BEGIN: GENERATED DSL CONTRACT -->"
END_MARKER = "<!-- END: GENERATED DSL CONTRACT -->"
USAGE_BEGIN = "<!-- BEGIN: GENERATED TD USAGE -->"
USAGE_END = "<!-- END: GENERATED TD USAGE -->"


@dataclass(frozen=True)
class Backend:
    name: str
    emitter: str
    # (path, scope); scope drives the "consumed by" classification.
    sources: tuple
    td_files: tuple
    # Only these sources participate in root-class discovery; shared files
    # also mention unrelated backends' classes.
    roots_from: tuple = ()
    # Roots that are not discovered through getAllDerivedDefinitions().
    extra_roots: tuple = ()
    # Classes whose fields are part of the contract even though no root
    # inherits from them.
    helper_classes: tuple = ()
    # Fields of records outside the register pipeline that still steer it.
    ambient_fields: tuple = ()
    # Fields pulled in by SetTheory::addFieldExpander() rather than by a
    # getValue*() call.
    field_expanders: tuple = ()


BACKENDS = {
    "register-info": Backend(
        name="register-info",
        emitter="llvm/utils/TableGen/RegisterInfoEmitter.cpp",
        sources=(
            ("llvm/utils/TableGen/RegisterInfoEmitter.cpp", "emitter"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.h", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.cpp", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.h", "shared"),
            # PositionOrder is read by the core register sorter, which decides
            # the generated register enum order.
            ("llvm/include/llvm/TableGen/Record.h", "core"),
        ),
        td_files=(
            "llvm/include/llvm/Target/Target.td",
            "llvm/include/llvm/Target/TargetCallingConv.td",
        ),
        roots_from=(
            "llvm/utils/TableGen/RegisterInfoEmitter.cpp",
            "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
        ),
        extra_roots=(
            ("RegAltNameIndex", "llvm/utils/TableGen/RegisterInfoEmitter.cpp",
             "Target.getRegAltNameIndices()"),
        ),
        helper_classes=(
            "HwModeSelect", "RegInfoByHwMode", "RegInfo", "SubRegRangeByHwMode",
            "SubRegRange", "DwarfRegAlias", "RegisterOperand", "DAGOperand",
        ),
        ambient_fields=(("Target", "RegistersAreIntervals"),),
        field_expanders=(
            ("RegisterClass", "MemberList"),
            ("CalleeSavedRegs", "SaveList"),
        ),
    ),
}


# Human-written semantics for the register-info backend.  Keys are field
# names; the same name may be declared by several classes, which is itself
# part of the contract (TableGen lookup is by name, not by type).
ANNOTATIONS = {
    # --- Register ---------------------------------------------------------
    "Namespace": "本后端据此生成 C++ namespace；取第一个 Register / RegisterClass / SubRegIndex 的值。",
    "AsmName": "汇编名；用作 `RegistersByName` 去重 key。`RegisterTuples` 未给 `RegAsmNames` 时为空串。",
    "AltNames": "本后端不读；由 `-gen-asm-writer` / `-gen-asm-matcher` 消费。",
    "Aliases": "显式别名；对称化成别名图，参与 regunit / `getSubRegs()` / lane mask 计算。",
    "SubRegs": "子寄存器列表；与 `SubRegIndices` 交给 `zip_equal` 配对。`RegisterTuples` 展开时被替换成 tuple。",
    "SubRegIndices": "与 `SubRegs` 一一对应；`RegisterTuples` 展开时取自 tuple 定义。",
    "RegAltNameIndices": "本后端不读；asm 打印用。",
    "DwarfNumbers": "每个 flavour 一个 gcc/gdb 编号；`-1` 表示无映射。长度取全局最大值，短的补 `-1`。",
    "CostPerUse": "每寄存器成本序列；按全局最大长度补 0，写入 `CostPerUseTable`。",
    "CoveredBySubRegs": "值完全由子寄存器决定；触发 covered-register 计算与合成子寄存器索引。",
    "HWEncoding": "bits<16>，转成整数写入 `RegEncodingTable[]`。",
    "isArtificial": "合成寄存器标记；写入 `MCRegisterDesc`，并参与寄存器类的 `Artificial` 计算（全 artificial 的类压力权重上限为 0）。",
    "isConstant": "生成 `isConstantPhysReg()`，并自动加入所有 `CalleeSavedRegs` 的 mask。",
    "PositionOrder": "间接消费：`LessRecordRegister`（Record.h）先按它排序，决定寄存器枚举顺序与 tuple 内部顺序。",
    # --- RegisterClass ----------------------------------------------------
    "RegInfos": "按 HwMode 的 {RegSize,SpillSize,SpillAlignment}；非空时覆盖 `Size`/`Alignment` 的默认模式值。",
    "RegTypes": "非空（.td 断言）；决定 `RegClassInfos` 的 MVT 列表，并在 `Size==0` 时提供推断位宽。",
    "Size": "寄存器类：spill size，`0` = 按首个 `RegTypes` 推断；子寄存器索引/范围：位数。",
    "Alignment": "spill 对齐（bit）。",
    "CopyCost": "8-bit 拷贝代价；>255 则 fatal。负值：注释说饱和为 255，实际被构造函数末尾的赋值覆盖，按 `uint8_t` 截断（见第 29 章 §深读 9）。",
    "MemberList": "不是普通 list：经 SetTheory field expander 展开（add/sub/sequence…），决定成员集与默认分配序。",
    "isAllocatable": "是否进入 `InAllocatableClassTable`，并决定哪些寄存器算“可分配”。",
    "AltOrders": "额外分配序；每个 dag 用 SetTheory 求值，成员必须属于 `MemberList`，否则 fatal。",
    "AltOrderSelect": "`code` 原文拼进生成的 `AltOrderSelect(MF, Rev)` 函数体；非空才生成 `getRawAllocationOrder()`。",
    "AllocationPriority": "0..31，越界 fatal；写入寄存器类表。",
    "GlobalPriority": "写入寄存器类表，交给 greedy 分配器的全局启发式。",
    "GeneratePressureSet": "`false` 时不为该类（及其派生类）生成寄存器压力集。",
    "Weight": "`?` 时按 regunit 数计算；否则覆盖 `getRegClassWeight()` 的每寄存器权重。",
    "TSFlags": "bits<8>，原样写入寄存器类表。",
    "BaseClassOrder": "`?` 时不参与；否则决定寄存器的“基类”归属与 `getPhysRegBaseClass()` 生成。",
    "SpillStackID": "0..255，越界 fatal；写入寄存器类表。",
    # --- SubRegIndex ------------------------------------------------------
    "SubRegRanges": "按 HwMode 的 {Size,Offset}；未包含 DefaultMode 时回落到 `Size`/`Offset`。",
    "Offset": "子寄存器起始位；`-1` 表示不固定（如 tuple 元素）。",
    "ComposedOf": "两个索引的复合关系；必须恰好 2 个，否则 fatal。",
    "CoveringSubRegIndices": "声明“拼接覆盖”关系；至少 2 个，否则 fatal（通常留空让 TableGen 推断）。",
    # --- RegisterTuples ---------------------------------------------------
    "RegAsmNames": "tuple 寄存器的 asm 名列表；数量不足时 fatal。",
    "DwarfAlias": "复制另一个寄存器的 `DwarfNumbers`（由 `DwarfRegAlias` 提供）；别名必须存在，否则 assert。",
    # --- RegisterCategory -------------------------------------------------
    "Classes": "类别包含的寄存器类。注意：本后端只识别三个 def 名，见“名字即 API”。",
    # --- CalleeSavedRegs --------------------------------------------------
    "SaveList": "经 SetTheory field expander 展开；生成 `<name>_SaveList`，顺序即 prologue 保存顺序。",
    "OtherPreserved": "只进 `<name>_RegMask`、不进 `_SaveList`；未设置时跳过。",
    # --- RegisterByHwMode / HwModeSelect ----------------------------------
    "Modes": "与 `Objects` 等长（.td 断言），按 ModeId 绑定。",
    "Objects": "n 号对象绑定 n 号 mode；对 `RegisterByHwMode` 是寄存器列表，对 `RegInfoByHwMode`/`SubRegRangeByHwMode` 是参数列表。",
    "RegClass": "`RegisterByHwMode` 的目标寄存器类；每个 `Objects` 成员必须属于它（否则 fatal）。",
    # --- RegInfo / SubRegRange / DwarfRegAlias ----------------------------
    "RegSize": "`RegInfo`：寄存器位数。",
    "SpillSize": "`RegInfo`：spill slot 位数。",
    "SpillAlignment": "`RegInfo`：spill slot 对齐。",
    # --- ambient ----------------------------------------------------------
    "RegistersAreIntervals": "`Target` 记录上的开关；决定是否生成 `RegUnitIntervals` 表。",
}

# Fields that are declared on a consumed class but intentionally not read by
# this backend.  The note points at the backend that owns them.
UNUSED_NOTES = {
    "GIZeroRegister": "isel / regalloc（generic zero register）",
    "AltNames": "asm writer / asm matcher",
    "RegAltNameIndices": "asm writer / asm matcher",
    "altNameIndex": "asm writer",
    "FallbackRegAltNameIndex": "asm writer",
    "DiagnosticType": "asm matcher",
    "DiagnosticString": "asm matcher",
    "CoerceFromRegClass": "asm matcher",
    "OperandNamespace": "asm matcher / MC operand",
    "DecoderMethod": "decoder",
    "hasCompleteDecoder": "decoder",
    "PrintMethod": "asm writer",
    "EncoderMethod": "code emitter",
    "ParserMatchClass": "asm parser",
    "MIOperandInfo": "instruction selection",
    "OperandType": "MC operand",
}

# Name-level contract: record names and generated identifiers that are not
# reachable through field lookup.  Each entry is (name, regex that must still
# exist in the sources, effect).
MAGIC_NAMES = (
    ("GeneralPurposeRegisters",
     r'"GeneralPurposeRegisters"',
     "`RegisterCategory` 的 def 名；生成 `isGeneralPurposeRegister()/isGeneralPurposeRegisterClass()`。"),
    ("FixedRegisters",
     r'"FixedRegisters"',
     "`RegisterCategory` 的 def 名；生成 `isFixedRegister()`。"),
    ("ArgumentRegisters",
     r'"ArgumentRegisters"',
     "`RegisterCategory` 的 def 名；生成 `isArgumentRegister()`。"),
    ("NoRegister",
     r'NoRegister',
     "生成的枚举 0；`MCRegisterDesc` / 各表的第一项。"),
    ("NUM_TARGET_REGS",
     r'NUM_TARGET_REGS',
     "生成枚举的哨兵，值为寄存器数 + 1。"),
    ("NoSubRegister",
     r'NoSubRegister',
     "子寄存器索引枚举 0；`composeSubRegIndices` 的空结果。"),
    ("NUM_TARGET_SUBREGS",
     r'NUM_TARGET_SUBREGS',
     "子寄存器索引枚举哨兵。"),
    ("<name>_SaveList",
     r'_SaveList',
     "`CalleeSavedRegs` def 名派生的保存列表。"),
    ("<name>_RegMask",
     r'_RegMask',
     "`CalleeSavedRegs` def 名派生的 call-preserved mask。"),
    ("get<Name>(HwMode)",
     r'"LLVM_READONLY MCRegister get"',
     "`RegisterByHwMode` def 名派生的 getter。"),
)

READ_RE = re.compile(
    r'\b(getValue[A-Za-z]*|getValueInit)\s*\(\s*"([A-Za-z_][A-Za-z0-9_]*)"')
ROOT_RE = re.compile(
    r'getAllDerivedDefinitions\s*\(\s*"([A-Za-z_][A-Za-z0-9_]*)"')
EXPANDER_RE = re.compile(
    r'addFieldExpander\s*\(\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*"([A-Za-z_][A-Za-z0-9_]*)"')
BACKEND_RE = re.compile(r'OptClass<[^>]*>\s*\w+\s*\(\s*"(gen-[A-Za-z0-9-]+)"')
CLI_OPT_RE = re.compile(r'cl::opt<[^>]*>\s+\w+\s*\(\s*"([A-Za-z0-9_-]+)"')
OUTPUT_RE = re.compile(r'"([A-Za-z0-9_]+\.[a-z]+)"')


class ContractError(Exception):
    pass


# --------------------------------------------------------------------------
# Source scanning
# --------------------------------------------------------------------------
def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments while preserving offsets/newlines."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif text.startswith("/*", i):
            while i < n and not text.startswith("*/", i):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            for _ in range(2):
                if i < n:
                    out[i] = " "
                    i += 1
        elif text[i] == '"':
            # Skip string literals so "//" inside them is not a comment.
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
        else:
            i += 1
    return "".join(out)


@dataclass
class ReadSite:
    field: str
    accessor: str
    file: str
    line: int
    snippet: str
    scope: str

    def sort_key(self):
        return (self.scope != "emitter", self.file, self.line)


def scan_reads(root: Path, backend: Backend):
    reads, roots, expanders = [], {}, []
    for rel, scope in backend.sources:
        text = strip_comments((root / rel).read_text())
        lines = text.splitlines()
        for m in READ_RE.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            reads.append(ReadSite(m.group(2), m.group(1), rel, line,
                                  lines[line - 1].strip()[:120], scope))
        if not backend.roots_from or rel in backend.roots_from:
            for m in ROOT_RE.finditer(text):
                line = text.count("\n", 0, m.start()) + 1
                roots.setdefault(m.group(1), []).append((rel, line, scope))
        for m in EXPANDER_RE.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            expanders.append((m.group(1), m.group(2), rel, line))
    return reads, roots, expanders


# --------------------------------------------------------------------------
# .td parsing
# --------------------------------------------------------------------------
@dataclass
class TdField:
    name: str
    type: str
    default: str
    line: int


@dataclass
class TdClass:
    name: str
    kind: str
    file: str
    line: int
    bases: tuple = ()
    fields: tuple = ()


def _line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def parse_td(root: Path, backend: Backend):
    classes = {}
    for rel in backend.td_files:
        text = strip_comments((root / rel).read_text())
        n = len(text)
        i = 0
        head_re = re.compile(r'\b(class|def)\s+([A-Za-z_][A-Za-z0-9_]*)')
        while True:
            m = head_re.search(text, i)
            if not m:
                break
            kind, name = m.group(1), m.group(2)
            j, depth = m.end(), 0
            while j < n:
                ch = text[j]
                if ch == "<":
                    depth += 1
                elif ch == ">":
                    depth = max(0, depth - 1)
                elif depth == 0 and ch in "{;":
                    break
                j += 1
            if j >= n:
                break
            if text[j] == ";":
                i = j + 1
                continue
            header = text[m.end():j]
            k, depth = j + 1, 1
            while k < n and depth:
                if text[k] == "{":
                    depth += 1
                elif text[k] == "}":
                    depth -= 1
                k += 1
            body = text[j + 1:k - 1]
            body_line = _line_of(text, j)
            classes[name] = TdClass(name, kind, rel, _line_of(text, m.start()),
                                    _parse_bases(header),
                                    _parse_fields(body, body_line))
            i = k
    return classes


def _parse_bases(header: str):
    depth = 0
    for idx, ch in enumerate(header):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif ch == ":" and depth == 0:
            return tuple(re.findall(r"(?:^|,)\s*([A-Za-z_][A-Za-z0-9_]*)",
                                    header[idx + 1:]))
    return ()


FIELD_RE = re.compile(
    r"^\s*(?:field\s+)?([A-Za-z_][A-Za-z0-9_]*(?:\s*<[^;]*>)?)\s+"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:=\s*(.*?))?;\s*$")


def _parse_fields(body: str, first_line: int):
    fields = []
    for offset, line in enumerate(body.splitlines()):
        m = FIELD_RE.match(line)
        if not m:
            continue
        ftype = m.group(1).strip()
        if ftype in {"let", "assert", "include"}:
            continue
        fields.append(TdField(m.group(2), ftype, (m.group(3) or "").strip(),
                              first_line + offset))
    return tuple(fields)


def collect_fields(name, classes, seen=None):
    """Own fields plus inherited ones, first declaration wins."""
    if seen is None:
        seen = set()
    if name in seen or name not in classes:
        return []
    seen.add(name)
    cls = classes[name]
    out = list(cls.fields)
    known = {f.name for f in out}
    for base in cls.bases:
        for f in collect_fields(base, classes, seen):
            if f.name not in known:
                out.append(f)
                known.add(f.name)
    return out


# --------------------------------------------------------------------------
# Contract
# --------------------------------------------------------------------------
def build_contract(root: Path, backend: Backend):
    reads, roots, expanders = scan_reads(root, backend)
    classes = parse_td(root, backend)

    root_names = list(roots)
    for name, _, _ in backend.extra_roots:
        if name not in root_names:
            root_names.append(name)

    # Fields of every consumed class (roots + helpers + ambient records).
    field_decls = {}
    for name in list(root_names) + list(backend.helper_classes):
        for f in collect_fields(name, classes):
            field_decls.setdefault(f.name, []).append((name, f))
    for cls_name, fld in backend.ambient_fields:
        if cls_name not in classes:
            raise ContractError(f"ambient record class '{cls_name}' not found")
        matches = [f for f in classes[cls_name].fields if f.name == fld]
        if not matches:
            raise ContractError(f"ambient field '{cls_name}.{fld}' not found")
        field_decls.setdefault(fld, []).append((cls_name, matches[0]))

    if not field_decls:
        raise ContractError("no .td fields found; check the td_files list")

    by_field = {}
    for r in reads:
        if r.field in field_decls:
            by_field.setdefault(r.field, []).append(r)

    # SetTheory field expanders are consumption sites without a getValue*().
    expander_sites = {}
    for cls, fld, rel, line in expanders:
        expander_sites.setdefault(fld, []).append(
            ReadSite(fld, "addFieldExpander", rel, line,
                     f'addFieldExpander("{cls}", "{fld}")', "shared"))

    root_rows = []
    for name in root_names:
        sites = list(roots.get(name, []))
        reason_sites = []
        for extra, rel, why in backend.extra_roots:
            if extra == name:
                text = (root / rel).read_text()
                hook = why.split("()")[0]
                if hook not in text:
                    raise ContractError(f"{name}: {why} not found in {rel}")
                reason_sites.append(f"{rel}:{_line_of(text, text.find(hook))}")
        root_rows.append({
            "name": name,
            "definition": (f"{classes[name].file}:{classes[name].line}"
                           if name in classes else "<unknown>"),
            "sites": [f"{f}:{l}" for f, l, _ in sites] or reason_sites,
        })
    def _def_key(r):
        path, _, line = r["definition"].rpartition(":")
        return (path, int(line) if line.isdigit() else 0)

    root_rows.sort(key=_def_key)

    rows = []
    for fld, decls in sorted(
            field_decls.items(),
            key=lambda kv: (kv[1][0][1].name if False else
                            min((classes.get(c).file, classes.get(c).line,
                                 f.line) if c in classes else ("zz", 0, 0)
                                for c, f in kv[1]))):
        sites = by_field.get(fld, []) + expander_sites.get(fld, [])
        sites.sort(key=ReadSite.sort_key)
        decls_sorted = sorted(decls, key=lambda cf: (cf[0] not in root_names,
                                                     cf[0]))

        def _uniq(values):
            seen, out = set(), []
            for v in values:
                if v not in seen:
                    seen.add(v)
                    out.append(v)
            return out

        rows.append({
            "field": fld,
            "declared_in": [c for c, _ in decls_sorted],
            "types": _uniq(f.type for _, f in decls_sorted),
            "defaults": _uniq(f.default or "（未设默认值）"
                              for _, f in decls_sorted),
            "type": decls_sorted[0][1].type,
            "default": decls_sorted[0][1].default or "（未设默认值）",
            "sites": [f"{s.file}:{s.line}" for s in sites],
            "scopes": sorted({s.scope for s in sites}),
            "effect": ANNOTATIONS.get(fld),
            "unused_note": UNUSED_NOTES.get(fld),
        })

    consumed = [r for r in rows if r["sites"]]
    unused = [r for r in rows if not r["sites"]]

    missing = [r["field"] for r in consumed if not r["effect"]]
    # Annotations must not rot either.
    known = {r["field"] for r in rows}
    stale = sorted(set(ANNOTATIONS) - known)

    magic = []
    emitter_text = strip_comments((root / backend.emitter).read_text())
    for name, pattern, effect in MAGIC_NAMES:
        lines = [_line_of(emitter_text, m.start())
                 for m in re.finditer(pattern, emitter_text)]
        if not lines:
            raise ContractError(
                f"magic name '{name}' ({pattern}) no longer appears in "
                f"{backend.emitter}; update MAGIC_NAMES")
        magic.append({"name": name,
                      "sites": [f"{backend.emitter}:{line}" for line in lines],
                      "effect": effect})

    backend_name = BACKEND_RE.search(emitter_text)
    cli_opts = CLI_OPT_RE.findall(emitter_text)
    outputs = []
    for m in OUTPUT_RE.finditer(emitter_text):
        if m.group(1) not in outputs and m.group(1).endswith(".inc"):
            outputs.append(m.group(1))

    return {
        "backend": backend.name,
        "emitter": backend.emitter,
        "backend_option": backend_name.group(1) if backend_name else None,
        "cli_options": sorted(cli_opts),
        "outputs": outputs,
        "roots": root_rows,
        "fields": rows,
        "magic": magic,
        "stats": {"roots": len(root_rows), "fields": len(rows),
                  "consumed": len(consumed), "unused": len(unused)},
        "problems": {"missing_annotations": missing, "stale_annotations": stale},
    }


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------
def _cell(text: str) -> str:
    return str(text).replace("|", "\\|")


def _sites(sites, limit=4):
    if not sites:
        return "—"
    shown = ", ".join(f"`{s}`" for s in sites[:limit])
    if len(sites) > limit:
        shown += f" …(+{len(sites) - limit})"
    return shown


def render_block(contract) -> str:
    out = [BEGIN_MARKER, ""]
    out.append(f"后端：`{contract['backend_option']}` · "
               f"emitter：`{contract['emitter']}`")
    if contract["cli_options"]:
        out.append(f"CLI：{', '.join('`-' + o + '`' for o in contract['cli_options'])}")
    if contract["outputs"]:
        out.append("产物（MultiFile）：" +
                   ", ".join(f"`{o}`" for o in contract["outputs"]) +
                   "，主文件为 `<Target>GenRegisterInfo.inc`。")
    s = contract["stats"]
    out.append(f"统计：{s['roots']} 个记录类入口 · {s['fields']} 个字段"
               f"（本后端消费 {s['consumed']}，未消费 {s['unused']}）。")
    out.append("")

    out.append("**记录类入口**")
    out.append("")
    out.append("| 记录类 | 定义 | 消费证据 |")
    out.append("| --- | --- | --- |")
    for r in contract["roots"]:
        out.append(f"| `{r['name']}` | `{r['definition']}` | "
                   f"{_sites(r['sites'], 3)} |")
    out.append("")

    out.append("**字段契约**（读取点按 `文件:行`；同一字段名可被多个类声明，"
               "TableGen 是按名字查的）")
    out.append("")
    out.append("| 字段 | 声明于 | 类型 | 默认值 | 读取点 | 语义 |")
    out.append("| --- | --- | --- | --- | --- | --- |")
    for r in contract["fields"]:
        if not r["sites"]:
            continue
        decl = ", ".join(f"`{c}`" for c in r["declared_in"])
        types = " / ".join(f"`{_cell(t)}`" for t in r["types"])
        defaults = " / ".join(f"`{_cell(d)}`" for d in r["defaults"])
        out.append(f"| `{r['field']}` | {decl} | {types} | "
                   f"{defaults} | {_sites(r['sites'])} | "
                   f"{_cell(r['effect'] or '**未注解**')} |")
    out.append("")

    out.append("**声明了但本后端不读的字段**")
    out.append("")
    out.append("| 字段 | 声明于 | 类型 | 归属 |")
    out.append("| --- | --- | --- | --- |")
    for r in contract["fields"]:
        if r["sites"]:
            continue
        decl = ", ".join(f"`{c}`" for c in r["declared_in"])
        types = " / ".join(f"`{_cell(t)}`" for t in r["types"])
        out.append(f"| `{r['field']}` | {decl} | {types} | "
                   f"{_cell(r['unused_note'] or '未知（需查其它后端）')} |")
    out.append("")

    out.append("**名字即 API**（不经过字段查找的硬编码名字）")
    out.append("")
    out.append("| 名字 | 出现位置 | 作用 |")
    out.append("| --- | --- | --- |")
    for m in contract["magic"]:
        out.append(f"| `{m['name']}` | {_sites(m['sites'], 3)} | "
                   f"{_cell(m['effect'])} |")
    out.append("")
    out.append(END_MARKER)
    return "\n".join(out)


# --------------------------------------------------------------------------
# Check / update
# --------------------------------------------------------------------------
def extract_block(text: str) -> str:
    start = text.find(BEGIN_MARKER)
    end = text.find(END_MARKER)
    if start < 0 or end < 0 or end < start:
        raise ContractError(f"markers not found ({BEGIN_MARKER} … {END_MARKER})")
    return text[start:end + len(END_MARKER)]


def check_doc(root: Path, backend: Backend, doc: Path, usage_files):
    contract = build_contract(root, backend)
    problems = []
    if contract["problems"]["missing_annotations"]:
        problems.append("consumed fields without a semantic annotation: " +
                        ", ".join(contract["problems"]["missing_annotations"]))
    if contract["problems"]["stale_annotations"]:
        problems.append("annotations for fields that no longer exist: " +
                        ", ".join(contract["problems"]["stale_annotations"]))
    expected = render_block(contract)
    text = doc.read_text()
    try:
        actual = extract_block(text)
    except ContractError as e:
        problems.append(str(e))
    else:
        if actual != expected:
            problems.append("generated contract block is out of date; "
                            "rerun with --update")
    if usage_files:
        rows = collect_usage(root, contract, [str(p) for p in usage_files])
        expected_usage = render_usage_block(rows)
        start, stop = text.find(USAGE_BEGIN), text.find(USAGE_END)
        if start < 0 or stop < 0:
            problems.append("TD usage markers missing")
        elif text[start:stop + len(USAGE_END)] != expected_usage:
            problems.append("generated TD usage block is out of date; "
                            "rerun with --update")
    if problems:
        print("dsl-surface: FAILED", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1
    print(f"dsl-surface: OK ({contract['backend']}: "
          f"{contract['stats']['consumed']}/{contract['stats']['fields']} "
          f"fields consumed, {len(contract['magic'])} magic names)")
    return 0


LET_RE = re.compile(r"\blet\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:\{[^}]*\})?\s*=")
TD_FIELD_DECL_RE = re.compile(
    r"\bfield\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*<[^;>]*>)?\s+([A-Za-z_][A-Za-z0-9_]*)")


def _display_path(root: Path, path) -> str:
    """Normalize to a repo-relative path so --update and --check agree."""
    p = Path(path)
    try:
        return str(p.resolve().relative_to(root.resolve()))
    except ValueError:
        return str(p)


def collect_usage(root: Path, contract, files):
    """Cross-check a target's .td files against the extracted contract."""
    by_field = {r["field"]: r for r in contract["fields"]}
    rows = []
    for path in files:
        rel = _display_path(root, path)
        text = strip_comments((root / rel).read_text())
        for m in LET_RE.finditer(text):
            fld = m.group(1)
            line = _line_of(text, m.start())
            r = by_field.get(fld)
            if r is None:
                status = "不在本后端契约（其它后端消费或拼写错误）"
            elif r["sites"]:
                status = "消费"
            else:
                status = r["unused_note"] or "本后端不读"
            rows.append((rel, line, fld, status))
        for m in TD_FIELD_DECL_RE.finditer(text):
            rows.append((rel, _line_of(text, m.start()), m.group(1),
                         "自定义字段（需自行保证 name-based 读取方存在）"))
    if not rows:
        raise ContractError("no `let`/`field` usages found")
    return rows


def render_usage_block(rows) -> str:
    out = [USAGE_BEGIN, "", "| 文件:行 | 字段 | 契约状态 |", "| --- | --- | --- |"]
    for rel, line, fld, status in rows:
        out.append(f"| `{rel}:{line}` | `{fld}` | {_cell(status)} |")
    unknown = sum(1 for r in rows if r[3].startswith("不在本后端契约"))
    out.extend(["", f"共 {len(rows)} 处使用；契约外 {unknown} 处。", USAGE_END])
    return "\n".join(out)


def _replace_block(text: str, begin: str, end: str, block: str) -> str:
    start, stop = text.find(begin), text.find(end)
    if start < 0 or stop < 0 or stop < start:
        raise ContractError(f"markers not found ({begin} … {end})")
    return text[:start] + block + text[stop + len(end):]


def update_doc(root: Path, backend: Backend, doc: Path, usage_files):
    contract = build_contract(root, backend)
    if contract["problems"]["missing_annotations"]:
        raise ContractError("missing annotations: " +
                            ", ".join(contract["problems"]["missing_annotations"]))
    text = doc.read_text()
    text = _replace_block(text, BEGIN_MARKER, END_MARKER,
                          render_block(contract))
    if usage_files:
        rows = collect_usage(root, contract,
                             [str(p) for p in usage_files])
        text = _replace_block(text, USAGE_BEGIN, USAGE_END,
                              render_usage_block(rows))
    doc.write_text(text)
    print(f"dsl-surface: updated {doc}")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--backend", default="register-info", choices=BACKENDS)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT,
                        help="LLVM source root")
    parser.add_argument("--json", action="store_true", help="dump the contract")
    parser.add_argument("--print-block", action="store_true",
                        help="print the generated markdown block")
    parser.add_argument("--update", type=Path, metavar="DOC")
    parser.add_argument("--check", type=Path, metavar="DOC")
    parser.add_argument("--usage", type=Path, nargs="+", metavar="TD",
                        help="cross-check these .td files against the contract")
    args = parser.parse_args(argv)

    backend = BACKENDS[args.backend]
    try:
        if args.check:
            return check_doc(args.root, backend, args.check, args.usage)
        if args.update:
            return update_doc(args.root, backend, args.update, args.usage)
        contract = build_contract(args.root, backend)
        if args.usage:
            rows = collect_usage(args.root, contract,
                                 [str(p) for p in args.usage])
            print(render_usage_block(rows))
            return 0
        if args.json:
            print(json.dumps(contract, indent=2, ensure_ascii=False))
        elif args.print_block:
            print(render_block(contract))
        else:
            print(f"{contract['backend']}: {contract['stats']}")
            if contract["problems"]["missing_annotations"]:
                print("missing annotations: " +
                      ", ".join(contract["problems"]["missing_annotations"]))
    except ContractError as e:
        print(f"dsl-surface: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
