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
/// The table is built from the rules: each cell names the rule that wins for
/// that cell's key.  A query classifies its types, finds its cell and replays
/// the named rule on the *query*, so every answer comes from the rules and is
/// spelled for the type that asked.  What the table can get wrong is the key
/// merging two questions the rules tell apart; the rule then does not match
/// the query and the table refuses, leaving the chain to answer.  Both claims
/// are checked rather than assumed: selfCheck() asks every key domain point,
/// and lookup() under -x86-gisel-table-check asks every real query.
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
    /// The cell named a rule and it was replayed on this query.
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
    /// The cell's rule does not answer this query: the cell merges two
    /// questions the rules tell apart.  Refused rather than answered with
    /// another shape's rule.
    Split,
  };

  struct Result {
    Answer Kind = Answer::Untabulated;
    LegalizeAction Action = LegalizeAction::Unsupported;
    unsigned TypeIdx = 0;
    LLT NewType;
  };

  /// A class's representative is built from the class itself, so it is the
  /// type the key names on every subtarget.  Nothing here takes a subtarget
  /// width: a class list that had to be re-derived per subtarget would be a
  /// second answer to "what is p0", and the two could disagree.
  /// \p Verify makes lookup() compare every answer the table gives a real
  /// query against the chain, and fail hard when they differ.  A table that
  /// answers a question it was not built for can only be caught on questions
  /// something actually asked; the domain check covers the shapes the key was
  /// built from, this covers the rest.
  explicit X86LegalizerTable(const LegalizerInfo &LI, bool Verify = false);
  ~X86LegalizerTable() override;

  /// LegalizerInfo::LegalizeTable.  Returns false when the cell is missing,
  /// declined, or out of the table's domain; the caller then uses the rules.
  bool lookup(const LegalityQuery &Query, LegalizeActionStep &Step) const override;

  Result query(const LegalityQuery &Q) const;

  /// Run every tabulated table's key domain through both paths and report.
  /// Fatal on a mismatch: a table that disagrees with the chain is a bug in
  /// the key, not a number to look at.
  void selfCheck(raw_ostream &OS);

  /// Time the same questions through the two paths getAction() can take: the
  /// rule chain, and table-then-chain-fallback.  \p Reps passes over the
  /// check domain; a warm-up pass fills every cell first and is not timed.
  void benchmark(raw_ostream &OS, unsigned Reps);

  uint64_t hits() const { return Hits; }
  uint64_t checked() const { return Checked; }
  uint64_t outside() const { return Outsides; }
  uint64_t pointerVectors() const { return PointerVectors; }
  uint64_t addressSpaces() const { return AddressSpaces; }
  uint64_t untabulated() const { return Untabulated; }
  uint64_t declined() const { return Declined; }
  uint64_t splits() const { return Splits; }
  uint64_t fills() const { return Fills; }

private:
  /// One question in the check domain, owning the storage its query points at:
  /// LegalityQuery holds ArrayRefs and cannot outlive what they name.
  struct DomainPoint {
    unsigned Opcode;
    const char *Name;
    SmallVector<LLT, 3> Types;
    SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
    /// True for the point that picks each keyed axis's class representative.
    /// A cell has exactly one of these and possibly several other members of
    /// its fiber; refusing a fiber member is the table saying "the key merged
    /// two questions", which is legal, and refusing the representative is the
    /// key naming a cell its own rule does not match, which is not.
    bool IsRepresentative = true;
    LegalityQuery query() const {
      return LegalityQuery(Opcode, Types, MemDescs);
    }
  };

  /// Enumerate every question selfCheck() and benchmark() must ask, once.
  void collectDomain(std::vector<DomainPoint> &Out) const;
  /// One question the table must refuse, plus what the classifier has to say
  /// about the type that makes it outside and which door the table has to
  /// refuse it at.  \p TypeSlot names the type slot holding the witness (it is
  /// not always 0) and is -1 for a shape-only case, a memdesc the table does
  /// not key.  \p Expected is the k*Class code the type has to classify to,
  /// and 0 when the witness is about the shape rather than the class set.
  struct OutsidePoint {
    unsigned Opcode;
    const char *Name;
    SmallVector<LLT, 3> Types;
    SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
    int TypeSlot = -1;
    int Expected = 0;
    Answer Want = Answer::Outside;
    const char *Why;
  };
  /// Enumerate the questions the table must refuse, one per reason.
  void collectOutside(std::vector<OutsidePoint> &Out) const;

  Result queryImpl(const LegalityQuery &Q) const;
  uint32_t fill(unsigned Table, const LegalityQuery &Q,
                ArrayRef<unsigned> Local) const;
  /// Record one refused query, with the reason, when asked to.
  void noteOutside(unsigned Why, unsigned Table, const LegalityQuery &Q) const;
  LLT representative(unsigned ClassID) const;

  const LegalizerInfo &LI;

  /// The largest number of spellings any one type class expanded to.  One
  /// means the fiber is the class representative alone, which is the case
  /// without extended LLTs: the check is then a cover of the quotients, not of
  /// what the quotients merged.  Reported rather than assumed.
  mutable unsigned FiberMax = 0;
  /// Float spellings the fiber declined to ask because the generator found a
  /// rule that forces a width with no FP semantics (see FloatOK).  Counted, so
  /// a slot with no float coverage is a number in the report rather than an
  /// absence nobody can see.
  mutable unsigned FiberSkipped = 0;
  mutable std::vector<std::vector<uint32_t>> Cells;
  /// Per cell, the rule that answered it -- see LegalizeRuleSet::findRule.
  ///
  /// The cell stores *which rule wins*, not the answer that rule gave.  One
  /// rule answers many queries ("widen slot 0 to 64 bits" answers both i128
  /// and s128) and its answers differ in spelling, so a stored answer would
  /// carry whichever type happened to fill the cell first.  The rule is
  /// replayed on the real query, and each query gets its own answer back.
  mutable std::vector<int64_t> RulesOfCell;
  /// -x86-gisel-table-check for the installed table: verify real queries.
  /// Checked counts how many it compared, so a run that verified nothing
  /// cannot look like a run that verified everything.
  bool Verify;
  mutable uint64_t Checked = 0;
  mutable uint64_t Hits = 0, Outsides = 0, PointerVectors = 0,
                   AddressSpaces = 0, Untabulated = 0, Declined = 0, Splits = 0,
                   Fills = 0;
  /// How deep the winning rule sits on the queries a compilation actually
  /// asks.  The check domain is uniform over type classes; real code is not,
  /// and this is what connects the two measurements.
  mutable uint64_t RuleIdxSum = 0, RuleIdxCount = 0;
  mutable unsigned RuleIdxMax = 0;
  /// Set by -x86-gisel-table-once after the one query it answers.
  mutable bool OnceServed = false;
  // Off unless -x86-gisel-table-outsides.  A count says how often the table
  // refused; the log says which shapes it refused, which is what turns "41
  // outside" into "41 of these, and here is the one that is a gap".
  bool LogOutsides = false;
  mutable std::vector<std::string> OutsideLog;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_X86_GISEL_X86LEGALIZERTABLE_H
