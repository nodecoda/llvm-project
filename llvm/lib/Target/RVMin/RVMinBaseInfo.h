//===-- RVMinBaseInfo.h - RVMin target flags and specifiers ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Two small enums shared by instruction selection, the asm printer and the
// MC layer:
//
//   * MachineOperand target flags: which half of a global address an operand
//     carries (the %hi20 or the %lo12 part),
//   * assembly specifiers: the %hi(...) / %lo(...) syntax used in .s files.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_RVMINBASEINFO_H
#define LLVM_LIB_TARGET_RVMIN_RVMINBASEINFO_H

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>

namespace llvm {
namespace rvmin {

enum Specifier : uint16_t {
  S_None = 0,
  S_HI20,
  // Assembler text `%lo(x)` does not say whether the value goes into an
  // I-type or an S-type immediate; the opcode decides.
  S_LO12,
  // The asm printer already knows the instruction shape.
  S_LO12_I,
  S_LO12_S,
};

inline StringRef getSpecifierName(Specifier S) {
  switch (S) {
  case S_HI20:
    return "hi";
  case S_LO12:
  case S_LO12_I:
  case S_LO12_S:
    return "lo";
  case S_None:
    return "";
  }
  return "";
}

inline std::optional<Specifier> parseSpecifierName(StringRef Name) {
  if (Name == "hi")
    return S_HI20;
  if (Name == "lo")
    return S_LO12;
  return std::nullopt;
}

} // end namespace rvmin

namespace RVMinII {

/// Target flags on MO_GlobalAddress operands.
enum : unsigned {
  MO_NO_FLAG = 0,
  MO_HI20 = 1 << 0,
  MO_LO12_I = 1 << 1,
  MO_LO12_S = 1 << 2,
};

} // end namespace RVMinII
} // end namespace llvm

#endif // LLVM_LIB_TARGET_RVMIN_RVMINBASEINFO_H
