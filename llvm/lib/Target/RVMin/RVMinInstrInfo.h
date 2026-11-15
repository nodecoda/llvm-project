//===-- RVMinInstrInfo.h - RVMin instruction information -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_RVMININSTRINFO_H
#define LLVM_LIB_TARGET_RVMIN_RVMININSTRINFO_H

#include "RVMinRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

#define GET_INSTRINFO_HEADER
#include "RVMinGenInstrInfo.inc"

namespace llvm {

class RVMinSubtarget;

class RVMinInstrInfo : public RVMinGenInstrInfo {
  const RVMinRegisterInfo RI;
  virtual void anchor();

public:
  explicit RVMinInstrInfo(const RVMinSubtarget &STI);

  const RVMinRegisterInfo &getRegisterInfo() const { return RI; }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_RVMIN_RVMININSTRINFO_H
