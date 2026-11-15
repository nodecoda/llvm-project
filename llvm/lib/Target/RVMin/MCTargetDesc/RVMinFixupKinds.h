//===-- RVMinFixupKinds.h - RVMin specific fixup entries -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Fixups are the MC-layer patches that survive parsing but need a layout or a
// symbol value that is only known later (branch targets, global addresses,
// relocations).  Chapter 2 only needs the three encodings below; chapter 7
// adds the %hi/%lo pair used for globals.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_MCTARGETDESC_RVMINFIXUPKINDS_H
#define LLVM_LIB_TARGET_RVMIN_MCTARGETDESC_RVMINFIXUPKINDS_H

#include "llvm/MC/MCFixup.h"

namespace llvm::rvmin {

enum FixupKind {
  // A plain 32-bit absolute value, used as a placeholder for symbolic
  // immediates until chapter 7 introduces hi20/lo12 fixups.
  fixup_rvmin_32 = FirstTargetFixupKind,
  // 13-bit PC-relative byte offset of a B-type branch (bit 0 always zero).
  fixup_rvmin_branch,
  // 21-bit PC-relative byte offset of a J-type JAL (bit 0 always zero).
  fixup_rvmin_jal,
  // %hi20(sym): the rounded high 20 bits of an absolute address (U-type).
  fixup_rvmin_hi20,
  // %lo12(sym) in an I-type immediate (ADDI/LW/JALR).
  fixup_rvmin_lo12_i,
  // %lo12(sym) in an S-type immediate (SB/SH/SW).
  fixup_rvmin_lo12_s,

  // Sentinel, must be last.
  fixup_rvmin_invalid,
  NumTargetFixupKinds = fixup_rvmin_invalid - FirstTargetFixupKind
};

} // namespace llvm::rvmin

#endif // LLVM_LIB_TARGET_RVMIN_MCTARGETDESC_RVMINFIXUPKINDS_H
