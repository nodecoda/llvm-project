//===----- RVTrimMergeBaseOffset.cpp - Optimise address calculations  ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Merge the offset of address calculation into the offset field
// of instructions in a global address lowering sequence.
//
//===----------------------------------------------------------------------===//

#include "RVTrim.h"
#include "RVTrimSubtarget.h"
#include "RVTrimTargetMachine.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/TargetOptions.h"
#include <optional>
using namespace llvm;

#define DEBUG_TYPE "rvtrim-merge-base-offset"
#define RVTrim_MERGE_BASE_OFFSET_NAME "RISC-V Merge Base Offset"
namespace {

class RVTrimMergeBaseOffsetLegacy : public MachineFunctionPass {
public:
  static char ID;
  bool runOnMachineFunction(MachineFunction &MF) override;

  RVTrimMergeBaseOffsetLegacy() : MachineFunctionPass(ID) {}

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setIsSSA();
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override {
    return RVTrim_MERGE_BASE_OFFSET_NAME;
  }
};
} // end anonymous namespace

char RVTrimMergeBaseOffsetLegacy::ID = 0;
INITIALIZE_PASS(RVTrimMergeBaseOffsetLegacy, DEBUG_TYPE,
                RVTrim_MERGE_BASE_OFFSET_NAME, false, false)

// Detect either of the patterns:
//
// 1. (medlow pattern):
//   a. lui   vreg1, %hi(s)
//      addi  vreg2, vreg1, %lo(s)
//
//   b.  qc.e.li vreg1, s
//
// 2. (medany pattern):
// .Lpcrel_hi1:
//   auipc vreg1, %pcrel_hi(s)
//   addi  vreg2, vreg1, %pcrel_lo(.Lpcrel_hi1)
//
// The pattern is only accepted if:
//    1) The first instruction has only one use, which is the ADDI.
//    2) The address operands have the appropriate type, reflecting the
//       lowering of a global address or constant pool using medlow or medany.
//    3) The offset value in the Global Address or Constant Pool is 0.
static bool detectFoldable(MachineInstr &Hi, MachineInstr *&Lo,
                           MachineRegisterInfo *MRI) {
  auto HiOpc = Hi.getOpcode();
  if (HiOpc != RVTrim::LUI && HiOpc != RVTrim::AUIPC &&
      HiOpc != RVTrim::PseudoMovAddr && HiOpc != RVTrim::QC_E_LI)
    return false;

  const MachineOperand &HiOp1 = Hi.getOperand(1);
  unsigned ExpectedFlags = HiOpc == RVTrim::AUIPC     ? RVTrimII::MO_PCREL_HI
                           : HiOpc == RVTrim::QC_E_LI ? RVTrimII::MO_None
                                                     : RVTrimII::MO_HI;
  if (HiOp1.getTargetFlags() != ExpectedFlags)
    return false;

  if (!(HiOp1.isGlobal() || HiOp1.isCPI() || HiOp1.isBlockAddress()) ||
      HiOp1.getOffset() != 0)
    return false;

  if (HiOpc == RVTrim::PseudoMovAddr || HiOpc == RVTrim::QC_E_LI) {
    // Most of the code should handle it correctly without modification by
    // setting Lo and Hi both point to PseudoMovAddr/QC_E_LI
    Lo = &Hi;
  } else {
    Register HiDestReg = Hi.getOperand(0).getReg();
    if (!MRI->hasOneUse(HiDestReg))
      return false;

    Lo = &*MRI->use_instr_begin(HiDestReg);
    if (Lo->getOpcode() != RVTrim::ADDI)
      return false;
  }

  if (HiOpc != RVTrim::QC_E_LI) {
    const MachineOperand &LoOp2 = Lo->getOperand(2);
    if (HiOpc == RVTrim::LUI || HiOpc == RVTrim::PseudoMovAddr) {
      if (LoOp2.getTargetFlags() != RVTrimII::MO_LO ||
          !(LoOp2.isGlobal() || LoOp2.isCPI() || LoOp2.isBlockAddress()) ||
          LoOp2.getOffset() != 0)
        return false;
    } else {
      assert(HiOpc == RVTrim::AUIPC);
      if (LoOp2.getTargetFlags() != RVTrimII::MO_PCREL_LO ||
          LoOp2.getType() != MachineOperand::MO_MCSymbol)
        return false;
    }
  }

  if (HiOp1.isGlobal()) {
    LLVM_DEBUG(dbgs() << "  Found lowered global address: "
                      << *HiOp1.getGlobal() << "\n");
  } else if (HiOp1.isBlockAddress()) {
    LLVM_DEBUG(dbgs() << "  Found lowered basic address: "
                      << *HiOp1.getBlockAddress() << "\n");
  } else if (HiOp1.isCPI()) {
    LLVM_DEBUG(dbgs() << "  Found lowered constant pool: " << HiOp1.getIndex()
                      << "\n");
  }

  return true;
}

