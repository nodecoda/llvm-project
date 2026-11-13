//===-- MiniRVMCInstLower.cpp - Lower MachineInstr to MCInst --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVMCInstLower.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

void MiniRVMCInstLower::Lower(const MachineInstr *MI, MCInst &OutMI) const {
  OutMI.setOpcode(MI->getOpcode());

  for (const MachineOperand &MO : MI->operands()) {
    switch (MO.getType()) {
    default:
      llvm_unreachable("MiniRV has no such operand");
    case MachineOperand::MO_Register:
      if (MO.isImplicit())
        continue;
      OutMI.addOperand(MCOperand::createReg(MO.getReg()));
      break;
    case MachineOperand::MO_Immediate:
      OutMI.addOperand(MCOperand::createImm(MO.getImm()));
      break;
    case MachineOperand::MO_MachineBasicBlock:
      OutMI.addOperand(MCOperand::createExpr(
          MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), Ctx)));
      break;
    case MachineOperand::MO_GlobalAddress:
      OutMI.addOperand(MCOperand::createExpr(
          MCSymbolRefExpr::create(Printer.getSymbol(MO.getGlobal()), Ctx)));
      break;
    case MachineOperand::MO_ExternalSymbol:
      OutMI.addOperand(MCOperand::createExpr(MCSymbolRefExpr::create(
          Printer.GetExternalSymbolSymbol(MO.getSymbolName()), Ctx)));
      break;
    case MachineOperand::MO_MCSymbol:
      OutMI.addOperand(MCOperand::createExpr(
          MCSymbolRefExpr::create(MO.getMCSymbol(), Ctx)));
      break;
    }
  }
}
