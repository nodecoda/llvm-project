//===-- RVMinInstPrinter.cpp - Convert RVMin MCInst to asm syntax ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Print an MCInst using standard RISC-V assembly syntax.  The per-instruction
// string comes from TableGen (the AsmString in RVMinInstrInfo.td); this file
// supplies the two pieces TableGen cannot know:
//
//   * register names: RVMin prints ABI names (zero/ra/sp/a0/...) by default,
//   * operands: RRVMin uses plain `imm` / `offset(reg)` syntax.
//
//===----------------------------------------------------------------------===//

#include "RVMinInstPrinter.h"
#include "RVMinMCTargetDesc.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#include "RVMinGenAsmWriter.inc"

void RVMinInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                                 StringRef Annot, const MCSubtargetInfo &STI,
                                 raw_ostream &O) {
  printInstruction(MI, Address, STI, O);
  printAnnotation(O, Annot);
}

void RVMinInstPrinter::printRegName(raw_ostream &O, MCRegister Reg) {
  O << getRegisterName(Reg, RVMin::ABIRegAltName);
}

void RVMinInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                    const MCSubtargetInfo &STI,
                                    raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);

  if (MO.isReg()) {
    O << getRegisterName(MO.getReg(), RVMin::ABIRegAltName);
    return;
  }

  if (MO.isImm()) {
    O << MO.getImm();
    return;
  }

  assert(MO.isExpr() && "unknown operand kind in printOperand");
  MAI.printExpr(O, *MO.getExpr());
}