// Update the offset in Hi and Lo instructions.
// Delete the tail instruction and update all the uses to use the
// output from Lo.
static bool foldOffset(MachineInstr &Hi, MachineInstr &Lo, MachineInstr &Tail,
                       int64_t Offset, MachineRegisterInfo *MRI) {
  assert(isInt<32>(Offset) && "Unexpected offset");

  // If Hi is an AUIPC, don't fold the offset if it is outside the bounds of
  // the global object. The object may be within 2GB of the PC, but addresses
  // outside of the object might not be.
  auto HiOpc = Hi.getOpcode();
  if (HiOpc == RVTrim::AUIPC && Hi.getOperand(1).isGlobal()) {
    const GlobalValue *GV = Hi.getOperand(1).getGlobal();
    Type *Ty = GV->getValueType();
    if (!Ty->isSized() || Offset < 0 ||
        (uint64_t)Offset > GV->getDataLayout().getTypeAllocSize(Ty))
      return false;
  }

  // Salvage the debug users of Lo's result by subtracting the offset again.
  SmallVector<uint64_t, 3> UndoOffset;
  DIExpression::appendOffset(UndoOffset, -Offset);
  for (MachineOperand &MO : MRI->use_operands(Lo.getOperand(0).getReg())) {
    MachineInstr &DbgMI = *MO.getParent();
    if (!DbgMI.isDebugValue())
      continue;
    DbgMI.getDebugExpressionOp().setMetadata(
        DIExpression::appendOpsToArg(DbgMI.getDebugExpression(), UndoOffset,
                                     DbgMI.getDebugOperandIndex(&MO)));
  }

  // Put the offset back in Hi and the Lo
  Hi.getOperand(1).setOffset(Offset);
  if (Hi.getOpcode() != RVTrim::AUIPC && Hi.getOpcode() != RVTrim::QC_E_LI)
    Lo.getOperand(2).setOffset(Offset);
  // Delete the tail instruction.
  Register LoOp0Reg = Lo.getOperand(0).getReg();
  Register TailOp0Reg = Tail.getOperand(0).getReg();
  MRI->constrainRegClass(LoOp0Reg, MRI->getRegClass(TailOp0Reg));
  MRI->replaceRegWith(TailOp0Reg, LoOp0Reg);
  Tail.eraseFromParent();
  LLVM_DEBUG(dbgs() << "  Merged offset " << Offset << " into base.\n"
                    << "     " << Hi << "     " << Lo;);
  return true;
}

