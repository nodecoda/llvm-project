//===-- MachineInstCount.cpp - Count generic machine instructions --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetOpcodes.h"

using namespace llvm;

namespace {
enum InstCountStage {
  PreLegalizer,
  PreRegBankSelect,
  PreInstructionSelect,
  NumStages,
};

static Statistic TotalInsts[NumStages] = {
    {"mir-instcount-prelegalizer", "TotalInsts",
     "Number of generic machine instructions before legalization"},
    {"mir-instcount-preregbankselect", "TotalInsts",
     "Number of generic machine instructions before register-bank selection"},
    {"mir-instcount-preinstructionselect", "TotalInsts",
     "Number of generic machine instructions before instruction selection"},
};

#undef HANDLE_TARGET_OPCODE
#undef HANDLE_TARGET_OPCODE_MARKER
#define HANDLE_TARGET_OPCODE(OPCODE)                                           \
  {{"mir-instcount-prelegalizer", "Num" #OPCODE "Inst",                        \
    "Number of " #OPCODE " instructions before legalization"},                 \
   {"mir-instcount-preregbankselect", "Num" #OPCODE "Inst",                    \
    "Number of " #OPCODE " instructions before register-bank selection"},      \
   {"mir-instcount-preinstructionselect", "Num" #OPCODE "Inst",                \
    "Number of " #OPCODE " instructions before instruction selection"}},
#define HANDLE_TARGET_OPCODE_MARKER(IDENT, OPCODE)

static Statistic OpcodeCounts[][NumStages] = {
#include "llvm/Support/TargetOpcodes.def"
};

#undef HANDLE_TARGET_OPCODE
#undef HANDLE_TARGET_OPCODE_MARKER

static_assert(sizeof(OpcodeCounts) / sizeof(OpcodeCounts[0]) ==
              TargetOpcode::GENERIC_OP_END + 1);

class MachineInstCount : public MachineFunctionPass {
public:
  static char ID;

  MachineInstCount(InstCountStage Stage)
      : MachineFunctionPass(ID), Stage(Stage) {}

  StringRef getPassName() const override {
    switch (Stage) {
    case PreLegalizer:
      return "Pre-legalizer machine instruction count";
    case PreRegBankSelect:
      return "Pre-register-bank-select machine instruction count";
    case PreInstructionSelect:
      return "Pre-instruction-select machine instruction count";
    case NumStages:
      llvm_unreachable("invalid machine instruction count stage");
    }
    llvm_unreachable("invalid machine instruction count stage");
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    for (MachineBasicBlock &MBB : MF) {
      for (MachineInstr &MI : MBB) {
        unsigned Opcode = MI.getOpcode();
        if (!isPreISelGenericOpcode(Opcode))
          continue;
        ++OpcodeCounts[Opcode][Stage];
        ++TotalInsts[Stage];
      }
    }
    return false;
  }

private:
  InstCountStage Stage;
};
} // namespace

char MachineInstCount::ID = 0;

MachineFunctionPass *llvm::createPreLegalizerInstCountPass() {
  return new MachineInstCount(PreLegalizer);
}

MachineFunctionPass *llvm::createPreRegBankSelectInstCountPass() {
  return new MachineInstCount(PreRegBankSelect);
}

MachineFunctionPass *llvm::createPreInstructionSelectInstCountPass() {
  return new MachineInstCount(PreInstructionSelect);
}
