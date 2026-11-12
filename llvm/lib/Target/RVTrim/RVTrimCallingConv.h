//===-- RVTrimCallingConv.h - RISC-V Custom CC Routines ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the custom routines for the RISC-V Calling Convention.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/RVTrimBaseInfo.h"
#include "llvm/CodeGen/CallingConvLower.h"

namespace llvm {

/// This is used for assigining arguments to locations when making calls.
CCAssignFn CC_RVTrim;

/// This is used for assigning return values to locations when making calls.
CCAssignFn RetCC_RVTrim;

class RVTrimSubtarget;

namespace RVTrim {

ArrayRef<MCPhysReg> getArgGPRs(const RVTrimSubtarget &STI);
ArrayRef<MCPhysReg> getArgFPRs(const RVTrimSubtarget &STI);
ArrayRef<MCPhysReg> getArgVRs(const RVTrimSubtarget &STI);

} // end namespace RVTrim

} // end namespace llvm