// Detect patterns for large offsets that are passed into an ADD instruction.
// If the pattern is found, updates the offset in Hi and Lo instructions
// and deletes TailAdd and the instructions that produced the offset.
//
//                     Base address lowering is of the form:
//                       Hi:  lui   vreg1, %hi(s)
//                       Lo:  addi  vreg2, vreg1, %lo(s)
//                       /                                  \
//                      /                                    \
//                     /                                      \
//                    /  The large offset can be of two forms: \
//  1) Offset that has non zero bits in lower      2) Offset that has non zero
//     12 bits and upper 20 bits                      bits in upper 20 bits only
//   OffseLUI: lui   vreg3, 4
// OffsetTail: addi  voff, vreg3, 188                OffsetTail: lui  voff, 128
//                    \                                        /
//                     \                                      /
//                      \                                    /
//                       \                                  /
//                         TailAdd: add  vreg4, vreg2, voff
static bool foldLargeOffset(MachineInstr &Hi, MachineInstr &Lo,
                            MachineInstr &TailAdd, Register GAReg,
                            MachineRegisterInfo *MRI,
                            const RVTrimSubtarget *ST) {
  assert((TailAdd.getOpcode() == RVTrim::ADD) && "Expected ADD instruction!");
  Register Rs = TailAdd.getOperand(1).getReg();
  Register Rt = TailAdd.getOperand(2).getReg();
  Register Reg = Rs == GAReg ? Rt : Rs;

  // Can't fold if the register has more than one use.
  if (!Reg.isVirtual() || !MRI->hasOneNonDBGUse(Reg))
    return false;
  // This can point to an ADDI(W) or a LUI:
  MachineInstr &OffsetTail = *MRI->getVRegDef(Reg);
  auto OffsetTailOpc = OffsetTail.getOpcode();
  if (OffsetTailOpc == RVTrim::ADDI || OffsetTailOpc == RVTrim::ADDIW) {
    // The offset value has non zero bits in both %hi and %lo parts.
    // Detect an ADDI that feeds from a LUI instruction.
    MachineOperand &AddiImmOp = OffsetTail.getOperand(2);
    if (AddiImmOp.getTargetFlags() != RVTrimII::MO_None)
      return false;
    // Make sure this isn't something weird like a frame index.
    if (!OffsetTail.getOperand(1).isReg())
      return false;
    Register AddiReg = OffsetTail.getOperand(1).getReg();
    int64_t OffLo = AddiImmOp.getImm();

    // Handle rs1 of ADDI is X0.
    if (AddiReg == RVTrim::X0) {
      LLVM_DEBUG(dbgs() << "  Offset Instrs: " << OffsetTail);
      if (!foldOffset(Hi, Lo, TailAdd, OffLo, MRI))
        return false;
      OffsetTail.eraseFromParent();
      return true;
    }

    MachineInstr &OffsetLui = *MRI->getVRegDef(AddiReg);
    MachineOperand &LuiImmOp = OffsetLui.getOperand(1);
    if (OffsetLui.getOpcode() != RVTrim::LUI ||
        LuiImmOp.getTargetFlags() != RVTrimII::MO_None ||
        !MRI->hasOneNonDBGUse(OffsetLui.getOperand(0).getReg()))
      return false;
    int64_t Offset = SignExtend64<32>(LuiImmOp.getImm() << 12);
    Offset += OffLo;
    // RV32 ignores the upper 32 bits. ADDIW sign extends the result.
    if (!ST->is64Bit() || OffsetTailOpc == RVTrim::ADDIW)
      Offset = SignExtend64<32>(Offset);
    // We can only fold simm32 offsets.
    if (!isInt<32>(Offset))
      return false;
    LLVM_DEBUG(dbgs() << "  Offset Instrs: " << OffsetTail
                      << "                 " << OffsetLui);
    if (!foldOffset(Hi, Lo, TailAdd, Offset, MRI))
      return false;
    OffsetTail.eraseFromParent();
    OffsetLui.eraseFromParent();
    return true;
  } else if (OffsetTailOpc == RVTrim::LUI) {
    // The offset value has all zero bits in the lower 12 bits. Only LUI
    // exists.
    LLVM_DEBUG(dbgs() << "  Offset Instr: " << OffsetTail);
    int64_t Offset = SignExtend64<32>(OffsetTail.getOperand(1).getImm() << 12);
    if (!foldOffset(Hi, Lo, TailAdd, Offset, MRI))
      return false;
    OffsetTail.eraseFromParent();
    return true;
  }
  return false;
}

