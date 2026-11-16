//===-- RVMinMCInstLower.cpp - Lower MachineInstr to MCInst --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinBaseInfo.h"
#include "RVMinMCInstLower.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

void llvm::LowerRVMinMachineInstrToMCInst(const MachineInstr *MI,
                                          MCInst &OutMI, MCContext &Ctx,
                                          const TargetMachine &TM,
                                          AsmPrinter &AP) {
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
    case MachineOperand::MO_GlobalAddress: {
      // A global address arrives in two halves: %hi20 for `lui`, %lo12 for
      // `addi rd, rd, ...` or a store's S-type immediate.
      MCSymbol *Sym = TM.getSymbol(MO.getGlobal());
      const MCExpr *Expr = MCSymbolRefExpr::create(Sym, Ctx);
      if (MO.getOffset())
        Expr = MCBinaryExpr::createAdd(
            Expr, MCConstantExpr::create(MO.getOffset(), Ctx), Ctx);

      switch (MO.getTargetFlags()) {
      case RVMinII::MO_HI20:
        Expr = MCSpecifierExpr::create(Expr, rvmin::S_HI20, Ctx);
        break;
      case RVMinII::MO_LO12_I:
        Expr = MCSpecifierExpr::create(Expr, rvmin::S_LO12_I, Ctx);
        break;
      case RVMinII::MO_LO12_S:
        Expr = MCSpecifierExpr::create(Expr, rvmin::S_LO12_S, Ctx);
        break;
      default:
        llvm_unreachable("RVMin: global address without a %hi/%lo flag");
      }
      OutMI.addOperand(MCOperand::createExpr(Expr));
      break;
    }
    case MachineOperand::MO_JumpTableIndex: {
      // Chapter 18: a jump table lives in .rodata under a linker-private
      // symbol such as .LJTI0.  AsmPrinter owns that symbol, so ask it for the
      // name and then print the half selected by the target flag, exactly like
      // a global address.
      const MCExpr *Expr =
          MCSymbolRefExpr::create(AP.GetJTISymbol(MO.getIndex()), Ctx);
      switch (MO.getTargetFlags()) {
      case RVMinII::MO_HI20:
        Expr = MCSpecifierExpr::create(Expr, rvmin::S_HI20, Ctx);
        break;
      case RVMinII::MO_LO12_I:
        Expr = MCSpecifierExpr::create(Expr, rvmin::S_LO12_I, Ctx);
        break;
      default:
        llvm_unreachable("RVMin: jump table without a %hi/%lo flag");
      }
      OutMI.addOperand(MCOperand::createExpr(Expr));
      break;
    }
    case MachineOperand::MO_MachineBasicBlock:
      // Branch targets become symbol references; the MCCodeEmitter turns them
      // into B/J fixups and the assembler backend patches the offset.
      OutMI.addOperand(MCOperand::createExpr(
          MCSymbolRefExpr::create(MO.getMBB()->getSymbol(), Ctx)));
      break;
    default:
      llvm_unreachable("RVMin: unsupported machine operand in MCInst lowering");
    }
  }
}
