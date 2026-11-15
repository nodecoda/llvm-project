//===-- RVMinMCTargetDesc.h - RVMin target descriptions --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_MCTARGETDESC_RVMINMCTARGETDESC_H
#define LLVM_LIB_TARGET_RVMIN_MCTARGETDESC_RVMINMCTARGETDESC_H

#include "llvm/Support/DataTypes.h"

namespace llvm {
class Target;
} // namespace llvm

#define GET_REGINFO_ENUM
#include "RVMinGenRegisterInfo.inc"

#define GET_INSTRINFO_ENUM
#define GET_INSTRINFO_MC_HELPER_DECLS
#include "RVMinGenInstrInfo.inc"

#define GET_SUBTARGETINFO_ENUM
#include "RVMinGenSubtargetInfo.inc"

#endif // LLVM_LIB_TARGET_RVMIN_MCTARGETDESC_RVMINMCTARGETDESC_H
