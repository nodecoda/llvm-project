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
  4. projection matrix    <- which -gen-* backends read the same field name
  5. derivation rewrites  <- where shared C++ rewrites the model
  6. unused fields        <- declared on a root class but never read here

`--update` writes the generated tables into a markdown document, `--check`
verifies the document is still in sync and that every consumed field has a
human-written semantic annotation.  A backend change that adds a keyword
without a spec therefore fails the check instead of silently drifting.
`--status-matrix`, `--effect`, and `--summary` provide the aggregate views:
one row per `.td` usage, one field across all backends, and one row per
backend.

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
STATUS_BEGIN = "<!-- BEGIN: GENERATED CROSS-BACKEND STATUS -->"
STATUS_END = "<!-- END: GENERATED CROSS-BACKEND STATUS -->"
EFFECT_BEGIN = "<!-- BEGIN: GENERATED FIELD EFFECT -->"
SUMMARY_BEGIN = "<!-- BEGIN: GENERATED BACKEND SUMMARY -->"
EFFECT_END = "<!-- END: GENERATED FIELD EFFECT -->"
SUMMARY_END = "<!-- END: GENERATED BACKEND SUMMARY -->"


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


MAIN_OUTPUTS = {
    "register-info": "<Target>GenRegisterInfo.inc",
    "instr-info": "<Target>GenInstrInfo.inc",
    "asm-writer": "<Target>GenAsmWriter.inc",
    "asm-matcher": "<Target>GenAsmMatcher.inc",
    "dag-isel": "<Target>GenDAGISel.inc",
    "fast-isel": "<Target>GenFastISel.inc",
    "disassembler": "<Target>GenDisassemblerTables.inc",
}

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
    "instr-info": Backend(
        name="instr-info",
        emitter="llvm/utils/TableGen/InstrInfoEmitter.cpp",
        sources=(
            ("llvm/utils/TableGen/InstrInfoEmitter.cpp", "emitter"),
            ("llvm/utils/TableGen/Common/CodeGenInstruction.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenInstruction.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.h", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.cpp", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.h", "shared"),
        ),
        td_files=(
            "llvm/include/llvm/Target/Target.td",
            "llvm/include/llvm/Target/TargetInstrPredicate.td",
        ),
        roots_from=("llvm/utils/TableGen/InstrInfoEmitter.cpp",),
        # Instruction is enumerated through CodeGenTarget::getInstructions(),
        # which calls getAllDerivedDefinitions("Instruction") in a shared file.
        extra_roots=(
            ("Instruction", "llvm/utils/TableGen/Common/CodeGenTarget.cpp",
             'getAllDerivedDefinitions("Instruction")'),
        ),
        helper_classes=(
            "HwModeSelect", "RegInfoByHwMode", "RegInfo",
            "SubRegRangeByHwMode", "SubRegRange", "DwarfRegAlias",
            "RegClassByHwMode",
        ),
    ),
    "asm-writer": Backend(
        name="asm-writer",
        emitter="llvm/utils/TableGen/AsmWriterEmitter.cpp",
        sources=(
            ("llvm/utils/TableGen/AsmWriterEmitter.cpp", "emitter"),
            ("llvm/utils/TableGen/Common/AsmWriterInst.cpp", "shared"),
            ("llvm/utils/TableGen/Common/AsmWriterInst.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenInstAlias.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenInstAlias.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenInstruction.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenInstruction.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.h", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.cpp", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.h", "shared"),
        ),
        td_files=("llvm/include/llvm/Target/Target.td",),
        roots_from=("llvm/utils/TableGen/AsmWriterEmitter.cpp",),
        extra_roots=(
            ("Instruction", "llvm/utils/TableGen/Common/CodeGenTarget.cpp",
             'getAllDerivedDefinitions("Instruction")'),
            ("Register", "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
             'getAllDerivedDefinitions("Register")'),
            ("RegisterClass", "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
             'getAllDerivedDefinitions("RegisterClass")'),
            ("Operand", "llvm/utils/TableGen/InstrInfoEmitter.cpp",
             'getAllDerivedDefinitions("Operand")'),
            ("RegisterOperand", "llvm/utils/TableGen/InstrInfoEmitter.cpp",
             'getAllDerivedDefinitions("RegisterOperand")'),
            ("RegAltNameIndex", "llvm/utils/TableGen/RegisterInfoEmitter.cpp",
             "Target.getRegAltNameIndices()"),
            ("AsmWriter", "llvm/utils/TableGen/Common/CodeGenTarget.cpp",
             'getValueAsListOfDefs("AssemblyWriters")'),
        ),
        helper_classes=(
            "HwModeSelect", "RegInfoByHwMode", "RegInfo",
            "SubRegRangeByHwMode", "SubRegRange", "DwarfRegAlias",
            "RegClassByHwMode",
        ),
        ambient_fields=(("Target", "AssemblyWriters"),),
    ),
    "asm-matcher": Backend(
        name="asm-matcher",
        emitter="llvm/utils/TableGen/AsmMatcherEmitter.cpp",
        sources=(
            ("llvm/utils/TableGen/AsmMatcherEmitter.cpp", "emitter"),
            ("llvm/utils/TableGen/Common/CodeGenInstAlias.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenInstAlias.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenInstruction.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenInstruction.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.h", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.cpp", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.h", "shared"),
            ("llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp", "shared"),
            ("llvm/utils/TableGen/Common/SubtargetFeatureInfo.h", "shared"),
        ),
        td_files=(
            "llvm/include/llvm/Target/Target.td",
            "llvm/include/llvm/Target/TargetSubtarget.td",
        ),
        roots_from=(
            "llvm/utils/TableGen/AsmMatcherEmitter.cpp",
            "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
            "llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp",
        ),
        extra_roots=(
            ("Instruction", "llvm/utils/TableGen/Common/CodeGenTarget.cpp",
             'getAllDerivedDefinitions("Instruction")'),
            ("Register", "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
             'getAllDerivedDefinitions("Register")'),
            ("RegisterClass", "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
             'getAllDerivedDefinitions("RegisterClass")'),
            ("Operand", "llvm/utils/TableGen/InstrInfoEmitter.cpp",
             'getAllDerivedDefinitions("Operand")'),
            ("AsmParser", "llvm/utils/TableGen/Common/CodeGenTarget.cpp",
             "getAsmParser()"),
            ("AsmParserVariant",
             "llvm/utils/TableGen/Common/CodeGenTarget.cpp",
             "getAsmParserVariant"),
        ),
        helper_classes=(
            "HwModeSelect", "RegInfoByHwMode", "RegInfo",
            "SubRegRangeByHwMode", "SubRegRange", "DwarfRegAlias",
            "RegClassByHwMode", "DAGOperand",
        ),
        ambient_fields=(
            ("Target", "AssemblyParsers"),
            ("Target", "AssemblyParserVariants"),
        ),
    ),
    "dag-isel": Backend(
        name="dag-isel",
        emitter="llvm/utils/TableGen/DAGISelEmitter.cpp",
        sources=(
            ("llvm/utils/TableGen/DAGISelEmitter.cpp", "emitter"),
            ("llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenDAGPatterns.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenInstruction.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenInstruction.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.h", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.cpp", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.h", "shared"),
            ("llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp", "shared"),
            ("llvm/utils/TableGen/Common/SubtargetFeatureInfo.h", "shared"),
            ("llvm/utils/TableGen/Basic/SDNodeProperties.cpp", "shared"),
            ("llvm/utils/TableGen/Basic/SDNodeProperties.h", "shared"),
        ),
        td_files=(
            "llvm/include/llvm/Target/Target.td",
            "llvm/include/llvm/Target/TargetSelectionDAG.td",
            "llvm/include/llvm/Target/TargetSubtarget.td",
        ),
        roots_from=(
            "llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp",
            "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
        ),
        extra_roots=(
            ("Instruction", "llvm/utils/TableGen/Common/CodeGenTarget.cpp",
             'getAllDerivedDefinitions("Instruction")'),
            ("Register", "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
             'getAllDerivedDefinitions("Register")'),
            ("RegisterClass", "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
             'getAllDerivedDefinitions("RegisterClass")'),
            ("Operand", "llvm/utils/TableGen/InstrInfoEmitter.cpp",
             'getAllDerivedDefinitions("Operand")'),
            ("RegisterOperand", "llvm/utils/TableGen/InstrInfoEmitter.cpp",
             'getAllDerivedDefinitions("RegisterOperand")'),
        ),
        helper_classes=(
            "HwModeSelect", "RegInfoByHwMode", "RegInfo",
            "SubRegRangeByHwMode", "SubRegRange", "DwarfRegAlias",
            "RegClassByHwMode", "DAGOperand", "SDTypeProfile",
            "SDTypeConstraint", "SDTCisSameAs", "SDTCisVTSmallerThanOp",
            "SDTCisOpSmallerThanOp", "SDTCisEltOfVec", "SDTCisSubVecOfVec",
            "SDTCVecEltisVT", "SDTCisSameNumEltsAs", "SDTCisSameSizeAs",
            "ValueType", "Predicate", "ImmLeaf",
        ),
    ),
    "fast-isel": Backend(
        name="fast-isel",
        emitter="llvm/utils/TableGen/FastISelEmitter.cpp",
        sources=(
            ("llvm/utils/TableGen/FastISelEmitter.cpp", "emitter"),
            ("llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenDAGPatterns.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenInstruction.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenInstruction.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.h", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.cpp", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.h", "shared"),
            ("llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp", "shared"),
            ("llvm/utils/TableGen/Common/SubtargetFeatureInfo.h", "shared"),
            ("llvm/utils/TableGen/Basic/SDNodeProperties.cpp", "shared"),
            ("llvm/utils/TableGen/Basic/SDNodeProperties.h", "shared"),
        ),
        td_files=(
            "llvm/include/llvm/Target/Target.td",
            "llvm/include/llvm/Target/TargetSelectionDAG.td",
            "llvm/include/llvm/Target/TargetSubtarget.td",
        ),
        roots_from=(
            "llvm/utils/TableGen/Common/CodeGenDAGPatterns.cpp",
            "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
        ),
        extra_roots=(
            ("Instruction", "llvm/utils/TableGen/Common/CodeGenTarget.cpp",
             'getAllDerivedDefinitions("Instruction")'),
            ("Register", "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
             'getAllDerivedDefinitions("Register")'),
            ("RegisterClass", "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
             'getAllDerivedDefinitions("RegisterClass")'),
            ("Operand", "llvm/utils/TableGen/InstrInfoEmitter.cpp",
             'getAllDerivedDefinitions("Operand")'),
            ("RegisterOperand", "llvm/utils/TableGen/InstrInfoEmitter.cpp",
             'getAllDerivedDefinitions("RegisterOperand")'),
        ),
        helper_classes=(
            "HwModeSelect", "RegInfoByHwMode", "RegInfo",
            "SubRegRangeByHwMode", "SubRegRange", "DwarfRegAlias",
            "RegClassByHwMode", "DAGOperand", "SDTypeProfile",
            "SDTypeConstraint", "SDTCisSameAs", "SDTCisVTSmallerThanOp",
            "SDTCisOpSmallerThanOp", "SDTCisEltOfVec", "SDTCisSubVecOfVec",
            "SDTCVecEltisVT", "SDTCisSameNumEltsAs", "SDTCisSameSizeAs",
            "ValueType", "Predicate", "ImmLeaf",
        ),
    ),
    "disassembler": Backend(
        name="disassembler",
        emitter="llvm/utils/TableGen/DisassemblerEmitter.cpp",
        sources=(
            ("llvm/utils/TableGen/DisassemblerEmitter.cpp", "emitter"),
            ("llvm/utils/TableGen/DecoderEmitter.cpp", "emitter"),
            ("llvm/utils/TableGen/DecoderTableEmitter.cpp", "emitter"),
            ("llvm/utils/TableGen/DecoderTableEmitter.h", "emitter"),
            ("llvm/utils/TableGen/DecoderTree.cpp", "emitter"),
            ("llvm/utils/TableGen/DecoderTree.h", "emitter"),
            ("llvm/utils/TableGen/Common/CodeGenInstruction.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenInstruction.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenRegisters.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenTarget.h", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.cpp", "shared"),
            ("llvm/utils/TableGen/Common/CodeGenHwModes.h", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.cpp", "shared"),
            ("llvm/utils/TableGen/Common/InfoByHwMode.h", "shared"),
            ("llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp", "shared"),
            ("llvm/utils/TableGen/Common/SubtargetFeatureInfo.h", "shared"),
        ),
        td_files=(
            "llvm/include/llvm/Target/Target.td",
            "llvm/include/llvm/Target/TargetSubtarget.td",
        ),
        roots_from=(
            "llvm/utils/TableGen/DecoderEmitter.cpp",
            "llvm/utils/TableGen/Common/SubtargetFeatureInfo.cpp",
        ),
        extra_roots=(
            ("Instruction", "llvm/utils/TableGen/Common/CodeGenTarget.cpp",
             'getAllDerivedDefinitions("Instruction")'),
            ("RegClassByHwMode", "llvm/utils/TableGen/Common/CodeGenTarget.cpp",
             'getAllDerivedDefinitions("RegClassByHwMode")'),
            ("Register", "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
             'getAllDerivedDefinitions("Register")'),
            ("RegisterClass", "llvm/utils/TableGen/Common/CodeGenRegisters.cpp",
             'getAllDerivedDefinitions("RegisterClass")'),
            ("Operand", "llvm/utils/TableGen/InstrInfoEmitter.cpp",
             'getAllDerivedDefinitions("Operand")'),
            ("RegisterOperand", "llvm/utils/TableGen/InstrInfoEmitter.cpp",
             'getAllDerivedDefinitions("RegisterOperand")'),
        ),
        helper_classes=(
            "HwModeSelect", "EncodingByHwMode",
            "RegInfoByHwMode", "RegInfo",
            "SubRegRangeByHwMode", "SubRegRange", "DwarfRegAlias",
            "DAGOperand", "Predicate", "ValueType",
        ),
    ),
}


# Human-written semantics for the register-info backend.  Keys are field
# names; the same name may be declared by several classes, which is itself
# part of the contract (TableGen lookup is by name, not by type).
REGISTER_INFO_ANNOTATIONS = {
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

# Human-written semantics for -gen-instr-info.  The instruction schema is much
# larger than the register schema; most fields are plain MCID flags, but the
# operand model (OutOperandList/InOperandList/MIOperandInfo) and the
# register-class parameterization are shared with other backends.
INSTR_INFO_ANNOTATIONS = {
    # --- Instruction: operand model ---------------------------------------
    "OutOperandList": "输出操作数 dag；`CodeGenInstruction` 展开成 def 列表，决定 `NumDefs`。",
    "InOperandList": "输入操作数 dag；展开成 use 列表，参与操作数编号与 `MIOperandInfo` 解析。",
    "AsmString": "汇编模板；`CodeGenInstruction` 保存，供 asm writer/matcher 使用；本后端只解析不直接发射。",
    "Constraints": "内联汇编约束字符串；`ParseConstraints` 为每个操作数填约束位。",
    "Uses": "隐式使用寄存器列表；合并进 `ImplicitUses`，发射到 `ImplicitLists`。",
    "Defs": "隐式定义寄存器列表；合并进 `ImplicitDefs`，发射到 `ImplicitLists`。",
    "Predicates": "指令谓词列表；`computeRequiredFeatures` 用它编码每个 opcode 的 feature bitset。",
    "Namespace": "指令/寄存器命名空间；`Target.getInstNamespace()` 决定生成 enum 的 C++ namespace。",
    "Size": "指令编码字节数；`emitRecord` 写入 MCInstrDesc 的 Size。同名 `RegisterClass.Size` 是 spill size。",
    "TSFlags": "目标自定义标志位；`emitRecord` 校验位宽并写入 MCInstrDesc 的 TSFlags。同名 `RegisterClass.TSFlags` 是寄存器类标志。",
    "Instruction": "`TargetSpecializedStandardPseudoInstruction` 的基指令；用于建立特化伪指令到标准伪指令的映射。",
    "FunctionName": "`TIIPredicate` 的 C++ 函数名；`PredicateExpander` 用它生成 `InstrInfo` 方法。",
    "Body": "`TIIPredicate` 的 `MCStatement` 函数体；`expandStatement` 直接展开成目标 C++ 代码。",
    # --- Instruction: MCID flags ------------------------------------------
    "isPreISelOpcode": "MCID::PreISelOpcode；pre-isel 伪操作码。",
    "isPseudo": "MCID::Pseudo；伪指令。",
    "isMeta": "MCID::Meta；元指令，不参与调度/编码。",
    "isReturn": "MCID::Return。",
    "isEHScopeReturn": "MCID::EHScopeReturn。",
    "isBranch": "MCID::Branch。",
    "isIndirectBranch": "MCID::IndirectBranch。",
    "isCompare": "MCID::Compare。",
    "isMoveImm": "MCID::MoveImm。",
    "isMoveReg": "MCID::MoveReg。",
    "isBitcast": "MCID::Bitcast。",
    "isSelect": "MCID::Select。",
    "isBarrier": "MCID::Barrier；控制流不能 fall through。",
    "isCall": "MCID::Call。",
    "isAdd": "MCID::Add。",
    "isTrap": "MCID::Trap。",
    "canFoldAsLoad": "MCID::FoldableAsLoad。",
    "mayLoad": "MCID::MayLoad；值为 `?` 时由 pattern 推断。",
    "mayStore": "MCID::MayStore；值为 `?` 时由 pattern 推断。",
    "mayRaiseFPException": "MCID::MayRaiseFPException。",
    "isConvertibleToThreeAddress": "MCID::ConvertibleTo3Addr。",
    "isCommutable": "MCID::Commutable。",
    "isTerminator": "MCID::Terminator。",
    "isReMaterializable": "MCID::Rematerializable。",
    "isPredicable": "MCID::Predicable。",
    "isUnpredicable": "MCID::Unpredicable。",
    "hasDelaySlot": "MCID::HasDelaySlot。",
    "usesCustomInserter": "MCID::UsesCustomInserter。",
    "hasPostISelHook": "MCID::HasPostISelHook。",
    "hasCtrlDep": "MCID::HasCtrlDep。",
    "isNotDuplicable": "MCID::NotDuplicable。",
    "isConvergent": "MCID::Convergent。",
    "isAuthenticated": "MCID::Authenticated。",
    "isAsCheapAsAMove": "MCID::AsCheapAsAMove。",
    "hasExtraSrcRegAllocReq": "MCID::ExtraSrcRegAllocReq。",
    "hasExtraDefRegAllocReq": "MCID::ExtraDefRegAllocReq。",
    "isRegSequence": "MCID::RegSequence。",
    "isExtractSubreg": "MCID::ExtractSubreg。",
    "isInsertSubreg": "MCID::InsertSubreg。",
    "variadicOpsAreDefs": "MCID::VariadicOpsAreDefs。",
    "hasSideEffects": "MCID::MayLoad/MayStore 之外的副作用；值为 `?` 时由 pattern 推断。",
    "isCodeGenOnly": "仅 codegen 使用，汇编器不可见；影响 asm matcher 与 MCID 标志。",
    "hasNoSchedulingInfo": "跳过调度类分配；`SchedModels.getSchedClassIdx` 返回 0。",
    "FastISelShouldIgnore": "FastISel 忽略该指令；只保存在 `CodeGenInstruction`，不进入 MCInstrDesc。",
    "UseNamedOperandTable": "为 true 时发射 `getNamedOperandIdx` 等命名操作数表。",
    "UseLogicalOperandMappings": "为 true 时发射 `getLogicalOperandSize/Idx` 表。",
    # --- Operand / RegisterOperand ----------------------------------------
    "MIOperandInfo": "复合操作数的子操作数 dag；`CollectOperandInfo` 用它展开 MC operand 列表。",
    "OperandType": "MC operand 的 C++ 类型名；`CodeGenInstruction` 存入 `OperandType`。",
    "OperandNamespace": "MC operand 类型所在 namespace；与 `OperandType` 一起构成完整类型名。",
    "RegClass": "操作数绑定的寄存器类；解析为 `RegisterClass` 并用于寄存器操作数信息。",
    "EncoderMethod": "自定义编码函数名；`CodeGenInstruction` 保存，供 encoder 调用。",
    "PrintMethod": "自定义打印函数名；`CodeGenInstruction` 保存，供 asm writer 调用。",
    # --- RegisterClass / shared register model -----------------------------
    "Alignment": "寄存器类 spill 对齐（bit）；写入 `RegClassInfo::SpillAlignment`。",
    "AllocationPriority": "寄存器分配优先级；越高越先分配。",
    "AltOrderSelect": "`code` 注入点；非空时生成替代分配顺序选择函数。",
    "AltOrders": "备用分配顺序列表；与 `AltOrderSelect` 配合。",
    "BaseClassOrder": "寄存器类在继承层次中的顺序；影响类排序。",
    "CopyCost": "寄存器拷贝代价；8-bit，>255 fatal，负值实际按 uint8_t 截断。",
    "GeneratePressureSet": "是否生成寄存器压力集。",
    "GlobalPriority": "全局分配优先级。",
    "RegInfos": "按 HwMode 的 `{RegSize,SpillSize,SpillAlignment}` 三元组；覆盖默认模式值。",
    "RegSize": "`RegInfo` 的寄存器位数。",
    "RegTypes": "寄存器类支持的 MVT 列表；`Size==0` 时用于推断位宽。",
    "SpillAlignment": "`RegInfo` 的 spill slot 对齐。",
    "SpillSize": "`RegInfo` 的 spill slot 位数。",
    "SpillStackID": "spill stack slot 的自定义 ID；影响 frame index 分配。",
    "Weight": "寄存器类的压力权重；影响压力集计算。",
    "isAllocatable": "是否可分配；决定 `InAllocatableClassTable`。",
    "MemberList": "不是普通 list：经 SetTheory field expander 展开，决定寄存器类成员集与默认分配序。",
    "Modes": "HwMode 列表；与 `Objects` 等长，按 ModeId 绑定。",
    "Objects": "按 HwMode 绑定的对象列表；`RegisterByHwMode` 是寄存器，`RegInfoByHwMode`/`SubRegRangeByHwMode` 是参数。",
    "Offset": "子寄存器/范围的起始位；`-1` 表示不固定。",
}
# -gen-asm-writer consumes the instruction/register schemas through the same
# shared closure as instr-info, plus the alias and asm-writer-specific fields.
# Reuse the common instruction semantics and override the entries where the
# asm-writer meaning differs.
ASM_WRITER_ANNOTATIONS = {
    k: v for k, v in INSTR_INFO_ANNOTATIONS.items()
    if k not in {"Body", "FunctionName", "Instruction"}
}
ASM_WRITER_ANNOTATIONS.update({
    # --- asm-writer-specific ----------------------------------------------
    "AsmString": "汇编模板；asm writer 解析 `$operand`/`{a|b}` 并生成打印 switch。",
    "AsmWriterClassName": "`AsmWriter` 的 C++ 类名后缀；生成 `<Target><ClassName>`。",
    "PassSubtarget": "是否把 `MCSubtargetInfo` 传给 print 方法；影响生成函数签名。",
    "Variant": "汇编变体编号；展开 `{a|b|c}` 时选择第几个分支。",
    "UseBytecode": "为 true 时生成 bytecode 表 + 解释器，而不是大 switch。",
    # --- InstAlias ---------------------------------------------------------
    "EmitPriority": "`InstAlias` 的打印优先级；0 禁用别名打印，1 在无冲突时启用。",
    "ResultInst": "`InstAlias` 的目标指令 dag；决定别名展开成哪个 MCInst。",
    # --- registers / alt names --------------------------------------------
    "AsmName": "寄存器的主汇编名；asm writer 打印寄存器操作数时使用。",
    "AltNames": "寄存器的备用汇编名列表；与 `RegAltNameIndices` 配对。",
    "RegAltNameIndices": "与 `AltNames` 一一对应的别名索引；asm writer 按索引选名字。",
    "FallbackRegAltNameIndex": "备用寄存器别名索引；`AltNames` 缺失时的回退。",
    "Aliases": "寄存器别名列表；asm writer 用于别名/规范化寄存器名。",
    "CostPerUse": "每寄存器成本序列；共享寄存器模型消费。",
    "CoveredBySubRegs": "值由子寄存器决定；共享寄存器模型消费。",
    "SubRegs": "子寄存器列表；共享寄存器模型消费。",
    "SubRegIndices": "子寄存器索引；与 `SubRegs` 一一对应。",
    "isArtificial": "合成寄存器标记；asm writer 特殊处理。",
    "isConstant": "常量寄存器标记；asm writer/寄存器模型消费。",
    "MCOperandPredicate": "`Operand` 的 MC operand 谓词；asm writer 判断能否打印。",
    "Predicates": "指令/别名谓词列表；共享层解析，asm writer 用于打印条件。",
    "AssemblyWriters": "目标级 `AsmWriter` 记录列表；`-asmwriternum` 选择其中一个。",
})

# The asm matcher shares most instruction-level semantics with instr-info;
# only the fields it reads on its own records are added below.
ASM_MATCHER_ANNOTATIONS = {
    k: v for k, v in INSTR_INFO_ANNOTATIONS.items()
    if k not in {"Body", "FunctionName", "Instruction"}
}

DAG_ISEL_ANNOTATIONS = {
    k: v for k, v in INSTR_INFO_ANNOTATIONS.items()
    if k not in {"Body", "FunctionName", "Instruction"}
}

DAG_ISEL_ANNOTATIONS.update({
    # `SDNode` also declares TSFlags, so the merged note must cover both.
    "TSFlags": "目标自定义标志位：`Instruction`/`RegisterClass` 写入 MCInstrDesc/寄存器类；`SDNode.TSFlags` 是 DAG 节点标志，参与匹配。",
    "IsAPInt": "`ImmLeaf` 的立即数是否按 `APInt` 处理。",
    "IsAPFloat": "`ImmLeaf` 的立即数是否按 `APFloat` 处理。",
    # --- Pattern -----------------------------------------------------------
    "PatternToMatch": "pattern 的源 dag；`TreePattern` 从它构建匹配树。",
    "ResultInstrs": "pattern 的结果指令列表；决定选中后生成哪些 MachineInstr。",
    "GISelShouldIgnore": "为 true 时 GlobalISel 导入忽略该 pattern。",
    "AddedComplexity": "pattern/指令的优先级加成；越大越先匹配。",
    "Pattern": "指令上的 pattern 列表；与独立 `Pattern` 记录一起进入匹配器。",
    # --- PatFrags ----------------------------------------------------------
    "Operands": "`PatFrags` 的命名嵌套操作数 dag；fragment 里用 `$name` 引用。",
    "Fragments": "候选 fragment 列表；结构匹配时任一命中即可。",
    "PredicateCode": "结构匹配后的 C++ 谓词；为空表示只看结构。",
    "GISelPredicateCode": "GlobalISel 等价谓词；导入 pattern 时使用。",
    "ImmediateCode": "立即数值检查的 C++ 片段；SDISel 与 GlobalISel 共享。",
    "MemoryVT": "load/store fragment 的内存值类型。",
    "ScalarMemoryVT": "向量内存操作实际存储的标量类型。",
    "AddressSpaces": "可接受的地址空间列表；空表示任意。",
    "MinAlignment": "内存 fragment 要求的最小对齐。",
    "OperandTransform": "绑定 `$name` 时套用的 `SDNodeXForm`。",
    # --- ComplexPattern ----------------------------------------------------
    "Ty": "`ComplexPattern` 的返回值类型。",
    "NumOperands": "select 函数返回的操作数个数；`SDTypeProfile` 中则是节点操作数个数。",
    "SelectFunc": "执行复杂匹配的 C++ 函数名。",
    "RootNodes": "复杂匹配可接受的根节点列表。",
    "Properties": "结果节点需具备的 `SDNode` 属性；也用于校验 pattern 与指令一致。",
    "Complexity": "复杂 pattern 的相对优先级；越大越先匹配。",
    "WantsRoot": "把匹配根节点作为额外参数传给 select 函数。",
    "WantsParent": "把父节点作为额外参数传给 select 函数。",
    # --- SDNode / SDNodeXForm / SDTypeProfile ------------------------------
    "Opcode": "`SDNode` 的 opcode 字符串（`ISD::XXX`）；`SDNodeXForm` 中则是被变换的节点。",
    "SDClass": "生成的 C++ SDNode 子类名；决定 cast 的目标类型。",
    "TypeProfile": "`SDTypeProfile` 引用；给出结果/操作数个数与类型约束。",
    "IsStrictFP": "严格浮点节点标记；影响匹配与重排。",
    "NumResults": "`SDTypeProfile` 的结果个数。",
    "Constraints": "`SDTypeProfile` 的类型约束列表。",
    "XFormFunction": "`SDNodeXForm` 的 C++ 变换函数体。",
    "OperandNum": "`SDTypeConstraint` 约束的操作数编号。",
    "OtherOperandNum": "与 `OperandNum` 比较的另一个操作数编号。",
    "BigOperandNum": "`SDTCisOpSmallerThanOp` 中较大的操作数编号。",
    "OtherOpNum": "`SDTCisEltOfVec`/`SDTCisSubVecOfVec` 的另一个操作数编号。",
    "VT": "约束要求的 `ValueType`。",
    # --- Instruction / Operand --------------------------------------------
    "CodeSize": "指令的 code size 成本；DAGISel 用它排序匹配候选。",
    "Type": "操作数的 `ValueType`；类型推断时使用。",
    "DefaultOps": "`OperandWithDefaultOps` 的默认操作数 dag。",
    # --- Predicate ---------------------------------------------------------
    "CondString": "谓词的 C++ 条件字符串；pattern 把它拼进匹配代码。",
    "AssemblerMatcherPredicate": "是否作为汇编匹配谓词参与生成。",
    "AssemblerCondDag": "汇编器谓词的 dag 条件。",
    "RecomputePerFunction": "是否每个函数重新计算该谓词。",
    # --- shared register model --------------------------------------------
    "Aliases": "寄存器别名列表；共享寄存器模型合并别名。",
    "AsmName": "寄存器主汇编名；共享寄存器模型建立名字表。",
    "CostPerUse": "每寄存器使用成本序列；共享寄存器模型消费。",
    "CoveredBySubRegs": "值由子寄存器决定；共享寄存器模型消费。",
    "isArtificial": "合成寄存器标记；不参与用户可见匹配。",
    "isConstant": "常量寄存器标记；共享寄存器模型消费。",
    "SubRegs": "子寄存器列表；共享寄存器模型消费。",
    "SubRegIndices": "与 `SubRegs` 一一对应的子寄存器索引。",
    "RegAsmNames": "`RegisterTuples` 展开时覆盖每个合成寄存器的 `AsmName`。",
    "Classes": "`RegisterCategory` 包含的寄存器类。",
    "ComposedOf": "复合子寄存器索引的组成列表。",
    "CoveringSubRegIndices": "覆盖该子寄存器索引的父索引列表。",
    "SubRegRanges": "按 HwMode 的子寄存器范围。",
})

ASM_MATCHER_ANNOTATIONS.update({
    # --- AsmOperandClass / AsmParserVariant -------------------------------
    "Name": "操作数类的 `MCK_<Name>` 枚举名与默认方法名前缀；`AsmParserVariant` 中则是变体名，用于过滤 `AsmVariantName`。",
    "SuperClasses": "操作数类的父类列表；决定分类层级与匹配优先级。",
    "PredicateMethod": "操作数类判定方法名；未设时由 `Name` 派生 `is<Name>`。",
    "RenderMethod": "操作数写回 MCInst 的方法名；未设时派生 `add<Name>Operands`。",
    "ParserMethod": "自定义操作数解析方法名；非空时生成 parse switch 分支。",
    "DiagnosticType": "匹配失败时使用的诊断类型；`AsmParser` 侧映射成文本。",
    "DiagnosticString": "参数化诊断文本；用于非法操作数报错。",
    "IsOptional": "该操作数类是否可省略；匹配结束时允许缺少。",
    "DefaultMethod": "可选操作数的默认构造方法名；未设时由 `Name` 派生 `default<Name>Operands`。",
    # --- AsmParser ---------------------------------------------------------
    "AsmParserClassName": "生成解析器的类名后缀；默认 `AsmParser`。",
    "AsmParserInstCleanup": "每条指令匹配成功后调用的目标自定义清理函数；空则不生成。",
    "ShouldEmitMatchRegisterName": "是否生成主寄存器名匹配函数。",
    "ShouldEmitMatchRegisterAltName": "是否生成备用寄存器名匹配函数。",
    "ShouldEmitMatchRegisterAltNameWithIndex": "是否按 AltNameIndex 生成逐索引匹配函数，不做回退。",
    "AllowDuplicateRegisterNames": "允许寄存器名重复；控制是否仍生成匹配函数并去重。",
    "HasMnemonicFirst": "输入是否总是助记符在前；决定匹配表布局。",
    "OperandParserMethod": "自定义操作数解析入口方法名；生成调用点使用。",
    "CallCustomParserForAllOperands": "对所有操作数调用自定义 parser，而不仅是声明了 `ParserMethod` 的类。",
    "EmitTokenDiagnosticTypes": "为 token 匹配失败生成带诊断类型的返回值。",
    "PreferSmallerInstructions": "同等匹配时优先较小（`Size` 小）的指令。",
    "ReportMultipleNearMisses": "生成 near-miss 候选列表，而不是只报第一个失败。",
    "PrioritizeFeatureInMultiMismatchFallback": "多重不匹配回退时优先报告 feature 冲突。",
    # --- AsmParserVariant --------------------------------------------------
    "Variant": "汇编变体编号；写入 `AsmVariantID` 并参与匹配比较。",
    "CommentDelimiter": "汇编模板中的注释定界符。",
    "RegisterPrefix": "寄存器 token 前缀；用于自动识别寄存器。",
    "TokenizingCharacters": "独立成 token 的字符集合（默认 `[]*!`）。",
    "SeparatorCharacters": "不属于 token 的分隔字符（默认空格/制表/逗号）。",
    "BreakCharacters": "开始新标识符的字符集合；tokenizer 使用。",
    # --- InstAlias / MnemonicAlias / TokenAlias ---------------------------
    "ResultInst": "别名展开的目标指令 dag；`CodeGenInstAlias` 解析成 MCInst 操作数。",
    "UseInstAsmMatchConverter": "是否复用目标指令的 `AsmMatchConverter`。",
    "AsmVariantName": "所属汇编变体；非空时只在同名变体下参与匹配。",
    "FromMnemonic": "`MnemonicAlias` 的源助记符；生成助记符重写表。",
    "ToMnemonic": "`MnemonicAlias` 的目标助记符；重写后的规范形式。",
    "FromToken": "`TokenAlias` 的源 token 字面量。",
    "ToToken": "`TokenAlias` 的目标 token 字面量；建立 token 子类关系。",
    # --- Instruction / Operand --------------------------------------------
    "AsmMatchConverter": "自定义汇编匹配转换函数名；非空时替代 dag 生成的转换代码。",
    "HasPositionOrder": "指令是否带位置排序；影响 alias/匹配排序。",
    "TwoOperandAliasConstraint": "两操作数 alias 的额外约束表达式；asm matcher 生成检查。",
    "ParserMatchClass": "操作数对应的 `AsmOperandClass`；决定分类与自定义解析。",
    # --- Predicate ---------------------------------------------------------
    "AssemblerMatcherPredicate": "是否作为汇编匹配谓词参与生成。",
    "AssemblerCondDag": "汇编器谓词的 dag 条件；展开成 feature 检查代码。",
    "CondString": "谓词条件的字符串形式；`SubtargetFeatureInfo` 用其判空。",
    "PredicateName": "生成 `getSubtargetFeatureName` 时使用的谓词名。",
    "RecomputePerFunction": "是否每个函数重新计算该谓词。",
    # --- registers / register tuples / subreg indices ----------------------
    "AsmName": "寄存器主汇编名；生成寄存器名匹配表。",
    "AltNames": "寄存器备用汇编名；asm matcher 生成 alt-name 匹配表。",
    "RegAltNameIndices": "与 `AltNames` 一一对应的别名索引。",
    "Aliases": "寄存器别名列表；共享寄存器模型合并别名。",
    "CostPerUse": "每寄存器使用成本序列；共享寄存器模型消费。",
    "CoveredBySubRegs": "值由子寄存器决定；共享寄存器模型消费。",
    "SubRegs": "子寄存器列表；共享寄存器模型消费。",
    "SubRegIndices": "与 `SubRegs` 一一对应的子寄存器索引。",
    "isArtificial": "合成寄存器标记；不参与用户可见匹配。",
    "isConstant": "常量寄存器标记；共享寄存器模型消费。",
    "RegAsmNames": "`RegisterTuples` 展开时覆盖每个合成寄存器的 `AsmName`。",
    "Classes": "`RegisterCategory` 包含的寄存器类；共享寄存器模型用于分类。",
    "CoerceFromRegClass": "匹配时把该寄存器类强制转换到另一个类。",
    "ComposedOf": "复合子寄存器索引的组成列表。",
    "CoveringSubRegIndices": "覆盖该子寄存器索引的父索引列表。",
    "SubRegRanges": "按 HwMode 的子寄存器范围。",
    # --- Target ------------------------------------------------------------
    "AssemblyParsers": "目标选用的 `AsmParser` 记录列表；`-asmparsernum` 选择。",
    "AssemblyParserVariants": "汇编变体记录列表；决定 `Variant` 匹配循环。",
})

FAST_ISEL_ANNOTATIONS = dict(DAG_ISEL_ANNOTATIONS)
FAST_ISEL_ANNOTATIONS.update({
    "FastIselShouldIgnore": "为 true 时 fast-isel 跳过该立即数谓词的 pattern，不进入 lookup 表。",
})

DISASSEMBLER_ANNOTATIONS = {
    k: v for k, v in INSTR_INFO_ANNOTATIONS.items()
    if k not in {"Body", "FunctionName", "Instruction"}
}
DISASSEMBLER_ANNOTATIONS.update({
    k: REGISTER_INFO_ANNOTATIONS[k]
    for k in (
        "Aliases", "AsmName", "CostPerUse", "CoveredBySubRegs",
        "SubRegIndices", "SubRegs", "isArtificial", "isConstant",
    )
})
DISASSEMBLER_ANNOTATIONS.update({
    k: ASM_MATCHER_ANNOTATIONS[k]
    for k in (
        "AssemblerCondDag", "AssemblerMatcherPredicate",
        "CondString", "RecomputePerFunction",
    )
})
DISASSEMBLER_ANNOTATIONS.update({
    "DecoderNamespace": "解码器命名空间；`decodeInstruction` 按它选择子表。",
    "EncodingInfos": "按 HwMode 的 encoding 选择；非空时忽略 `Instruction.Inst`。",
    "isAsmParserOnly": "只给汇编器用的指令；反汇编器跳过。",
    "AliasOf": "`AdditionalEncoding` 指向的真实指令；解码结果归到它。",
})

ANNOTATIONS = {
    "register-info": REGISTER_INFO_ANNOTATIONS,
    "instr-info": INSTR_INFO_ANNOTATIONS,
    "asm-writer": ASM_WRITER_ANNOTATIONS,
    "asm-matcher": ASM_MATCHER_ANNOTATIONS,
    "dag-isel": DAG_ISEL_ANNOTATIONS,
    "fast-isel": FAST_ISEL_ANNOTATIONS,
    "disassembler": DISASSEMBLER_ANNOTATIONS,
}

# Fields that are declared on a consumed class but intentionally not read by
# this backend.  The note points at the backend that owns them.
REGISTER_INFO_UNUSED_NOTES = {
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

INSTR_INFO_UNUSED_NOTES = {
    "AddedComplexity": "isel pattern 优先级；`-gen-dag-isel` 消费。",
    "AsmMatchConverter": "asm matcher 的自定义转换器。",
    "AsmVariantName": "asm variant 选择。",
    "CodeSize": "isel 的 code size 排序；`-gen-dag-isel` 消费。",
    "CoerceFromRegClass": "asm matcher 的寄存器类强制。",
    "DecoderMethod": "decoder 自定义方法。",
    "DecoderNamespace": "decoder 生成 namespace。",
    "DiagnosticString": "asm matcher 诊断文本。",
    "DiagnosticType": "asm matcher 诊断类型。",
    "DwarfAlias": "DWARF 别名；`-gen-register-info` 消费。",
    "EncodingInfos": "按 HwMode 的编码；encoder/decoder 消费。",
    "GISelMatchGenericTypes": "GlobalISel 泛型类型匹配。",
    "GIZeroRegister": "GlobalISel 零寄存器。",
    "HasPositionOrder": "asm matcher 的位置排序。",
    "Itinerary": "旧调度模型；`-gen-instr-info` 不读。",
    "MCOperandPredicate": "MC operand 谓词；asm writer 消费。",
    "MemberList": "寄存器类成员集合；SetTheory 展开，`-gen-register-info` 消费。",
    "ParserMatchClass": "asm parser 匹配类。",
    "Pattern": "isel pattern；`-gen-dag-isel` 消费。",
    "PostEncoderMethod": "编码后处理函数。",
    "SchedRW": "新调度模型读写资源。",
    "TwoOperandAliasConstraint": "两操作数 alias 约束；asm matcher 消费。",
    "Type": "操作数 ValueType；isel/asm 消费。",
    "altNameIndex": "寄存器类别名索引；asm writer 消费。",
    "hasCompleteDecoder": "decoder 完整性标记。",
    "isAsmParserOnly": "仅汇编器使用，codegen 不可见。",
    "supportsDeactivationSymbol": "指令级 deactivation 标记；其它后端消费。",
}
ASM_WRITER_UNUSED_NOTES = dict(INSTR_INFO_UNUSED_NOTES)
# MCOperandPredicate is consumed by asm-writer, so drop the instr-info note.
ASM_WRITER_UNUSED_NOTES.pop("MCOperandPredicate", None)
ASM_WRITER_UNUSED_NOTES.update({
    "DwarfNumbers": "DWARF 编号；`-gen-register-info` 消费。",
    "HWEncoding": "寄存器编码；`-gen-register-info` 消费。",
    "PositionOrder": "寄存器枚举顺序；核心排序器消费。",
    "UseInstAsmMatchConverter": "别名是否复用目标的 AsmMatchConverter；asm matcher 消费。",
    "UseLogicalOperandMappings": "逻辑操作数映射开关；`-gen-instr-info` 消费。",
    "UseNamedOperandTable": "命名操作数表开关；`-gen-instr-info` 消费。",
})

DAG_ISEL_UNUSED_NOTES = dict(INSTR_INFO_UNUSED_NOTES)
DAG_ISEL_UNUSED_NOTES.update({
    "HasNoUse": "预置谓词标记；`PredicateExpanderDag` 生成检查，不在本 source set。",
    "HasOneUse": "预置谓词标记；`PredicateExpanderDag` 生成检查，不在本 source set。",
    "IsLoad": "预置内存谓词；`PredicateExpanderDag` 生成检查。",
    "IsStore": "预置内存谓词；`PredicateExpanderDag` 生成检查。",
    "IsAtomic": "预置内存谓词；`PredicateExpanderDag` 生成检查。",
    "IsUnindexed": "预置内存谓词；`PredicateExpanderDag` 生成检查。",
    "IsNonExtLoad": "预置加载谓词；`PredicateExpanderDag` 生成检查。",
    "IsAnyExtLoad": "预置加载谓词；`PredicateExpanderDag` 生成检查。",
    "IsSignExtLoad": "预置加载谓词；`PredicateExpanderDag` 生成检查。",
    "IsZeroExtLoad": "预置加载谓词；`PredicateExpanderDag` 生成检查。",
    "IsTruncStore": "预置存储谓词；`PredicateExpanderDag` 生成检查。",
    "IsAtomicOrderingMonotonic": "预置原子序谓词；`PredicateExpanderDag` 生成检查。",
    "IsAtomicOrderingAcquire": "预置原子序谓词；`PredicateExpanderDag` 生成检查。",
    "IsAtomicOrderingRelease": "预置原子序谓词；`PredicateExpanderDag` 生成检查。",
    "IsAtomicOrderingAcquireRelease": "预置原子序谓词；`PredicateExpanderDag` 生成检查。",
    "IsAtomicOrderingSequentiallyConsistent": "预置原子序谓词；`PredicateExpanderDag` 生成检查。",
    "IsAtomicOrderingAcquireOrStronger": "预置原子序谓词；`PredicateExpanderDag` 生成检查。",
    "IsAtomicOrderingReleaseOrStronger": "预置原子序谓词；`PredicateExpanderDag` 生成检查。",
    "PredicateCodeUsesOperands": "允许 `PredicateCode` 引用 `Operands`；匹配器生成阶段消费。",
    "PredicateName": "`getSubtargetFeatureName` 生成用；`-gen-subtarget`/asm matcher 消费。",
    "FastIselShouldIgnore": "fast-isel 的过滤开关；dag-isel 不读它。",
})

ASM_MATCHER_UNUSED_NOTES = dict(INSTR_INFO_UNUSED_NOTES)
ASM_MATCHER_UNUSED_NOTES.update({
    "DwarfNumbers": "DWARF 编号；`-gen-register-info` 消费。",
    "HWEncoding": "寄存器编码；`-gen-register-info` 消费。",
    "PositionOrder": "寄存器枚举顺序；核心排序器消费。",
    "UseLogicalOperandMappings": "逻辑操作数映射开关；`-gen-instr-info` 消费。",
    "UseNamedOperandTable": "命名操作数表开关；`-gen-instr-info` 消费。",
    "EmitPriority": "别名打印优先级；`-gen-asm-writer` 消费，asm matcher 不读。",
    "isAsmParserOnly": "仅汇编器可见的标记；本后端的匹配表不直接读它。",
})

UNUSED_NOTES = {
    "register-info": REGISTER_INFO_UNUSED_NOTES,
    "instr-info": INSTR_INFO_UNUSED_NOTES,
    "asm-writer": ASM_WRITER_UNUSED_NOTES,
    "asm-matcher": ASM_MATCHER_UNUSED_NOTES,
    "dag-isel": DAG_ISEL_UNUSED_NOTES,
    "fast-isel": dict(DAG_ISEL_UNUSED_NOTES),
    "disassembler": dict(ASM_MATCHER_UNUSED_NOTES),
}

# Name-level contract: record names and generated identifiers that are not
# reachable through field lookup.  Each entry is (name, regex that must still
# exist in the sources, effect).
REGISTER_INFO_MAGIC_NAMES = (
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
# -gen-instr-info has no name-level API: it compares target/opcode names, not
# magic def names, so this table is intentionally empty.
ASM_WRITER_MAGIC_NAMES = (
    ("NoRegAltName",
     r'"NoRegAltName"',
     "缺少 AltName 时的哨兵；asm writer 回退到主 `AsmName`。"),
    ("PHI",
     r'"PHI"',
     "asm writer 跳过 PHI 的别名排序/打印。"),
)

ASM_MATCHER_MAGIC_NAMES = (
    ("Imm", r'"Imm"',
     "内置立即数操作数类；`ParserMatchClass` 为 Imm 时走立即数分类。"),
    ("MCK_", r'"MCK_"',
     "生成的操作数类枚举名前缀；`MatchClassKind` 的基本单位。"),
)

MAGIC_NAMES = {
    "register-info": REGISTER_INFO_MAGIC_NAMES,
    "instr-info": (),
    "asm-writer": ASM_WRITER_MAGIC_NAMES,
    "asm-matcher": ASM_MATCHER_MAGIC_NAMES,
    "fast-isel": (),
    "disassembler": (),
    "dag-isel": (
        ("set", r'"set"', "pattern 结果绑定节点；分隔匹配树与结果树。"),
        ("node", r'"node"', "未类型化叶子的占位节点。"),
        ("srcvalue", r'"srcvalue"', "源值叶子；表示已加载的值。"),
        ("null_frag", r'"null_frag"', "空 fragment；使 pattern 失效。"),
        ("immAllOnesV / immAllZerosV", r'"immAllOnesV"|"immAllZerosV"',
         "全 1/全 0 向量立即数；类型推断特殊处理。"),
        ("INSERT_SUBREG / REG_SEQUENCE", r'"INSERT_SUBREG"|"REG_SEQUENCE"',
         "结果树里的寄存器合成伪节点；决定操作数映射。"),
    ),
}

MATRIX_BEGIN = "<!-- BEGIN: GENERATED PROJECTION MATRIX -->"
MATRIX_END = "<!-- END: GENERATED PROJECTION MATRIX -->"
DERIV_BEGIN = "<!-- BEGIN: GENERATED DERIVATIONS -->"
DERIV_END = "<!-- END: GENERATED DERIVATIONS -->"

# Derivation-layer rewrites: places where shared C++ changes what a record
# means before any backend projects it.  (kind, schema key, regex, effect);
# collect_derivations() fails the check if a pattern no longer matches.
REGISTER_INFO_DERIVATIONS = (
    ("SetTheory expander", "RegisterClass.MemberList",
     r'Sets\.addFieldExpander\("RegisterClass", "MemberList"\)',
     "把 dag 展开成寄存器集合；决定成员集与默认分配序。"),
    ("SetTheory expander", "CalleeSavedRegs.SaveList",
     r'Sets\.addFieldExpander\("CalleeSavedRegs", "SaveList"\)',
     "把 dag 展开成保存顺序，生成 `<name>_SaveList`。"),
    ("tuple synthesis", "RegisterTuples",
     r'Sets\.addExpander\("RegisterTuples"',
     "在 backend 运行时合成新的 Record（`SynthDefs`），不进入 RecordKeeper。"),
    ("tuple rewrite", "RegisterTuples.SubRegs",
     r'if \(Field == "SubRegs"\)',
     "把原型的 SubRegs 替换成 zip 出来的 tuple。"),
    ("tuple rewrite", "RegisterTuples.CoveredBySubRegs",
     r'if \(Field == "CoveredBySubRegs"\)',
     "合成寄存器强制 `CoveredBySubRegs = true`。"),
    ("tuple rewrite", "RegisterTuples.DwarfNumbers",
     r'Field == "DwarfNumbers"',
     "`DwarfNumbers`/`DwarfAlias`/`Aliases` 重置为 Register 类默认值。"),
    ("sorting", "Register.PositionOrder",
     r'llvm::sort\(Regs, LessRecordRegister\(\)\)',
     "消费端排序；决定寄存器枚举顺序（`LessRecordRegister` 读 `PositionOrder`）。"),
    ("inference", "RegisterClass",
     r'computeInferredRegisterClasses',
     "从显式寄存器类推断出缺失的类；只存在于派生模型。"),
    ("hw-mode selection", "HwModeSelect.Objects",
     r'zip_equal\(Modes, Objects\)',
     "按 ModeId 把 `Objects[i]` 绑定到 `Modes[i]`。"),
    ("parameterization", "RegInfo",
     r'RegSize = R->getValueAsInt\("RegSize"\)',
     "把 `RegInfo` 三元组转成内部 `RegSizeInfo`（按 HwMode 存）。"),
    ("parameterization", "SubRegRange",
     r'SubRegRange::SubRegRange',
     "把 `SubRegIndex.Size/Offset` 或 `SubRegRange` 转成内部 `SubRegRange`。"),
)

INSTR_INFO_DERIVATIONS = (
    ("operand list expansion", "Instruction.OutOperandList/InOperandList",
     r'CGIOperandList::CGIOperandList',
     "把 dag 展开成 OperandInfo；同时解析 `MIOperandInfo` 复合操作数。"),
    ("constraint parsing", "Instruction.Constraints",
     r'ParseConstraints\(',
     "把约束字符串拆成每个操作数的约束位。"),
    ("instruction ordering", "Instruction",
     r'getAllDerivedDefinitions\("Instruction"\)',
     "按记录名排序，决定 opcode 编号与 `NumberedInstructions` 顺序。"),
    ("pseudo specialization", "TargetSpecializedStandardPseudoInstruction",
     r'buildTargetSpecializedPseudoInstsMap',
     "建立特化伪指令到标准伪指令的映射，并继承其操作数/标志。"),
    ("hw-mode selection", "HwModeSelect.Objects",
     r'zip_equal\(Modes, Objects\)',
     "按 ModeId 把 `Objects[i]` 绑定到 `Modes[i]`。"),
    ("inference", "RegisterClass",
     r'computeInferredRegisterClasses',
     "从显式寄存器类推断出缺失的类；只存在于派生模型。"),
    ("parameterization", "RegInfo",
     r'RegSize = R->getValueAsInt\("RegSize"\)',
     "把 `RegInfo` 三元组转成内部 `RegSizeInfo`。"),
    ("operand type resolution", "RegisterOperand.RegClass",
     r'getValueAsDef\("RegClass"\)',
     "把 `RegisterOperand` 解析到具体 `RegisterClass`，决定寄存器操作数信息。"),
)
ASM_WRITER_DERIVATIONS = (
    ("alias construction", "InstAlias",
     r'CodeGenInstAlias::CodeGenInstAlias',
     "把 `ResultInst` dag 展开成别名操作数，并解析寄存器/立即数。"),
    ("asm string parsing", "Instruction.AsmString",
     r'AsmWriterInst::AsmWriterInst',
     "把汇编模板拆成 literal/operand/variant 片段，供打印 switch 使用。"),
    ("asm writer selection", "Target.AssemblyWriters",
     r'getValueAsListOfDefs\("AssemblyWriters"\)',
     "按 `-asmwriternum` 选择目标的一个 `AsmWriter`。"),
    ("instruction ordering", "Instruction",
     r'getAllDerivedDefinitions\("Instruction"\)',
     "按记录名排序，决定 opcode 编号与打印表顺序。"),
    ("register ordering", "Register",
     r'getAllDerivedDefinitions\("Register"\)',
     "按 `PositionOrder`/名字排序，决定寄存器枚举与名字表顺序。"),
    ("alt-name resolution", "Register.RegAltNameIndices",
     r'getRegAltNameIndices',
     "把 `RegAltNameIndices` 解析成实际的别名索引表。"),
    ("hw-mode selection", "HwModeSelect.Objects",
     r'zip_equal\(Modes, Objects\)',
     "按 ModeId 把 `Objects[i]` 绑定到 `Modes[i]`。"),
    ("operand type resolution", "RegisterOperand.RegClass",
     r'getValueAsDef\("RegClass"\)',
     "把 `RegisterOperand` 解析到具体 `RegisterClass`。"),
)

DAG_ISEL_DERIVATIONS = (
    ("pattern collection", "Pattern",
     r'getAllDerivedDefinitions\("Pattern"\)',
     "收集独立 `Pattern` 并展开成 `TreePattern`。"),
    ("instruction pattern merge", "Instruction.Pattern",
     r'getValueAsListInit\("Pattern"\)',
     "把指令上的 `Pattern` 列表并入匹配器。"),
    ("fragment flattening", "PatFrags.Fragments",
     r'getValueAsListInit\("Fragments"\)',
     "把候选 fragment 展开成可匹配的树。"),
    ("type inference", "SDNode.TypeProfile",
     r'TypeInfer::',
     "按类型约束推断每个节点/操作数的 `ValueType`。"),
    ("predicate lowering", "Predicate.CondString",
     r'getValueAsString\("CondString"\)',
     "把谓词条件拼进匹配树。"),
    ("node registration", "SDNode",
     r'getAllDerivedDefinitions\("SDNode"\)',
     "注册 `SDNode` 的 opcode、属性与类型 profile。"),
    ("operand type resolution", "Operand.Type",
     r'getValueAsDef\("Type"\)',
     "把操作数解析到 `ValueType`。"),
)

ASM_MATCHER_DERIVATIONS = (
    ("alias construction", "InstAlias",
     r'CodeGenInstAlias::CodeGenInstAlias',
     "把 `ResultInst` dag 展开成别名操作数，并解析寄存器/立即数。"),
    ("operand class hierarchy", "AsmOperandClass.SuperClasses",
     r'getValueAsListInit\("SuperClasses"\)',
     "把父类列表展开成 `isSubclass()` 判定的偏序关系。"),
    ("register ordering", "Register",
     r'getAllDerivedDefinitions\("Register"\)',
     "按 `PositionOrder`/名字排序，决定寄存器枚举与匹配表顺序。"),
    ("operand type resolution", "RegisterOperand.RegClass",
     r'getValueAsDef\("RegClass"\)',
     "把 `RegisterOperand` 解析到具体 `RegisterClass`。"),
    ("mnemonic lowering", "MnemonicAlias",
     r'getAllDerivedDefinitions\("MnemonicAlias"\)',
     "收集助记符别名，生成匹配前的重写表。"),
    ("token substitution", "TokenAlias",
     r'getAllDerivedDefinitions\("TokenAlias"\)',
     "把 FromToken 的匹配类标记为 ToToken 的子类。"),
    ("subtarget predicate collection", "Predicate.AssemblerMatcherPredicate",
     r'SubtargetFeatureInfo::getAll',
     "收集汇编器谓词并分配 feature bit，供匹配表做条件检查。"),
)

DERIVATIONS = {
    "register-info": REGISTER_INFO_DERIVATIONS,
    "instr-info": INSTR_INFO_DERIVATIONS,
    "asm-writer": ASM_WRITER_DERIVATIONS,
    "asm-matcher": ASM_MATCHER_DERIVATIONS,
    "dag-isel": DAG_ISEL_DERIVATIONS,
    "disassembler": (
        ("encoding collection", "Instruction",
         r'getAllDerivedDefinitions\("Instruction"\)',
         "收集非伪指令，逐条检查 encoding 是否可解码。"),
        ("additional encodings", "AdditionalEncoding",
         r'getAllDerivedDefinitions\("AdditionalEncoding"\)',
         "把额外 encoding 归并到 `AliasOf` 指向的指令。"),
        ("hwmode encoding select", "EncodingByHwMode",
         r'getValueAsOptionalDef\("EncodingInfos"\)',
         "按 HwMode 取得 `EncodingByHwMode.Objects` 里的具体 encoding。"),
        ("predicate lowering", "Predicate.AssemblerMatcherPredicate",
         r'getValueAsListOfDefs\("Predicates"\)',
         "把汇编匹配谓词下沉成 MCDecoder 的 feature 检查。"),
    ),
    "fast-isel": (
        ("pattern filtering", "ImmLeaf.FastIselShouldIgnore",
         r'getValueAsBit\("FastIselShouldIgnore"\)',
         "跳过显式标记的立即数谓词 fragment，只把简单 pattern 收进 lookup 表。"),
        ("pattern collection", "Pattern",
         r'getAllDerivedDefinitions\("Pattern"\)',
         "与 dag-isel 共用同一套 `TreePattern` 构建。"),
        ("type inference", "SDNode.TypeProfile",
         r'TypeInfer::',
         "按类型约束推断节点/操作数的 `ValueType`。"),
        ("operand type resolution", "Operand.RegClass",
         r'getValueAsDef\("RegClass"\)',
         "把操作数解析到具体 `RegisterClass`。"),
    ),
}


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
                # `class Foo<...> : Bar<...>;` has no body but still carries
                # an inheritance list; forward declarations have neither.
                # Never let a forward declaration shadow a real definition.
                header = text[m.end():j]
                bases = _parse_bases(header)
                if bases and name not in classes:
                    classes[name] = TdClass(
                        name, kind, rel, _line_of(text, m.start()), bases, ())
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
    """Extract base class names, ignoring template-argument commas."""
    depth = 0
    for idx, ch in enumerate(header):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif ch == ":" and depth == 0:
            bases, cur, depth = [], "", 0
            for ch2 in header[idx + 1:] + ",":
                if ch2 in "<(":
                    depth += 1
                elif ch2 in ">)":
                    depth -= 1
                if ch2 == "," and depth == 0:
                    m = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)", cur)
                    if m:
                        bases.append(m.group(1))
                    cur = ""
                else:
                    cur += ch2
            return tuple(bases)
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
    """Own fields plus inherited ones as (field, declaring_class).

    Keeping the declaring class is what lets the contract separate the
    register schema from unrelated schemas that happen to use the same field
    name (Namespace, Size, Objects, ...).
    """
    if seen is None:
        seen = set()
    if name in seen or name not in classes:
        return []
    seen.add(name)
    cls = classes[name]
    out = [(f, name) for f in cls.fields]
    known = {f.name for f in cls.fields}
    for base in cls.bases:
        for f, decl in collect_fields(base, classes, seen):
            if f.name not in known:
                out.append((f, decl))
                known.add(f.name)
    return out


# --------------------------------------------------------------------------
# Contract
# --------------------------------------------------------------------------
GEN_RE = re.compile(r'"(gen-[A-Za-z0-9-]+)"')


def _reader_name(rel: str, text: str) -> str:
    """Map a TableGen source file to the projection that owns it."""
    if "/Common/" in rel or "/Basic/" in rel:
        return "shared"
    regs = sorted(set(GEN_RE.findall(text)))
    if regs:
        return ", ".join(r[len("gen-"):] for r in regs)
    return "helper:" + Path(rel).stem


def scan_matrix(root: Path, field_names):
    """field -> {reader: [file:line]} over every TableGen backend source."""
    tg = root / "llvm/utils/TableGen"
    out = {}
    files = sorted(list(tg.rglob("*.cpp")) + list(tg.rglob("*.h")))
    for path in files:
        rel = str(path.relative_to(root))
        text = strip_comments(path.read_text())
        reader = _reader_name(rel, text)
        for m in READ_RE.finditer(text):
            fld = m.group(2)
            if fld not in field_names:
                continue
            line = text.count("\n", 0, m.start()) + 1
            out.setdefault(fld, {}).setdefault(reader, []).append(
                f"{rel}:{line}")
    return out


def collect_derivations(root: Path, backend: Backend):
    """Check the derivation-layer rewrites registered for this backend."""
    rows = []
    for kind, key, pattern, effect in DERIVATIONS[backend.name]:
        sites = []
        for rel, _ in backend.sources:
            text = strip_comments((root / rel).read_text())
            for m in re.finditer(pattern, text):
                sites.append(f"{rel}:{_line_of(text, m.start())}")
        if not sites:
            raise ContractError(
                f"derivation '{key}' ({pattern}) no longer matches; "
                f"update DERIVATIONS")
        rows.append({"kind": kind, "key": key, "sites": sites,
                     "effect": effect})
    return rows


def build_contract(root: Path, backend: Backend):
    reads, roots, expanders = scan_reads(root, backend)
    classes = parse_td(root, backend)

    root_names = list(roots)
    for name, _, _ in backend.extra_roots:
        if name not in root_names:
            root_names.append(name)

    # field -> {declaring_class: TdField}.  A field name may be declared by
    # several independent schemas (Namespace, Size, Objects, ...); keeping the
    # declaring class is what makes the contract schema-aware.
    field_decls = {}
    reached_from = {}
    for rname in list(root_names) + list(backend.helper_classes):
        for f, decl in collect_fields(rname, classes, set()):
            field_decls.setdefault(f.name, {})[decl] = f
            reached_from.setdefault(f.name, set()).add(rname)
    for cls_name, fld in backend.ambient_fields:
        if cls_name not in classes:
            raise ContractError(f"ambient record class '{cls_name}' not found")
        matches = [f for f in classes[cls_name].fields if f.name == fld]
        if not matches:
            raise ContractError(f"ambient field '{cls_name}.{fld}' not found")
        field_decls.setdefault(fld, {})[cls_name] = matches[0]
        reached_from.setdefault(fld, set()).add(cls_name)

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

    matrix = scan_matrix(root, set(field_decls))

    def _uniq(values):
        seen, out = set(), []
        for v in values:
            if v not in seen:
                seen.add(v)
                out.append(v)
        return out

    rows = []
    for fld, decl_map in field_decls.items():
        decl_items = sorted(
            decl_map.items(),
            key=lambda kv: (classes[kv[0]].file, classes[kv[0]].line,
                            kv[1].line))
        sites = by_field.get(fld, []) + expander_sites.get(fld, [])
        sites.sort(key=ReadSite.sort_key)
        scopes = {s.scope for s in sites}
        if not sites:
            stage = "schema"
        elif "emitter" in scopes:
            stage = "project" if scopes == {"emitter"} else "derive+project"
        else:
            stage = "derive"
        readers = matrix.get(fld, {})
        other = sorted(k for k in readers
                       if k != "shared" and k != backend.name)
        rows.append({
            "field": fld,
            "declared_in": [c for c, _ in decl_items],
            "reached_from": sorted(reached_from.get(fld, ())),
            "types": _uniq(f.type for _, f in decl_items),
            "defaults": _uniq(f.default or "（未设默认值）"
                              for _, f in decl_items),
            "stage": stage,
            "ambiguous": len(decl_items) > 1,
            "sites": [f"{s.file}:{s.line}" for s in sites],
            "site_details": [{"file": s.file, "line": s.line,
                              "accessor": s.accessor, "scope": s.scope,
                              "snippet": s.snippet} for s in sites],
            "shared": "shared" in readers,
            "projects": [k for k in other if not k.startswith("helper:")],
            "helpers": [k for k in other if k.startswith("helper:")],
            "readers": readers,
            "effect": ANNOTATIONS[backend.name].get(fld),
            "unused_note": UNUSED_NOTES[backend.name].get(fld),
        })

    rows.sort(key=lambda r: (
        min((classes[c].file, classes[c].line) for c in r["declared_in"]),
        r["field"]))

    consumed = [r for r in rows if r["sites"]]
    unused = [r for r in rows if not r["sites"]]

    missing = [r["field"] for r in consumed if not r["effect"]]
    # Annotations must not rot either.
    known = {r["field"] for r in rows}
    stale = sorted(set(ANNOTATIONS[backend.name]) - known)

    magic = []
    emitter_text = strip_comments((root / backend.emitter).read_text())
    # Magic names may be compared in the emitter or in shared C++ that the
    # backend delegates to, so search the whole source set.
    source_texts = [(rel, strip_comments((root / rel).read_text()))
                    for rel, _ in backend.sources]
    for name, pattern, effect in MAGIC_NAMES[backend.name]:
        sites = []
        for rel, text in source_texts:
            for m in re.finditer(pattern, text):
                site = f"{rel}:{_line_of(text, m.start())}"
                if site not in sites:
                    sites.append(site)
        if not sites:
            raise ContractError(
                f"magic name '{name}' ({pattern}) no longer appears in the "
                f"sources of {backend.name}; update MAGIC_NAMES")
        magic.append({"name": name, "sites": sites, "effect": effect})

    derivations = collect_derivations(root, backend)

    backend_name = BACKEND_RE.search(emitter_text)
    cli_opts = CLI_OPT_RE.findall(emitter_text)
    outputs = []
    for m in OUTPUT_RE.finditer(emitter_text):
        if m.group(1) not in outputs and m.group(1).endswith(".inc"):
            outputs.append(m.group(1))

    stages = {k: sum(1 for r in rows if r["stage"] == k)
              for k in ("schema", "derive", "project", "derive+project")}

    return {
        "backend": backend.name,
        "emitter": backend.emitter,
        "backend_option": backend_name.group(1) if backend_name else None,
        "cli_options": sorted(cli_opts),
        "outputs": outputs,
        "main_output": MAIN_OUTPUTS[backend.name],
        "roots": root_rows,
        "fields": rows,
        "magic": magic,
        "derivations": derivations,
        "stats": {"roots": len(root_rows), "fields": len(rows),
                  "consumed": len(consumed), "unused": len(unused),
                  "stages": stages},
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
                   f"，主文件为 `{contract['main_output']}`。")
    s = contract["stats"]
    st = s["stages"]
    out.append(f"统计：{s['roots']} 个记录类入口 · {s['fields']} 个字段"
               f"（schema {st['schema']} · derive {st['derive']} · "
               f"project {st['project']} · derive+project {st['derive+project']}）。")
    out.append("")

    out.append("**记录类入口**")
    out.append("")
    out.append("| 记录类 | 定义 | 消费证据 |")
    out.append("| --- | --- | --- |")
    for r in contract["roots"]:
        out.append(f"| `{r['name']}` | `{r['definition']}` | "
                   f"{_sites(r['sites'], 3)} |")
    out.append("")

    out.append("**字段契约**（主键是 `(declaring class, field)`；"
               "`stage` 表示它停在哪一层，`⚠` 表示同名多声明）")
    out.append("")
    out.append("| 字段 | 声明于 | 类型 | 默认值 | stage | 本后端读取点 | 语义 |")
    out.append("| --- | --- | --- | --- | --- | --- | --- |")
    for r in contract["fields"]:
        if not r["sites"]:
            continue
        decl = ", ".join(f"`{c}`" for c in r["declared_in"])
        types = " / ".join(f"`{_cell(t)}`" for t in r["types"])
        defaults = " / ".join(f"`{_cell(d)}`" for d in r["defaults"])
        stage = f"`{r['stage']}`" + (" ⚠" if r["ambiguous"] else "")
        out.append(f"| `{r['field']}` | {decl} | {types} | "
                   f"{defaults} | {stage} | {_sites(r['sites'])} | "
                   f"{_cell(r['effect'] or '**未注解**')} |")
    out.append("")
    out.append("`stage`：`schema` 仅声明 · `derive` 只被共享派生层读 · "
               "`project` 只被本 emitter 读 · `derive+project` 两者都读。")
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

    if contract["magic"]:
        out.append("**名字即 API**（不经过字段查找的硬编码名字）")
        out.append("")
        out.append("| 名字 | 出现位置 | 作用 |")
        out.append("| --- | --- | --- |")
        for m in contract["magic"]:
            out.append(f"| `{m['name']}` | {_sites(m['sites'], 3)} | "
                       f"{_cell(m['effect'])} |")
        out.append("")
    else:
        out.append("本后端没有硬编码记录名（名字只作为数据/生成标识符使用）。")
        out.append("")
    out.append(END_MARKER)
    return "\n".join(out)


def render_matrix_block(contract) -> str:
    s = contract["stats"]
    rows = [r for r in contract["fields"]
            if r["projects"] or r["helpers"] or r["ambiguous"]]
    out = [MATRIX_BEGIN, "",
           "只列“本后端之外还有读者”或“同名多声明”的字段；"
           "`shared` 表示共享派生层也读它。", "",
           "| 字段 | 声明于 | shared | 其它 `-gen-*` | helper | 同名 |",
           "| --- | --- | --- | --- | --- | --- |"]
    for r in rows:
        decl = ", ".join(f"`{c}`" for c in r["declared_in"])
        projects = ", ".join(f"`{p}`" for p in r["projects"]) or "—"
        helpers = ", ".join(f"`{h}`" for h in r["helpers"]) or "—"
        out.append(f"| `{r['field']}` | {decl} | "
                   f"{'✓' if r['shared'] else '—'} | {projects} | "
                   f"{helpers} | {'⚠' if r['ambiguous'] else '—'} |")
    out.extend(["", f"矩阵覆盖 {len(rows)} 个字段（共 {s['fields']} 个）。",
                MATRIX_END])
    return "\n".join(out)


def render_derivations_block(rows) -> str:
    out = [DERIV_BEGIN, "",
           "Record 里只有声明；这些是共享 C++ 在投影之前对语义的改写。", "",
           "| 改写 | Schema 键 | 证据 | 说明 |",
           "| --- | --- | --- | --- |"]
    for r in rows:
        out.append(f"| `{r['kind']}` | `{r['key']}` | "
                   f"{_sites(r['sites'], 2)} | {_cell(r['effect'])} |")
    out.append(DERIV_END)
    return "\n".join(out)


# --------------------------------------------------------------------------
# Check / update
# --------------------------------------------------------------------------
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


def collect_td_usages(root: Path, files):
    """Raw `let`/`field` usages: (file, line, field, is_custom_field)."""
    rows = []
    for path in files:
        rel = _display_path(root, path)
        text = strip_comments((root / rel).read_text())
        for m in LET_RE.finditer(text):
            rows.append((rel, _line_of(text, m.start()), m.group(1), False))
        for m in TD_FIELD_DECL_RE.finditer(text):
            rows.append((rel, _line_of(text, m.start()), m.group(1), True))
    if not rows:
        raise ContractError("no `let`/`field` usages found")
    return rows


def collect_usage(root: Path, contract, files):
    """Cross-check a target's .td files against one backend contract."""
    by_field = {r["field"]: r for r in contract["fields"]}
    rows = []
    for rel, line, fld, custom in collect_td_usages(root, files):
        if custom:
            status = "自定义字段（需自行保证 name-based 读取方存在）"
        else:
            r = by_field.get(fld)
            if r is None:
                status = "不在本后端契约（其它后端消费或拼写错误）"
            elif r["sites"]:
                status = f"消费（{r['stage']}）"
            else:
                status = r["unused_note"] or "本后端不读"
        rows.append((rel, line, fld, status))
    return rows


def collect_status_rows(root: Path, contracts, files):
    """One row per .td usage with a status cell for every backend."""
    by_backend = [
        (c["backend"], {r["field"]: r for r in c["fields"]})
        for c in contracts
    ]
    rows = []
    for rel, line, fld, custom in collect_td_usages(root, files):
        cells, decls = [], set()
        for _, fields in by_backend:
            r = fields.get(fld)
            if custom:
                cells.append("自定义")
            elif r is None:
                cells.append("—")
            elif r["sites"]:
                cells.append(r["stage"])
            else:
                cells.append("未消费")
            if r:
                decls.update(r["declared_in"])
        rows.append({"file": rel, "line": line, "field": fld,
                     "custom": custom, "declared_in": sorted(decls),
                     "cells": cells})
    return rows


def render_status_matrix(rows, backend_names) -> str:
    stages = {"derive", "project", "derive+project"}
    out = [STATUS_BEGIN, "",
           "同一份 `.td` 在每个后端下的投影状态；"
           "`—` 表示该字段不在这个后端的 schema 里。", "",
           "| 文件:行 | 字段 | 声明于 | " +
           " | ".join(f"`{n}`" for n in backend_names) + " |",
           "| --- | --- | --- | " + " | ".join("---" for _ in backend_names) +
           " |"]
    for r in rows:
        decl = ", ".join(f"`{c}`" for c in r["declared_in"]) or "—"
        cells = [f"`{c}`" if c in stages else c for c in r["cells"]]
        out.append(f"| `{r['file']}:{r['line']}` | `{r['field']}` | {decl} | " +
                   " | ".join(cells) + " |")
    total = len(rows)
    any_consumed = sum(
        1 for r in rows if any(c in stages for c in r["cells"]))
    all_consumed = sum(
        1 for r in rows if all(c in stages for c in r["cells"]))
    custom = sum(1 for r in rows if r["custom"])
    out.extend([
        "",
        f"共 {total} 处使用；{len(backend_names)} 个后端都消费 "
        f"{all_consumed} 处，至少一个消费 {any_consumed} 处，"
        f"自定义字段 {custom} 处。",
        STATUS_END])
    return "\n".join(out)


def render_effect(root: Path, contracts, field: str) -> str:
    """Field-centric view: declarations, per-backend stages, all readers."""
    decl_map = {}
    projections = []
    for c in contracts:
        by_field = {r["field"]: r for r in c["fields"]}
        r = by_field.get(field)
        if r is None:
            projections.append((c["backend"], None))
            continue
        for decl in r["declared_in"]:
            entry = decl_map.setdefault(
                decl, {"types": set(), "defaults": set(), "backends": set()})
            entry["types"].update(r["types"])
            entry["defaults"].update(r["defaults"])
            entry["backends"].add(c["backend"])
        projections.append((c["backend"], r))

    readers = scan_matrix(root, {field}).get(field, {})
    out = [EFFECT_BEGIN, "", f"影响面：`{field}`", ""]

    out.extend(["**声明**", "",
                "| declaring class | 类型 | 默认值 | 出现于后端 |",
                "| --- | --- | --- | --- |"])
    if decl_map:
        for decl in sorted(decl_map):
            e = decl_map[decl]
            types = " / ".join(f"`{_cell(t)}`" for t in sorted(e["types"]))
            defaults = " / ".join(f"`{_cell(d)}`"
                                  for d in sorted(e["defaults"]))
            backs = ", ".join(f"`{b}`" for b in sorted(e["backends"]))
            out.append(f"| `{decl}` | {types} | {defaults} | {backs} |")
    else:
        out.append("| — | — | — | 不在已配置后端的 schema 里 |")
    out.append("")

    out.extend(["**投影**", "",
                "| backend | stage | 读取点 | 语义 |",
                "| --- | --- | --- | --- |"])
    for backend, r in projections:
        if r is None:
            out.append(f"| `{backend}` | — | — | 不在该后端 schema 里 |")
        elif r["sites"]:
            out.append(f"| `{backend}` | `{r['stage']}` | "
                       f"{_sites(r['sites'], 3)} | "
                       f"{_cell(r['effect'] or '**未注解**')} |")
        else:
            note = r["unused_note"] or "本后端不读"
            out.append(f"| `{backend}` | 未消费 | — | {_cell(note)} |")
    out.append("")

    out.extend(["**所有读取点**（扫描 `llvm/utils/TableGen/**`）", "",
                "| reader | 读取点 |", "| --- | --- |"])
    if readers:
        for reader in sorted(readers):
            out.append(f"| `{reader}` | {_sites(readers[reader], 4)} |")
    else:
        out.append("| — | 没有任何 `getValue*` 读取点 |")
    out.append("")

    consumed = sum(1 for _, r in projections if r and r["sites"])
    out.extend([
        f"共 {len(decl_map)} 个声明类 · {consumed}/{len(projections)} 个后端消费"
        f" · {len(readers)} 个 reader。",
        EFFECT_END])
    return "\n".join(out)


def render_summary(contracts) -> str:
    """One row per backend: the acceptance table for the tutorial."""
    out = [SUMMARY_BEGIN, "",
           "| backend | roots | 字段 | 消费 | derive | project | both | "
           "schema | magic | derivations |",
           "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    totals = {"roots": 0, "fields": 0, "consumed": 0, "schema": 0,
              "derive": 0, "project": 0, "derive+project": 0,
              "magic": 0, "derivations": 0}
    for c in contracts:
        st = c["stats"]
        stages = st["stages"]
        row = {
            "roots": st["roots"],
            "fields": st["fields"],
            "consumed": st["consumed"],
            "schema": stages["schema"],
            "derive": stages["derive"],
            "project": stages["project"],
            "derive+project": stages["derive+project"],
            "magic": len(c["magic"]),
            "derivations": len(c["derivations"]),
        }
        for k, v in row.items():
            totals[k] += v
        out.append(
            f"| `{c['backend']}` | {row['roots']} | {row['fields']} | "
            f"{row['consumed']} | {row['derive']} | {row['project']} | "
            f"{row['derive+project']} | {row['schema']} | {row['magic']} | "
            f"{row['derivations']} |")
    out.extend([
        "",
        f"共 {len(contracts)} 个后端 · {totals['roots']} 个 root 条目 · "
        f"{totals['fields']} 个后端字段条目 · {totals['consumed']} 个被消费 · "
        f"{totals['magic']} 个 magic name · {totals['derivations']} 条派生改写。",
        SUMMARY_END])
    return "\n".join(out)


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


def _get_block(text: str, begin: str, end: str) -> str:
    start, stop = text.find(begin), text.find(end)
    if start < 0 or stop < 0 or stop < start:
        raise ContractError(f"markers not found ({begin} … {end})")
    return text[start:stop + len(end)]


def build_all_contracts(root: Path):
    return [build_contract(root, be) for be in BACKENDS.values()]


def _doc_blocks(root, backend, contract, usage_files, status_matrix=False,
                effect=None, summary=False):
    if summary:
        contracts = build_all_contracts(root)
        return [(SUMMARY_BEGIN, SUMMARY_END, render_summary(contracts))]
    if effect:
        contracts = build_all_contracts(root)
        return [(EFFECT_BEGIN, EFFECT_END,
                 render_effect(root, contracts, effect))]
    if status_matrix:
        if not usage_files:
            raise ContractError("--status-matrix requires --usage")
        contracts = build_all_contracts(root)
        rows = collect_status_rows(root, contracts,
                                   [str(p) for p in usage_files])
        names = [c["backend"] for c in contracts]
        return [(STATUS_BEGIN, STATUS_END, render_status_matrix(rows, names))]
    blocks = [
        (BEGIN_MARKER, END_MARKER, render_block(contract)),
        (MATRIX_BEGIN, MATRIX_END, render_matrix_block(contract)),
        (DERIV_BEGIN, DERIV_END,
         render_derivations_block(contract["derivations"])),
    ]
    if usage_files:
        rows = collect_usage(root, contract, [str(p) for p in usage_files])
        blocks.append((USAGE_BEGIN, USAGE_END, render_usage_block(rows)))
    return blocks


def check_doc(root: Path, backend: Backend, doc: Path, usage_files,
              status_matrix=False, effect=None, summary=False):
    all_backends = status_matrix or bool(effect) or summary
    contract = None if all_backends else build_contract(root, backend)
    contracts = build_all_contracts(root) if all_backends else [contract]
    problems = []
    for c in contracts:
        prefix = "" if not all_backends else f"{c['backend']}: "
        if c["problems"]["missing_annotations"]:
            problems.append(prefix +
                            "consumed fields without a semantic annotation: " +
                            ", ".join(c["problems"]["missing_annotations"]))
        if c["problems"]["stale_annotations"]:
            problems.append(prefix +
                            "annotations for fields that no longer exist: " +
                            ", ".join(c["problems"]["stale_annotations"]))
    text = doc.read_text()
    for begin, end, expected in _doc_blocks(root, backend, contract,
                                            usage_files, status_matrix,
                                            effect, summary):
        try:
            actual = _get_block(text, begin, end)
        except ContractError as e:
            problems.append(str(e))
            continue
        if actual != expected:
            problems.append(f"{begin} is out of date; rerun with --update")
    if problems:
        print("dsl-surface: FAILED", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1
    if summary:
        print(f"dsl-surface: OK (backend summary: {len(contracts)} backends)")
        return 0
    if effect:
        print(f"dsl-surface: OK (field effect: {effect}, "
              f"{len(contracts)} backends)")
        return 0
    if status_matrix:
        rows = collect_status_rows(root, contracts,
                                   [str(p) for p in usage_files])
        print(f"dsl-surface: OK (cross-backend status: {len(rows)} usages, "
              f"{len(contracts)} backends)")
        return 0
    st = contract["stats"]["stages"]
    print(f"dsl-surface: OK ({contract['backend']}: "
          f"{contract['stats']['consumed']}/{contract['stats']['fields']} "
          f"fields consumed [{st['derive']} derive, {st['project']} project, "
          f"{st['derive+project']} both], {len(contract['magic'])} magic names, "
          f"{len(contract['derivations'])} derivations)")
    return 0


def update_doc(root: Path, backend: Backend, doc: Path, usage_files,
               status_matrix=False, effect=None, summary=False):
    all_backends = status_matrix or bool(effect) or summary
    contract = None if all_backends else build_contract(root, backend)
    contracts = build_all_contracts(root) if all_backends else [contract]
    for c in contracts:
        if c["problems"]["missing_annotations"]:
            raise ContractError(
                f"{c['backend']}: missing annotations: " +
                ", ".join(c["problems"]["missing_annotations"]))
    text = doc.read_text()
    for begin, end, block in _doc_blocks(root, backend, contract, usage_files,
                                         status_matrix, effect, summary):
        text = _replace_block(text, begin, end, block)
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
                        help="print the generated contract block")
    parser.add_argument("--matrix", action="store_true",
                        help="print the cross-backend projection matrix")
    parser.add_argument("--status-matrix", action="store_true",
                        help="print/update/check a merged .td status table "
                             "for every backend (requires --usage)")
    parser.add_argument("--effect", metavar="FIELD",
                        help="print/update/check the impact of one .td field")
    parser.add_argument("--summary", action="store_true",
                        help="print/update/check the seven-backend summary "
                             "table")
    parser.add_argument("--update", type=Path, metavar="DOC")
    parser.add_argument("--check", type=Path, metavar="DOC")
    parser.add_argument("--usage", type=Path, nargs="+", metavar="TD",
                        help="cross-check these .td files against the contract")
    args = parser.parse_args(argv)

    backend = BACKENDS[args.backend]
    try:
        if args.effect and args.status_matrix:
            raise ContractError("--effect and --status-matrix are exclusive")
        if args.summary and (args.effect or args.status_matrix):
            raise ContractError("--summary is exclusive with --effect / "
                                "--status-matrix")
        if args.effect and not (args.check or args.update):
            contracts = build_all_contracts(args.root)
            print(render_effect(args.root, contracts, args.effect))
            return 0
        if args.status_matrix and not (args.check or args.update):
            if not args.usage:
                raise ContractError("--status-matrix requires --usage")
            contracts = build_all_contracts(args.root)
            rows = collect_status_rows(args.root, contracts,
                                       [str(p) for p in args.usage])
            print(render_status_matrix(rows,
                                       [c["backend"] for c in contracts]))
            return 0
        if args.summary and not (args.check or args.update):
            print(render_summary(build_all_contracts(args.root)))
            return 0
        if args.check:
            return check_doc(args.root, backend, args.check, args.usage,
                             args.status_matrix, args.effect, args.summary)
        if args.update:
            return update_doc(args.root, backend, args.update, args.usage,
                              args.status_matrix, args.effect, args.summary)
        contract = build_contract(args.root, backend)
        if args.usage:
            rows = collect_usage(args.root, contract,
                                 [str(p) for p in args.usage])
            print(render_usage_block(rows))
            return 0
        if args.json:
            print(json.dumps(contract, indent=2, ensure_ascii=False))
        elif args.matrix:
            print(render_matrix_block(contract))
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
