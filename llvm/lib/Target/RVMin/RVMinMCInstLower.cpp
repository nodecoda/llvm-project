//===-- RVMinMCInstLower.cpp - Lower MachineInstr to MCInst --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinMCInstLower.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

void llvm::LowerRVMinMachineInstrToMCInst(const MachineInstr *MI,
                                          MCInst &OutMI) {
  OutMI.setOpcode(MI->getOpcode());

  for (const MachineOperand &MO : MI->explicit_operands()) {
    switch (MO.getType()) {
    case MachineOperand::MO_Register:
      assert(!MO.getSubReg() && "RVMin does not use subregisters");
      OutMI.addOperand(MCOperand::createReg(MO.getReg()));
      break;
    case MachineOperand::MO_Immediate:
      OutMI.addOperand(MCOperand::createImm(MO.getImm()));
      break;
    default:
      llvm_unreachable("RVMin: unsupported machine operand in MCInst lowering");
    }
  }
}
