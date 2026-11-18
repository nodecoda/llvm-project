//===-- GPUMinInstrInfo.h - GPUMin instruction information -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUMIN_GPUMININSTRINFO_H
#define LLVM_LIB_TARGET_GPUMIN_GPUMININSTRINFO_H

#include "GPUMinRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

#define GET_INSTRINFO_HEADER
#include "GPUMinGenInstrInfo.inc"

namespace llvm {

class GPUMinSubtarget;

class GPUMinInstrInfo : public GPUMinGenInstrInfo {
  const GPUMinRegisterInfo RI;
  virtual void anchor();

public:
  explicit GPUMinInstrInfo(const GPUMinSubtarget &STI);

  const GPUMinRegisterInfo &getRegisterInfo() const { return RI; }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_GPUMIN_GPUMININSTRINFO_H
