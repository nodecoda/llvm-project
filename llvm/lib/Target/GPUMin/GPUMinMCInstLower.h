//===-- GPUMinMCInstLower.h - Lower MachineInstr to MCInst -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUMIN_GPUMINMCINSTLOWER_H
#define LLVM_LIB_TARGET_GPUMIN_GPUMINMCINSTLOWER_H

namespace llvm {

class AsmPrinter;
class MCContext;
class MCInst;
class MachineInstr;
class TargetMachine;

/// Copy a MachineInstr into an MCInst.  Handles registers, immediates and
/// basic-block operands (branch targets).  Chapter 06 adds global addresses.
void LowerGPUMinMachineInstrToMCInst(const MachineInstr *MI, MCInst &OutMI,
                                     MCContext &Ctx, const TargetMachine &TM,
                                     AsmPrinter &AP);

} // namespace llvm

#endif // LLVM_LIB_TARGET_GPUMIN_GPUMINMCINSTLOWER_H