// Detect patterns for offsets that are passed into a SHXADD instruction.
// The offset has 1, 2, or 3 trailing zeros and fits in simm13, simm14, simm15.
// The constant is created with addi voff, x0, C, and shXadd is used to
// fill insert the trailing zeros and do the addition.
// If the pattern is found, updates the offset in Hi and Lo instructions
// and deletes TailShXAdd and the instructions that produced the offset.
//
// Hi:         lui     vreg1, %hi(s)
// Lo:         addi    vreg2, vreg1, %lo(s)
// OffsetTail: addi    voff, x0, C
// TailAdd:    shXadd  vreg4, voff, vreg2
static bool foldShiftedOffset(MachineInstr &Hi, MachineInstr &Lo,
                              MachineInstr &TailShXAdd, Register GAReg,
                              MachineRegisterInfo *MRI) {
  assert((TailShXAdd.getOpcode() == RVTrim::SH1ADD ||
          TailShXAdd.getOpcode() == RVTrim::SH2ADD ||
          TailShXAdd.getOpcode() == RVTrim::SH3ADD) &&
         "Expected SHXADD instruction!");

  if (GAReg != TailShXAdd.getOperand(2).getReg())
    return false;

  // The first source is the shifted operand.
  Register Rs1 = TailShXAdd.getOperand(1).getReg();

  // Can't fold if the register has more than one use.
  if (!Rs1.isVirtual() || !MRI->hasOneNonDBGUse(Rs1))
    return false;
  // This can point to an ADDI X0, C.
  MachineInstr &OffsetTail = *MRI->getVRegDef(Rs1);
  if (OffsetTail.getOpcode() != RVTrim::ADDI)
    return false;
  if (!OffsetTail.getOperand(1).isReg() ||
      OffsetTail.getOperand(1).getReg() != RVTrim::X0 ||
      !OffsetTail.getOperand(2).isImm())
    return false;

  int64_t Offset = OffsetTail.getOperand(2).getImm();
  assert(isInt<12>(Offset) && "Unexpected offset");

  unsigned ShAmt;
  switch (TailShXAdd.getOpcode()) {
  default: llvm_unreachable("Unexpected opcode");
  case RVTrim::SH1ADD: ShAmt = 1; break;
  case RVTrim::SH2ADD: ShAmt = 2; break;
  case RVTrim::SH3ADD: ShAmt = 3; break;
  }

  Offset = (uint64_t)Offset << ShAmt;

  LLVM_DEBUG(dbgs() << "  Offset Instr: " << OffsetTail);
  if (!foldOffset(Hi, Lo, TailShXAdd, Offset, MRI))
    return false;
  OffsetTail.eraseFromParent();
  return true;
}

static bool detectAndFoldOffset(MachineInstr &Hi, MachineInstr &Lo,
                                MachineRegisterInfo *MRI,
                                const RVTrimSubtarget *ST) {
  Register DestReg = Lo.getOperand(0).getReg();

  // Look for arithmetic instructions we can get an offset from.
  // We might be able to remove the arithmetic instructions by folding the
  // offset into the LUI+ADDI.
  if (!MRI->hasOneNonDBGUse(DestReg))
    return false;

  // Lo has only one use.
  MachineInstr &Tail = *MRI->use_instr_nodbg_begin(DestReg);
  switch (Tail.getOpcode()) {
  default:
    LLVM_DEBUG(dbgs() << "Don't know how to get offset from this instr: "
                      << Tail);
    break;
  case RVTrim::ADDI:
  case RVTrim::QC_E_ADDI:
  case RVTrim::QC_E_ADDAI: {
    // Offset is simply an immediate operand.
    int64_t Offset = Tail.getOperand(2).getImm();
    if (Tail.getOpcode() == RVTrim::ADDI) {
      // We might have two ADDIs in a row.
      Register TailDestReg = Tail.getOperand(0).getReg();
      if (MRI->hasOneNonDBGUse(TailDestReg)) {
        MachineInstr &TailTail = *MRI->use_instr_nodbg_begin(TailDestReg);
        if (TailTail.getOpcode() == RVTrim::ADDI) {
          Offset += TailTail.getOperand(2).getImm();
          LLVM_DEBUG(dbgs() << "  Offset Instrs: " << Tail << TailTail);
          if (!foldOffset(Hi, Lo, TailTail, Offset, MRI))
            return false;
          Tail.eraseFromParent();
          return true;
        }
      }
    }

    LLVM_DEBUG(dbgs() << "  Offset Instr: " << Tail);
    return foldOffset(Hi, Lo, Tail, Offset, MRI);
  }
  case RVTrim::ADD:
    // The offset is too large to fit in the immediate field of ADDI.
    // This can be in two forms:
    // 1) LUI hi_Offset followed by:
    //    ADDI lo_offset
    //    This happens in case the offset has non zero bits in
    //    both hi 20 and lo 12 bits.
    // 2) LUI (offset20)
    //    This happens in case the lower 12 bits of the offset are zeros.
    return foldLargeOffset(Hi, Lo, Tail, DestReg, MRI, ST);
  case RVTrim::SH1ADD:
  case RVTrim::SH2ADD:
  case RVTrim::SH3ADD:
    // The offset is too large to fit in the immediate field of ADDI.
    // It may be encoded as (SH2ADD (ADDI X0, C), DestReg) or
    // (SH3ADD (ADDI X0, C), DestReg).
    return foldShiftedOffset(Hi, Lo, Tail, DestReg, MRI);
  }

  return false;
}

