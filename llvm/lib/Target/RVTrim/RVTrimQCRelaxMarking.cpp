//===-- RVTrimQCRelaxMarking.cpp - Mark Instructions for QC Relaxations ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass adds access tags to some instructions which are used by the
// assembler to emit marker relocations, which enable some code-size relaxations
// for Xqcilo/Xqcili.
//
// The pass is looking for the following sequences:
//
//   $dst1 = QC_E_LI sym
//   $dst2 = Load killed $dst1, 0
//
//   $dst1 = QC_E_LI sym
//   Store $dst2, killed $dst1, 0
//
// In either case, the Load/Store is modified to become a
// PseudoQCAccess<Load/Store>, with an additional operand that represents the
// accessed symbolic address, which will become the contents of a
// `R_RISCV_QC_ACCESS_*` relocation on the emitted instruction.
//
// FIXME: The intention is this pass does not change the size of any
// instructions, but right now it has to do instruction compression as the
// CompressPat infrastructure cannot handle compressing the `%qc.access(...)`
// operand. Symbolic operands are not usually compressible, but this one is as
// we have relocations for both 32-bit and 16-bit instructions (and the
// relocation does not care about the fields of the instruction).

#include "RVTrim.h"
#include "RVTrimSubtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/IR/Analysis.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define DEBUG_TYPE "rvtrim-qc-relax-marking"
#define RVTrim_QC_RELAX_MARKING_NAME "RISC-V QC Relaxation Marking"

STATISTIC(NumMarked, "Number of Loads/Stores Marked");

namespace {

struct RVTrimQCRelaxMarkingLegacy : public MachineFunctionPass {
  static char ID;

  bool runOnMachineFunction(MachineFunction &) override;

