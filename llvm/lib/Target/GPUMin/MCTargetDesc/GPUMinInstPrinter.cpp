//===-- GPUMinInstPrinter.cpp - Convert GPUMin MCInst to asm syntax -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Print an MCInst using GM32 assembly syntax.  The per-instruction string comes
// from TableGen (the AsmString in GPUMinInstrInfo.td / later chapters); this
// file supplies the two pieces TableGen cannot know:
//
//   * register names: r0-r31, plus the symbolic aliases zero/sp/lr,
//   * operand syntax: plain `imm`, `sym` and `offset(base)` forms.
//
//===----------------------------------------------------------------------===//

#include "GPUMinInstPrinter.h"
#include "GPUMinMCTargetDesc.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

void GPUMinInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                                  StringRef Annot, const MCSubtargetInfo &STI,
                                  raw_ostream &O) {
  printInstruction(MI, Address, STI, O);
  printAnnotation(O, Annot);
}

void GPUMinInstPrinter::printRegName(raw_ostream &O, MCRegister Reg) {
  O << getRegisterName(Reg);
}

void GPUMinInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                     const MCSubtargetInfo &STI,
                                     raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);

  if (MO.isReg()) {
    O << getRegisterName(MO.getReg());
    return;
  }
  if (MO.isImm()) {
    O << MO.getImm();
    return;
  }

  assert(MO.isExpr() && "unknown operand kind in printOperand");
  MAI.printExpr(O, *MO.getExpr());
}

#include "GPUMinGenAsmWriter.inc"
