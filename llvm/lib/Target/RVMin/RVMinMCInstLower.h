//===-- RVMinMCInstLower.h - Lower MachineInstr to MCInst ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_RVMINMCINSTLOWER_H
#define LLVM_LIB_TARGET_RVMIN_RVMINMCINSTLOWER_H

namespace llvm {

class MCContext;
class MCInst;
class MachineInstr;

/// Copy a MachineInstr into an MCInst.  Chapter 2 handles registers and
/// immediates, chapter 5 basic-block operands (branch targets), chapter 6 adds
/// frame indices and chapter 7 adds global addresses / symbol references.
void LowerRVMinMachineInstrToMCInst(const MachineInstr *MI, MCInst &OutMI,
                                    MCContext &Ctx);

} // namespace llvm

#endif // LLVM_LIB_TARGET_RVMIN_RVMINMCINSTLOWER_H