  RVTrimQCRelaxMarkingLegacy() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return RVTrim_QC_RELAX_MARKING_NAME; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // end namespace

char RVTrimQCRelaxMarkingLegacy::ID = 0;

INITIALIZE_PASS(RVTrimQCRelaxMarkingLegacy, DEBUG_TYPE,
                RVTrim_QC_RELAX_MARKING_NAME, false, false)

/// Returns an instance of the Make Compressible Optimization pass.
FunctionPass *llvm::createRVTrimQCRelaxMarkingLegacyPass() {
  return new RVTrimQCRelaxMarkingLegacy();
}

static bool isUImm7LSB000(const MachineOperand &MO) {
  return MO.isImm() && isShiftedUInt<4, 3>(MO.getImm());
}

static bool isUImm2LSB0(const MachineOperand &MO) {
  return MO.isImm() && isShiftedUInt<1, 1>(MO.getImm());
}

static bool isUImm2(const MachineOperand &MO) {
  return MO.isImm() && isUInt<2>(MO.getImm());
}

static bool isGPRC(const MachineOperand &MO) {
  return RVTrim::GPRCRegClass.contains(MO.getReg());
}

static unsigned getQCMarkedOpcode(const MachineInstr &MI,
                                  const RVTrimSubtarget &STI) {
  switch (MI.getOpcode()) {
  case RVTrim::LB:
    // No c.lb
    return RVTrim::PseudoQCAccessLB;
  case RVTrim::LBU:
    if (STI.hasStdExtZcb() && isGPRC(MI.getOperand(0)) &&
        isGPRC(MI.getOperand(1)) && isUImm2(MI.getOperand(2)))
      return RVTrim::PseudoQCAccessC_LBU;
    return RVTrim::PseudoQCAccessLBU;
  case RVTrim::LH:
    if (STI.hasStdExtZcb() && isGPRC(MI.getOperand(0)) &&
        isGPRC(MI.getOperand(1)) && isUImm2LSB0(MI.getOperand(2)))
      return RVTrim::PseudoQCAccessC_LH;
    return RVTrim::PseudoQCAccessLH;
  case RVTrim::LHU:
    if (STI.hasStdExtZcb() && isGPRC(MI.getOperand(0)) &&
        isGPRC(MI.getOperand(1)) && isUImm2LSB0(MI.getOperand(2)))
      return RVTrim::PseudoQCAccessC_LHU;
    return RVTrim::PseudoQCAccessLHU;
  case RVTrim::LW:
    if (STI.hasStdExtZca() && isGPRC(MI.getOperand(0)) &&
        isGPRC(MI.getOperand(1)) && isUImm7LSB000(MI.getOperand(2)))
      return RVTrim::PseudoQCAccessC_LW;
    return RVTrim::PseudoQCAccessLW;
  case RVTrim::SB:
    if (STI.hasStdExtZcb() && isGPRC(MI.getOperand(0)) &&
        isGPRC(MI.getOperand(1)) && isUImm2(MI.getOperand(2)))
      return RVTrim::PseudoQCAccessC_SB;
    return RVTrim::PseudoQCAccessSB;
  case RVTrim::SH:
    if (STI.hasStdExtZcb() && isGPRC(MI.getOperand(0)) &&
        isGPRC(MI.getOperand(1)) && isUImm2LSB0(MI.getOperand(2)))
      return RVTrim::PseudoQCAccessC_SH;
    return RVTrim::PseudoQCAccessSH;
  case RVTrim::SW:
    if (STI.hasStdExtZca() && isGPRC(MI.getOperand(0)) &&
        isGPRC(MI.getOperand(1)) && isUImm7LSB000(MI.getOperand(2)))
      return RVTrim::PseudoQCAccessC_SW;
    return RVTrim::PseudoQCAccessSW;
  default:
    reportFatalInternalError(
        "Unhandled Opcode: No Corresponding Marked Opcode");
  }
}

static bool runQCRelaxMarking(MachineFunction &MF) {
  // This is only relevant for QC.E.LI with a symbol, which we only use in the
  // small code model.
  if (MF.getTarget().getCodeModel() != CodeModel::Small)
    return false;

  auto &STI = MF.getSubtarget<RVTrimSubtarget>();
  // We need QC.E.LI instructions to perform this optimisation, which needs
  // 32-bit and Xqcili. The markers are only needed when linker relaxations are
  // enabled.
  if (STI.is64Bit() || !STI.hasVendorXqcili() || !STI.enableLinkerRelax())
    return false;

  const RVTrimInstrInfo *TII = STI.getInstrInfo();

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    for (auto MI = MBB.begin(), E = MBB.end(); MI != E; MI++) {
      auto NextMI = std::next(MI);
      if (NextMI == E)
        break;

      // Looking for QC.E.LI followed by a load or store
      if (MI->getOpcode() != RVTrim::QC_E_LI ||
          !(RVTrimInstrInfo::isBaseLoad(*NextMI) ||
            RVTrimInstrInfo::isBaseStore(*NextMI)))
        continue;

      LLVM_DEBUG(dbgs() << "Found QC_E_LI " << *MI);
      LLVM_DEBUG(dbgs() << "Followed by Load/Store " << *NextMI);

      if (MI->getOperand(0).getReg() != NextMI->getOperand(1).getReg())
        continue;
      if (!NextMI->getOperand(1).isKill())
        continue;

      // This is unsafe for stores where the access address is being stored.
      if (RVTrimInstrInfo::isBaseStore(*NextMI) &&
          MI->getOperand(0).getReg() == NextMI->getOperand(0).getReg())
        continue;

      MachineOperand &SymOp = MI->getOperand(1);
      if (!SymOp.isSymbol() && !SymOp.isGlobal() && !SymOp.isMCSymbol() &&
          !SymOp.isCPI())
        continue;

      unsigned NewOpc = getQCMarkedOpcode(*NextMI, STI);
      LLVM_DEBUG(dbgs() << "Load/Store " << TII->getName(NextMI->getOpcode())
                        << " will become " << TII->getName(NewOpc) << "\n");
      MachineInstrBuilder MIB =
          BuildMI(MBB, NextMI, NextMI->getDebugLoc(), TII->get(NewOpc))
              .add(NextMI->getOperand(0))
              .add(NextMI->getOperand(1))
              .add(NextMI->getOperand(2))
              .cloneMemRefs(*NextMI);

      if (SymOp.isSymbol()) {
        MIB.addExternalSymbol(SymOp.getSymbolName(), RVTrimII::MO_QC_ACCESS);
      } else if (SymOp.isGlobal()) {
        MIB.addGlobalAddress(SymOp.getGlobal(), SymOp.getOffset(),
                             RVTrimII::MO_QC_ACCESS);
      } else if (SymOp.isMCSymbol()) {
        MachineOperand MO = MachineOperand::CreateMCSymbol(
            SymOp.getMCSymbol(), RVTrimII::MO_QC_ACCESS);
        MO.setOffset(SymOp.getOffset());
        MIB.add(MO);
      } else if (SymOp.isCPI()) {
        MIB.addConstantPoolIndex(SymOp.getIndex(), SymOp.getOffset(),
                                 RVTrimII::MO_QC_ACCESS);
      } else {
        reportFatalInternalError("Unhandled SymOp Kind");
      }

      NextMI->removeFromParent();
      NumMarked++;
      Changed |= true;
    }
  }

  return Changed;
}

bool RVTrimQCRelaxMarkingLegacy::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  return runQCRelaxMarking(MF);
}

PreservedAnalyses
RVTrimQCRelaxMarkingPass::run(MachineFunction &MF,
                             MachineFunctionAnalysisManager &MFAM) {
  return runQCRelaxMarking(MF) ? getMachineFunctionPassPreservedAnalyses()
                                     .preserveSet<CFGAnalyses>()
                               : PreservedAnalyses::all();
}
