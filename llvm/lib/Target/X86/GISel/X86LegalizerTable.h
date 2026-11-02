//===-- X86LegalizerTable.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// A table-driven view of the X86 legalizer rules.
///
/// The rules stay in X86LegalizerInfo.td.  This class only answers "which rule
/// wins for this shape" by indexing a table instead of walking the comparison
/// chain: one classify per keyed slot and one array load per query.
///
/// The table is filled lazily, one cell at a time, by the evaluator the table
/// generator emits for the rule set: the same guards and mutations the chain
/// runs, written out against the same prologue.  Typing a query by its class
/// representatives is what makes one cell stand for the whole class pair, so
/// the table can still expose a key that merges two types the rules treat
/// differently -- which is exactly what selfCheck() is for.  A rule set the
/// generator cannot translate is opaque: the reader reports it as an escape
/// and the chain answers.
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_GISEL_X86LEGALIZERTABLE_H
#define LLVM_LIB_TARGET_X86_GISEL_X86LEGALIZERTABLE_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include <cstdint>
#include <vector>

namespace llvm {

class raw_ostream;
class X86Subtarget;

class X86LegalizerTable : public LegalizerInfo::LegalizeTable {
public:
  /// What the table could say about a query.
  enum class Answer : uint8_t {
    /// The cell held an answer; the result carries it.
    Hit,
    /// A keyed type is outside the classes the table was built for, so the
    /// generated evaluator was asked directly.  The answer is still the
    /// table's own compilation of the rules -- there is no chain behind it.
    Residual,
    /// The evaluator declined: the rule set has no table translation.
    Escape,
    /// The query is malformed for this table (a memory rule with no
    /// descriptor).
    Outside,
    /// The table has no entry for this opcode at all.
    Untabulated,
  };

  struct Result {
    Answer Kind = Answer::Untabulated;
    LegalizeAction Action = LegalizeAction::Unsupported;
    unsigned TypeIdx = 0;
    LLT NewType;
  };

  /// Subtarget is the machine the rules were built for: feature guards and
  /// the pointer type are evaluated against it, exactly as the generated rule
  /// chain does.
  ///
  /// PointerBits is the width of address space 0 for that subtarget.  It is
  /// fixed for the instance and is therefore not part of the key.
  X86LegalizerTable(const LegalizerInfo &LI, const X86Subtarget &Subtarget,
                    unsigned PointerBits);
  ~X86LegalizerTable() override;

  /// LegalizerInfo::LegalizeTable.  True when the table or its evaluator
  /// answered; false only for a malformed query or an opcode it does not know.
  bool lookup(const LegalityQuery &Query,
              LegalizeActionStep &Step) const override;

  Result query(const LegalityQuery &Q) const;

  /// Run the enumerated domain through both paths and report.
  /// Fatal on a mismatch: a table that disagrees with the chain is a bug in
  /// the key, not a number to look at.
  void selfCheck(raw_ostream &OS);

private:
  uint16_t fill(unsigned Table, const LegalityQuery &Q, unsigned L0,
                unsigned L1, unsigned MemClass) const;
  LLT representative(unsigned ClassID) const;
  LLT memRepresentative(unsigned ClassID) const;

  const LegalizerInfo &LI;
  const X86Subtarget &Subtarget;
  const unsigned PointerBits;
  mutable std::vector<std::vector<uint16_t>> Cells;
  mutable std::vector<LegalizeActionStep> Steps;
  mutable uint64_t Hits = 0, Residuals = 0, Escapes = 0, Outsides = 0,
                   PointerVectors = 0, Untabulated = 0, Fills = 0;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_X86_GISEL_X86LEGALIZERTABLE_H
