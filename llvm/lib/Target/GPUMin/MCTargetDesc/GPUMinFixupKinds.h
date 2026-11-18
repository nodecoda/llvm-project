//===-- GPUMinFixupKinds.h - GPUMin specific fixup entries -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Fixups are the MC-layer patches whose value is only known after layout.
// Chapter 00 needs exactly one: a 32-bit absolute value.  Chapter 02 adds the
// PC-relative kinds used by GM32 branches and calls.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUMIN_MCTARGETDESC_GPUMINFIXUPKINDS_H
#define LLVM_LIB_TARGET_GPUMIN_MCTARGETDESC_GPUMINFIXUPKINDS_H

#include "llvm/MC/MCFixup.h"

namespace llvm::gpumin {

enum FixupKind {
  // A plain 32-bit absolute value, used for symbolic data operands.
  fixup_gpumin_32 = FirstTargetFixupKind,

  // Sentinel, must be last.
  fixup_gpumin_invalid,
  NumTargetFixupKinds = fixup_gpumin_invalid - FirstTargetFixupKind
};

} // namespace llvm::gpumin

#endif // LLVM_LIB_TARGET_GPUMIN_MCTARGETDESC_GPUMINFIXUPKINDS_H
