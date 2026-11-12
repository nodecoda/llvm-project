//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVTrim_MCTARGETDESC_RVTrimMCOPTIONS_H
#define LLVM_LIB_TARGET_RVTrim_MCTARGETDESC_RVTrimMCOPTIONS_H

#include "llvm/MC/MCTargetOptions.h"

#define OPTIONS_STRUCT_DECL
#include "RVTrimMCOptions.inc"

namespace llvm::RVTrim_MC {
inline const RVTrimMCOptions &getCLOpts(const MCTargetOptions &Options) {
  return cl::LibraryOptionsContext::get<RVTrimMCOptions>(Options.LibraryOptions);
}
} // namespace llvm::RVTrim_MC

#endif // LLVM_LIB_TARGET_RVTrim_MCTARGETDESC_RVTrimMCOPTIONS_H
