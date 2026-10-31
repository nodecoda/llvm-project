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

#include "llvm/ADT/STLFunctionalExtras.h"
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
    /// Which axis the key refused, for Kind == Outside: 1 = a keyed type has
    /// no class at all, 2 = slot 0's class is not in this table's map, 3 = the
    /// same for slot 1, 4 = the memory type is scalable, 5 = the access size is
    /// one no rule names, 6 = a pointer in an address space the class list does
    /// not model.  0 for every other Kind.
    unsigned Why = 0;
  };

  /// \p PointerBits is the width the current subtarget gives LLT::pointer(0),
  /// which X86_p0 denotes.  The class list pins it to 64 because the table only
  /// installs on LP64, but the self-check also runs where the table is not
  /// installed: there the representatives have to be types the chain knows,
  /// or the check asks the chain a question it is not written for.
  explicit X86LegalizerTable(const LegalizerInfo &LI, unsigned PointerBits);
  ~X86LegalizerTable() override;

  /// LegalizerInfo::LegalizeTable.  Returns false when the cell is missing,
  /// declined, or out of the table's domain; the caller then uses the rules.
  bool lookup(const LegalityQuery &Query, LegalizeActionStep &Step) const override;

  Result query(const LegalityQuery &Q) const;

  /// Run every tabulated table's key domain through both paths and report.
  /// Fatal on a mismatch: a table that disagrees with the chain is a bug in
  /// the key, not a number to look at.
  void selfCheck(raw_ostream &OS);
  /// Write the table's answers, with the chain's, as one TSV row per
  /// (domain point, memdesc variant).  This writes data, not a verdict: a row
  /// whose table and chain disagree is recorded with match=mismatch, and the
  /// caller -- not this function -- decides what that means.
  ///
  /// The domain is the same one selfCheck() walks.  A memdesc point is written
  /// once per (alignment, ordering) variant because the key ignores both: the
  /// rows are how "the chain does not read them either" becomes evidence
  /// instead of an assumption.
  /// \p Features is the subtarget's feature vector as an opaque key: two CPUs
  /// that enable the same features must produce the same key, which is what
  /// makes a sweep over -mcpu a sweep over feature vectors.
  void dumpAnswers(raw_ostream &OS, StringRef CPU, StringRef Triple,
                   StringRef Features) const;

  uint64_t hits() const { return Hits; }
  uint64_t outside() const { return Outsides; }
  uint64_t pointerVectors() const { return PointerVectors; }
  uint64_t addressSpaces() const { return AddressSpaces; }
  uint64_t untabulated() const { return Untabulated; }
  uint64_t declined() const { return Declined; }
  uint64_t fills() const { return Fills; }

private:
  /// Walk the self-check domain once, handing each point to \p F: the opcode
  /// name, its entry in the generated opcode list, the local class indices,
  /// the representative types, and the memory type (invalid for a point with
  /// no mem axis).  Shared by selfCheck() and dumpAnswers() so the two cannot
  /// drift into checking different questions.
  void forEachPoint(function_ref<void(StringRef Name, unsigned Entry,
                                     ArrayRef<unsigned> Local,
                                     ArrayRef<LLT> Types, LLT MemTy)>
                        F) const;
  uint32_t fill(unsigned Table, const LegalityQuery &Q,
                ArrayRef<unsigned> Local) const;
  /// Record one refused query, with the reason, when asked to.
  void noteOutside(unsigned Why, unsigned Table, const LegalityQuery &Q) const;
  LLT representative(unsigned ClassID) const;

  const LegalizerInfo &LI;
  const unsigned PointerBits;
  mutable std::vector<std::vector<uint32_t>> Cells;
  mutable std::vector<LegalizeActionStep> Steps;
  mutable uint64_t Hits = 0, Outsides = 0, PointerVectors = 0,
                   AddressSpaces = 0, Untabulated = 0, Declined = 0, Fills = 0;
  // Off unless -x86-gisel-table-outsides.  A count says how often the table
  // refused; the log says which shapes it refused, which is what turns "41
  // outside" into "41 of these, and here is the one that is a gap".
  bool LogOutsides = false;
  mutable std::vector<std::string> OutsideLog;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_X86_GISEL_X86LEGALIZERTABLE_H