static void overwriteMachineOperandInPlace(MachineOperand &MO,
                                           const MachineOperand &ImmOp) {
  switch (ImmOp.getType()) {
  case MachineOperand::MO_ConstantPoolIndex:
    MO.ChangeToCPI(ImmOp.getIndex(), ImmOp.getOffset(), ImmOp.getTargetFlags());
    break;
  case MachineOperand::MO_GlobalAddress:
    MO.ChangeToGA(ImmOp.getGlobal(), ImmOp.getOffset(), ImmOp.getTargetFlags());
    break;
  case MachineOperand::MO_MCSymbol:
    MO.ChangeToMCSymbol(ImmOp.getMCSymbol(), ImmOp.getTargetFlags());
    MO.setOffset(ImmOp.getOffset());
    break;
  case MachineOperand::MO_BlockAddress:
    MO.ChangeToBA(ImmOp.getBlockAddress(), ImmOp.getOffset(),
                  ImmOp.getTargetFlags());
    break;
  default:
    report_fatal_error("unsupported machine operand type");
    break;
  }
}

static bool foldIntoMemoryOps(MachineInstr &Hi, MachineInstr &Lo,
                              MachineRegisterInfo *MRI,
                              const RVTrimSubtarget *ST) {
  Register DestReg = Lo.getOperand(0).getReg();

  // If all the uses are memory ops with the same offset, we can transform:
  //
  // 1. (medlow pattern):
  //   a. Hi:   lui vreg1, %hi(foo)          --->  lui vreg1, %hi(foo+8)
  //      Lo:   addi vreg2, vreg1, %lo(foo)  --->  lw vreg3, lo(foo+8)(vreg1)
  //      Tail: lw vreg3, 8(vreg2)
  //
  //   b. Hi:   qc.e.li vreg1, foo           ---> qc.e.li vreg1, foo+8
  //      Tail: lw vreg2, 8(vreg1)           ---> lw vreg2, 0(vreg1)
  //
  // 2. (medany pattern):
  // Hi: 1:auipc vreg1, %pcrel_hi(s)         ---> auipc vreg1, %pcrel_hi(foo+8)
  // Lo:   addi  vreg2, vreg1, %pcrel_lo(1b) ---> lw vreg3, %pcrel_lo(1b)(vreg1)
  // Tail: lw vreg3, 8(vreg2)

  std::optional<int64_t> CommonOffset;
  DenseMap<const MachineInstr *, SmallVector<unsigned>>
      InlineAsmMemoryOpIndexesMap;
  for (const MachineInstr &UseMI : MRI->use_nodbg_instructions(DestReg)) {
    switch (UseMI.getOpcode()) {
    default:
      LLVM_DEBUG(dbgs() << "Not a load or store instruction: " << UseMI);
      return false;
    case RVTrim::LB:
    case RVTrim::LH:
    case RVTrim::LH_INX:
    case RVTrim::LW:
    case RVTrim::LW_INX:
    case RVTrim::LBU:
    case RVTrim::LHU:
    case RVTrim::LWU:
    case RVTrim::LD:
    case RVTrim::LD_RV32:
    case RVTrim::FLH:
    case RVTrim::FLW:
    case RVTrim::FLD:
    case RVTrim::SB:
    case RVTrim::SH:
    case RVTrim::SH_INX:
    case RVTrim::SW:
    case RVTrim::SW_INX:
    case RVTrim::SD:
    case RVTrim::SD_RV32:
    case RVTrim::FSH:
    case RVTrim::FSW:
    case RVTrim::FSD: {
      if (UseMI.getOperand(1).isFI())
        return false;
      // Register defined by Lo should not be the value register.
      if (DestReg == UseMI.getOperand(0).getReg())
        return false;
      assert(DestReg == UseMI.getOperand(1).getReg() &&
             "Expected base address use");
      // All load/store instructions must use the same offset.
      int64_t Offset = UseMI.getOperand(2).getImm();
      if (CommonOffset && Offset != CommonOffset)
        return false;
      CommonOffset = Offset;
      break;
    }
    case RVTrim::PseudoCCLD:
    case RVTrim::PseudoCCLW:
    case RVTrim::PseudoCCLWU:
    case RVTrim::PseudoCCLH:
    case RVTrim::PseudoCCLHU:
    case RVTrim::PseudoCCLB:
    case RVTrim::PseudoCCLBU: {
      // The SFB Pseudos are like their non-SFB counterparts but have more
      // operands.
      if (UseMI.getOperand(2).isFI())
        return false;
      // Register defined by Lo should not be the (tied) false value, or a
      // register used in the branch predicate.
      if (DestReg == UseMI.getOperand(1).getReg() ||
          DestReg == UseMI.getOperand(5).getReg())
        return false;
      if (UseMI.getOperand(6).isReg() &&
          DestReg == UseMI.getOperand(6).getReg())
        return false;
      assert(DestReg == UseMI.getOperand(2).getReg() &&
             "Expected base address use");
      // All load/store instructions must use the same offset.
      int64_t Offset = UseMI.getOperand(3).getImm();
      if (CommonOffset && Offset != CommonOffset)
        return false;
      CommonOffset = Offset;
      break;
    }
    case RVTrim::INLINEASM:
    case RVTrim::INLINEASM_BR: {
      SmallVector<unsigned> InlineAsmMemoryOpIndexes;
      unsigned NumOps = 0;
      for (unsigned I = InlineAsm::MIOp_FirstOperand;
           I < UseMI.getNumOperands(); I += 1 + NumOps) {
        const MachineOperand &FlagsMO = UseMI.getOperand(I);
        // Should be an imm.
        if (!FlagsMO.isImm())
          continue;

        const InlineAsm::Flag Flags(FlagsMO.getImm());
        NumOps = Flags.getNumOperandRegisters();

        // Memory constraints have two operands.
        if (NumOps != 2 || !Flags.isMemKind()) {
          // If the register is used by something other than a memory
          // constraint, we should not fold.
          for (unsigned J = 0; J < NumOps; ++J) {
            const MachineOperand &MO = UseMI.getOperand(I + 1 + J);
            if (MO.isReg() && MO.getReg() == DestReg)
              return false;
          }
          continue;
        }

        // We can't do this for constraint A because AMO instructions don't have
        // an immediate offset field.
        if (Flags.getMemoryConstraintID() == InlineAsm::ConstraintCode::A)
          return false;

        const MachineOperand &AddrMO = UseMI.getOperand(I + 1);
        if (!AddrMO.isReg() || AddrMO.getReg() != DestReg)
          continue;

        const MachineOperand &OffsetMO = UseMI.getOperand(I + 2);
        if (!OffsetMO.isImm())
          continue;

        // All inline asm memory operands must use the same offset.
        int64_t Offset = OffsetMO.getImm();
        if (CommonOffset && Offset != CommonOffset)
          return false;
        CommonOffset = Offset;
        InlineAsmMemoryOpIndexes.push_back(I + 1);
      }
      InlineAsmMemoryOpIndexesMap.insert(
          std::make_pair(&UseMI, InlineAsmMemoryOpIndexes));
      break;
    }
    }
  }

  if (!CommonOffset)
    return false;

  // We found a common offset.
  // Update the offsets in global address lowering.
  // We may have already folded some arithmetic so we need to add to any
  // existing offset.
  int64_t NewOffset = Hi.getOperand(1).getOffset() + *CommonOffset;
  // RV32 ignores the upper 32 bits.
  if (!ST->is64Bit())
    NewOffset = SignExtend64<32>(NewOffset);
  // We can only fold simm32 offsets.
  if (!isInt<32>(NewOffset))
    return false;

  // Folding changes the value represented by DestReg, so its debug users can
  // no longer describe the same address.
  MRI->markUsesInDebugValueAsUndef(DestReg);

  Hi.getOperand(1).setOffset(NewOffset);
  MachineOperand &ImmOp =
      Hi.getOpcode() == RVTrim::QC_E_LI ? Lo.getOperand(1) : Lo.getOperand(2);
  auto HiOpc = Hi.getOpcode();
  // Expand PseudoMovAddr into LUI
  if (HiOpc == RVTrim::PseudoMovAddr) {
    auto *TII = ST->getInstrInfo();
    Hi.setDesc(TII->get(RVTrim::LUI));
    Hi.removeOperand(2);
  }

  if (HiOpc != RVTrim::AUIPC)
    ImmOp.setOffset(NewOffset);

  // Update the immediate in the load/store instructions to add the offset.
  for (MachineInstr &UseMI :
       llvm::make_early_inc_range(MRI->use_nodbg_instructions(DestReg))) {
    if (UseMI.getOpcode() == RVTrim::INLINEASM ||
        UseMI.getOpcode() == RVTrim::INLINEASM_BR) {
      auto &InlineAsmMemoryOpIndexes = InlineAsmMemoryOpIndexesMap[&UseMI];
      for (unsigned I : InlineAsmMemoryOpIndexes) {
        MachineOperand &MO = UseMI.getOperand(I + 1);
        overwriteMachineOperandInPlace(MO, ImmOp);
      }
    } else {
      unsigned ImmIdx;
      switch (UseMI.getOpcode()) {
      case RVTrim::INLINEASM:
      case RVTrim::INLINEASM_BR:
        llvm_unreachable("Should have been dealt with before this else");
      case RVTrim::PseudoCCLD:
      case RVTrim::PseudoCCLW:
      case RVTrim::PseudoCCLWU:
      case RVTrim::PseudoCCLH:
      case RVTrim::PseudoCCLHU:
      case RVTrim::PseudoCCLB:
      case RVTrim::PseudoCCLBU:
        ImmIdx = 3;
        break;
      case RVTrim::LB:
      case RVTrim::LH:
      case RVTrim::LH_INX:
      case RVTrim::LW:
      case RVTrim::LW_INX:
      case RVTrim::LBU:
      case RVTrim::LHU:
      case RVTrim::LWU:
      case RVTrim::LD:
      case RVTrim::LD_RV32:
      case RVTrim::FLH:
      case RVTrim::FLW:
      case RVTrim::FLD:
      case RVTrim::SB:
      case RVTrim::SH:
      case RVTrim::SH_INX:
      case RVTrim::SW:
      case RVTrim::SW_INX:
      case RVTrim::SD:
      case RVTrim::SD_RV32:
      case RVTrim::FSH:
      case RVTrim::FSW:
      case RVTrim::FSD:
        ImmIdx = 2;
        break;
      default:
        llvm_unreachable("Unknown Instruction");
      }

      MachineOperand &MO = UseMI.getOperand(ImmIdx);
      if (Hi.getOpcode() == RVTrim::QC_E_LI) {
        MO.ChangeToImmediate(0);
      } else {
        overwriteMachineOperandInPlace(MO, ImmOp);
      }
    }
  }

  // Prevent Lo (originally PseudoMovAddr, which is also pointed by Hi) from
  // being erased
  if (&Lo == &Hi)
    return true;

  MRI->replaceRegWith(Lo.getOperand(0).getReg(), Hi.getOperand(0).getReg());
  Lo.eraseFromParent();
  return true;
}

