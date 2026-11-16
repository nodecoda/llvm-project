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


MAIN_OUTPUTS = {
    "register-info": "<Target>GenRegisterInfo.inc",
    "instr-info": "<Target>GenInstrInfo.inc",
    "asm-writer": "<Target>GenAsmWriter.inc",
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
})

ANNOTATIONS = {
    "register-info": REGISTER_INFO_ANNOTATIONS,
    "instr-info": INSTR_INFO_ANNOTATIONS,
    "asm-writer": ASM_WRITER_ANNOTATIONS,
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

UNUSED_NOTES = {
    "register-info": REGISTER_INFO_UNUSED_NOTES,
    "instr-info": INSTR_INFO_UNUSED_NOTES,
    "asm-writer": ASM_WRITER_UNUSED_NOTES,
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

MAGIC_NAMES = {
    "register-info": REGISTER_INFO_MAGIC_NAMES,
    "instr-info": (),
    "asm-writer": ASM_WRITER_MAGIC_NAMES,
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

DERIVATIONS = {
    "register-info": REGISTER_INFO_DERIVATIONS,
    "instr-info": INSTR_INFO_DERIVATIONS,
    "asm-writer": ASM_WRITER_DERIVATIONS,
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
    for name, pattern, effect in MAGIC_NAMES[backend.name]:
        lines = [_line_of(emitter_text, m.start())
                 for m in re.finditer(pattern, emitter_text)]
        if not lines:
            raise ContractError(
                f"magic name '{name}' ({pattern}) no longer appears in "
                f"{backend.emitter}; update MAGIC_NAMES")
        magic.append({"name": name,
                      "sites": [f"{backend.emitter}:{line}" for line in lines],
                      "effect": effect})

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
                status = f"消费（{r['stage']}）"
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


def _get_block(text: str, begin: str, end: str) -> str:
    start, stop = text.find(begin), text.find(end)
    if start < 0 or stop < 0 or stop < start:
        raise ContractError(f"markers not found ({begin} … {end})")
    return text[start:stop + len(end)]


def _doc_blocks(root, backend, contract, usage_files):
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


def check_doc(root: Path, backend: Backend, doc: Path, usage_files):
    contract = build_contract(root, backend)
    problems = []
    if contract["problems"]["missing_annotations"]:
        problems.append("consumed fields without a semantic annotation: " +
                        ", ".join(contract["problems"]["missing_annotations"]))
    if contract["problems"]["stale_annotations"]:
        problems.append("annotations for fields that no longer exist: " +
                        ", ".join(contract["problems"]["stale_annotations"]))
    text = doc.read_text()
    for begin, end, expected in _doc_blocks(root, backend, contract,
                                            usage_files):
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
    st = contract["stats"]["stages"]
    print(f"dsl-surface: OK ({contract['backend']}: "
          f"{contract['stats']['consumed']}/{contract['stats']['fields']} "
          f"fields consumed [{st['derive']} derive, {st['project']} project, "
          f"{st['derive+project']} both], {len(contract['magic'])} magic names, "
          f"{len(contract['derivations'])} derivations)")
    return 0


def update_doc(root: Path, backend: Backend, doc: Path, usage_files):
    contract = build_contract(root, backend)
    if contract["problems"]["missing_annotations"]:
        raise ContractError("missing annotations: " +
                            ", ".join(contract["problems"]["missing_annotations"]))
    text = doc.read_text()
    for begin, end, block in _doc_blocks(root, backend, contract, usage_files):
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
