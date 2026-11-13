//===-- MiniRVInstPrinter.cpp - Convert MiniRV MCInst to asm syntax -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVInstPrinter.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"

using namespace llvm;

#define DEBUG_TYPE "minirv-asm-printer"

void MiniRVInstPrinter::printRegName(raw_ostream &O, MCRegister Reg) {
  O << getRegisterName(Reg);
}

void MiniRVInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                                  StringRef Annot, const MCSubtargetInfo &STI,
                                  raw_ostream &O) {
  printInstruction(MI, Address, O);
  printAnnotation(O, Annot);
}

void MiniRVInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                     raw_ostream &O) {
  const MCOperand &Op = MI->getOperand(OpNo);
  if (Op.isReg())
    O << getRegisterName(Op.getReg());
  else if (Op.isImm())
    O << Op.getImm();
  else {
    assert(Op.isExpr() && "unknown operand kind in printOperand");
    MAI.printExpr(O, *Op.getExpr());
  }
}

#include "MiniRVGenAsmWriter.inc"