// Try to fold sequences of the form:
//   Hi/lo:    qc.e.li vreg1, s           -> qc.e.li vreg1, s+imm
//   TailAdd:  shxadd vreg2, vreg3, vreg1 -> deleted
//   Tail:     lx vreg4, imm(vreg2)       -> qc.lrx vreg4, vreg1, vreg3, (1-7)
static bool foldShxaddIntoScaledMemory(MachineInstr &Hi, MachineInstr &Lo,
                                       MachineRegisterInfo *MRI,
                                       const RVTrimSubtarget *ST) {
  if (!ST->hasVendorXqcisls() || ST->is64Bit())
    return false;

  if (Hi.getOpcode() != RVTrim::QC_E_LI)
    return false;

  Register BaseReg = Hi.getOperand(0).getReg();
  if (!BaseReg.isVirtual() || !MRI->hasOneUse(BaseReg))
    return false;

  MachineInstr &ShxAdd = *MRI->use_instr_begin(BaseReg);
  unsigned ShxOpc = ShxAdd.getOpcode();
  unsigned ShAmt = 0;
  switch (ShxOpc) {
  default:
    return false;
  case RVTrim::SH1ADD:
    ShAmt = 1;
    break;
  case RVTrim::SH2ADD:
    ShAmt = 2;
    break;
  case RVTrim::SH3ADD:
    ShAmt = 3;
    break;
  case RVTrim::QC_SHLADD:
    uint8_t ShlImm = ShxAdd.getOperand(3).getImm();
    if (ShlImm > 7)
      return false;
    ShAmt = ShlImm;
    break;
  }

  // shxadd Rd, Rs1, Rs2
  Register ScaledReg = ShxAdd.getOperand(0).getReg();
  Register IndexReg = ShxAdd.getOperand(1).getReg();

  if (!IndexReg.isVirtual())
    return false;

  if (ShxAdd.getOperand(2).getReg() != BaseReg)
    return false;

  if (!ScaledReg.isVirtual() || !MRI->hasOneUse(ScaledReg))
    return false;

  MachineInstr &TailMem = *MRI->use_instr_begin(ScaledReg);
  unsigned Opc = TailMem.getOpcode();
  unsigned NewOpc = 0;

  switch (Opc) {
  case RVTrim::LB:
    NewOpc = RVTrim::QC_LRB;
    break;
  case RVTrim::LBU:
    NewOpc = RVTrim::QC_LRBU;
    break;
  case RVTrim::LH:
    NewOpc = RVTrim::QC_LRH;
    break;
  case RVTrim::LHU:
    NewOpc = RVTrim::QC_LRHU;
    break;
  case RVTrim::LW:
    NewOpc = RVTrim::QC_LRW;
    break;
  case RVTrim::SB:
    NewOpc = RVTrim::QC_SRB;
    break;
  case RVTrim::SH:
    NewOpc = RVTrim::QC_SRH;
    break;
  case RVTrim::SW:
    NewOpc = RVTrim::QC_SRW;
    break;
  default:
    return false;
  }

  if (!TailMem.getOperand(1).isReg() ||
      TailMem.getOperand(1).getReg() != ScaledReg)
    return false;
  if (!TailMem.getOperand(2).isImm())
    return false;
  int64_t Imm = TailMem.getOperand(2).getImm();

  // Update QC_E_LI offset.
  int64_t NewOffset = SignExtend64<32>(Hi.getOperand(1).getOffset() + Imm);

  Hi.getOperand(1).setOffset(NewOffset);

  // Build scaled load/store.
  auto *TII = ST->getInstrInfo();
  auto *MBB = TailMem.getParent();

  // Ensure index register satisfies GPRNoX0 class required by QC_LR*/QC_SR*.
  MRI->constrainRegClass(IndexReg, &RVTrim::GPRNoX0RegClass);

  BuildMI(*MBB, TailMem, TailMem.getDebugLoc(), TII->get(NewOpc))
      .add(TailMem.getOperand(0))
      .addReg(BaseReg, getKillRegState(ShxAdd.getOperand(2).isKill()))
      .addReg(IndexReg, getKillRegState(ShxAdd.getOperand(1).isKill()))
      .addImm(ShAmt)
      .cloneMemRefs(TailMem);

  TailMem.eraseFromParent();
  ShxAdd.eraseFromParent();
  return true;
}

