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
class TargetMachine;

/// Copy a MachineInstr into an MCInst.  Handles registers, immediates,
/// basic-block operands (branch targets) and, since chapter 7, global
/// addresses with %hi/%lo relocation specifiers.
void LowerRVMinMachineInstrToMCInst(const MachineInstr *MI, MCInst &OutMI,
                                    MCContext &Ctx, const TargetMachine &TM);

} // namespace llvm

#endif // LLVM_LIB_TARGET_RVMIN_RVMINMCINSTLOWER_H
