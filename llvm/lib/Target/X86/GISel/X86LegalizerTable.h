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
/// The table is filled lazily, one cell at a time, from whatever the rule
/// chain answers for that cell's key.  Filling from the chain means the table
/// cannot invent an answer the chain would not give; what it *can* expose is
/// a key that merges two types the rules treat differently, which is exactly
/// what selfCheck() is for.
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_GISEL_X86LEGALIZERTABLE_H
#define LLVM_LIB_TARGET_X86_GISEL_X86LEGALIZERTABLE_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include <cstdint>
#include <string>
#include <vector>

namespace llvm {

class raw_ostream;

class X86LegalizerTable : public LegalizerInfo::LegalizeTable {
public:
  /// What the table could say about a query.
  enum class Answer : uint8_t {
    /// The cell held an answer; the result carries it.
    Hit,
    /// The cell is opaque: the caller must fall back to the rule chain.
    Escape,
    /// A keyed type is not one of the classes the table was built for.
    Outside,
    /// A keyed type is a vector of pointers, which the class list cannot
    /// express yet.  Kept apart from Outside so the gap is counted, not hidden.
    PointerVector,
    /// The table has no entry for this opcode at all.
    Untabulated,
    /// The query carries an input the key does not observe yet (a memdesc or
    /// an immediate).  Answering it from a types-only cell would be a guess.
    Declined,
  };

  struct Result {
    Answer Kind = Answer::Untabulated;
    LegalizeAction Action = LegalizeAction::Unsupported;
    unsigned TypeIdx = 0;
    LLT NewType;
  };

  explicit X86LegalizerTable(const LegalizerInfo &LI);
  ~X86LegalizerTable() override;

  /// LegalizerInfo::LegalizeTable.  Returns false when the cell is missing,
  /// opaque, or out of the table's domain; the caller then uses the rules.
  bool lookup(const LegalityQuery &Query, LegalizeActionStep &Step) const override;

  Result query(const LegalityQuery &Q) const;

  /// Run every tabulated table's key domain through both paths and report.
  /// Fatal on a mismatch: a table that disagrees with the chain is a bug in
  /// the key, not a number to look at.
  void selfCheck(raw_ostream &OS);

  uint64_t hits() const { return Hits; }
  uint64_t escapes() const { return Escapes; }
  uint64_t outside() const { return Outsides; }
  uint64_t pointerVectors() const { return PointerVectors; }
  uint64_t addressSpaces() const { return AddressSpaces; }
  uint64_t untabulated() const { return Untabulated; }
  uint64_t declined() const { return Declined; }
  uint64_t fills() const { return Fills; }

private:
  uint16_t fill(unsigned Table, const LegalityQuery &Q,
                ArrayRef<unsigned> Local) const;
  /// Record one refused query, with the reason, when asked to.
  void noteOutside(unsigned Why, unsigned Table, const LegalityQuery &Q) const;
  static LLT representative(unsigned ClassID);

  const LegalizerInfo &LI;
  mutable std::vector<std::vector<uint16_t>> Cells;
  mutable std::vector<LegalizeActionStep> Steps;
  mutable uint64_t Hits = 0, Escapes = 0, Outsides = 0, PointerVectors = 0,
                   AddressSpaces = 0, Untabulated = 0, Declined = 0, Fills = 0;
  // Off unless -x86-gisel-table-outsides.  A count says how often the table
  // refused; the log says which shapes it refused, which is what turns "41
  // outside" into "41 of these, and here is the one that is a gap".
  bool LogOutsides = false;
  mutable std::vector<std::string> OutsideLog;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_X86_GISEL_X86LEGALIZERTABLE_H