static bool runMergeBaseOffset(MachineFunction &Fn) {
  const RVTrimSubtarget *ST = &Fn.getSubtarget<RVTrimSubtarget>();

  bool MadeChange = false;
  MachineRegisterInfo *MRI = &Fn.getRegInfo();
  for (MachineBasicBlock &MBB : Fn) {
    LLVM_DEBUG(dbgs() << "MBB: " << MBB.getName() << "\n");
    for (MachineInstr &Hi : MBB) {
      MachineInstr *Lo = nullptr;
      if (!detectFoldable(Hi, Lo, MRI))
        continue;
      MadeChange |= detectAndFoldOffset(Hi, *Lo, MRI, ST);
      MadeChange |= foldIntoMemoryOps(Hi, *Lo, MRI, ST);
      MadeChange |= foldShxaddIntoScaledMemory(Hi, *Lo, MRI, ST);
    }
  }

  return MadeChange;
}

/// Returns an instance of the Merge Base Offset Optimization pass.
FunctionPass *llvm::createRVTrimMergeBaseOffsetLegacyPass() {
  return new RVTrimMergeBaseOffsetLegacy();
}

bool RVTrimMergeBaseOffsetLegacy::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  return runMergeBaseOffset(MF);
}

PreservedAnalyses
RVTrimMergeBaseOffsetPass::run(MachineFunction &MF,
                              MachineFunctionAnalysisManager &MFAM) {
  return runMergeBaseOffset(MF) ? getMachineFunctionPassPreservedAnalyses()
                                      .preserveSet<CFGAnalyses>()
                                : PreservedAnalyses::all();
}
