//===-- X86LegalizerTable.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "X86LegalizerTable.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <chrono>

using namespace llvm;

static cl::opt<bool> X86GISelTableStats(
    "x86-gisel-table-stats", cl::Hidden, cl::init(false),
    cl::desc("Report how the X86 legalizer table answered"));

static cl::opt<bool> X86GISelTableOnce(
    "x86-gisel-table-once", cl::Hidden, cl::init(false),
    cl::desc("Answer one query from the X86 legalizer table and then fall back "
             "to the rule chains.  Negative control for the table's timing: a "
             "harness that cannot tell this from -x86-gisel-table=false cannot "
             "see the table's contribution at all"));

static cl::opt<bool> X86GISelTableOutsides(
    "x86-gisel-table-outsides", cl::Hidden, cl::init(false),
    cl::desc("List every query the X86 legalizer table refused, with the reason"));

namespace {

enum LegalizerTypeClassKind : uint8_t {
  Kind_Pointer = 0,
  Kind_Scalar = 1,
  Kind_Vector = 2,
  // A vector of pointers is its own kind: LLT::fixed_vector(2, p0) and
  // LLT::fixed_vector(2, 64) are different types, so they cannot share a class.
  Kind_PointerVector = 3,
};

struct LegalizerTypeClass {
  uint8_t Kind;
  // LLT's own widths: an element/size is 32 bits and lanes are 16.  A 16-bit
  // element field would silently truncate a class the rules can spell, and the
  // classifier would then answer a question about a narrower type.
  uint32_t ElemBits;
  uint16_t Lanes;
};

} // namespace

#include "X86LegalizerTable.inc"

namespace {

/// Whether two types are the same *spelling*, not merely the same value.
///
/// LLT::operator== treats ANY_SCALAR as a wildcard: s64 equals i64 and f64
/// whenever the size matches.  Legalization's output is an LLT that ends up in
/// MIR, and on a target with extended LLTs those are different types, so the
/// checks here compare what print() says instead of what operator== hides.
bool sameSpelling(const LLT &A, const LLT &B) {
  if (A.isValid() != B.isValid())
    return false;
  if (!A.isValid())
    return true;
  std::string SA, SB;
  {
    raw_string_ostream OS(SA);
    A.print(OS);
  }
  {
    raw_string_ostream OS(SB);
    B.print(OS);
  }
  return SA == SB;
}

/// Whether a (action, slot, new type) answer is the chain's answer, spelling
/// included.  This is what every check uses; comparing LegalizeActionSteps
/// with operator== would pass a table that handed i64 the s64 an s128
/// representative produced -- the a076fbbf9e72 bug, invisible per query.
bool sameAnswer(LegalizeAction Action, unsigned TypeIdx, const LLT &NewType,
                const LegalizeActionStep &Chain) {
  return Action == Chain.Action && TypeIdx == Chain.TypeIdx &&
         sameSpelling(NewType, Chain.NewType);
}

constexpr int kUnknownClass = -1;
constexpr int kPointerVectorClass = -2;
// A pointer in an address space the table does not model.  The class list is
// built from the named LLT constants, and every one of them names p0.
constexpr int kPointerAddressSpaceClass = -3;
/// Cell indices start at 1, so 0 stays "not filled in".  Keeping the step
/// count below this reserved value means a real index can never alias the
/// empty marker.  The table only ever gets here for rule sets the Core IR can
/// express in full; a rule set with an escape is not tabulated at all, so
/// there is no opaque cell to represent.
constexpr uint32_t kCellLimit = 0xFFFFFFFF;

/// Class of a type, or one of the negative class ids above.
int classifyType(LLT T) {
  if (!T.isValid())
    return kUnknownClass;
  // The address space is part of LLT identity: LLT::pointer(272, 64) and
  // LLT::pointer(0, 64) are different types, and the rules only spell p0.
  // The class list has no address-space axis, so folding the two would let the
  // table answer a query the chain refuses (a p272 load, for example).
  if (T.isPointerOrPointerVector() && T.getAddressSpace() != 0)
    return kPointerAddressSpaceClass;
  // A vector of pointers is a different LLT from a vector of integers of the
  // same shape, and the class list cannot tell them apart yet.  Refuse rather
  // than merge two types the rules distinguish.
  const bool IsPointerVector = T.isPointerVector();
  uint8_t Kind;
  unsigned Bits;
  unsigned Lanes = 1;
  if (IsPointerVector) {
    Kind = Kind_PointerVector;
    Bits = T.getScalarSizeInBits();
    Lanes = T.getNumElements();
  } else if (T.isPointer()) {
    Kind = Kind_Pointer;
    Bits = T.getSizeInBits().getFixedValue();
  } else if (T.isVector()) {
    if (T.isScalableVector())
      return kUnknownClass;
    Kind = Kind_Vector;
    Bits = T.getScalarSizeInBits();
    Lanes = T.getNumElements();
  } else if (T.isScalar()) {
    Kind = Kind_Scalar;
    Bits = T.getSizeInBits().getFixedValue();
  } else {
    return kUnknownClass;
  }
  // The class set and the mapping onto it are one generated fact, so this is a
  // single switch rather than a scan over the class list.
  const int Class = X86ClassifyType(Kind, Bits, Lanes);
  if (Class >= 0)
    return Class;
  // A pointer vector that has no class of its own is a gap, not an unknown:
  // say so, so the count points at the missing class instead of at the type.
  return IsPointerVector ? kPointerVectorClass : kUnknownClass;
}

/// Every type a size-keyed mem cell stands for.  The fiber of an access size
/// is all LLTs of that size; lanes and address spaces are unbounded, so this is
/// the bounded sample the self-check asks about.  Two members of one cell that
/// disagree are two queries the key merged and the chain did not.
static void memFiber(unsigned Bits, SmallVectorImpl<LLT> &Out) {
  Out.push_back(LLT::scalar(Bits));
  for (unsigned Elem = 1; Elem <= 128; Elem *= 2) {
    if (Bits % Elem)
      continue;
    const unsigned Lanes = Bits / Elem;
    if (Lanes < 2 || Lanes > 64)
      continue;
    Out.push_back(LLT::fixed_vector(Lanes, LLT::scalar(Elem)));
  }
  // A vector of pointers has exactly one element type, the pointer, so its
  // total size is Lanes * PtrBits.  Asking for "size Bits" therefore fixes the
  // lane count from Bits and the pointer width; it does not let the lane count
  // pick the size.  Emitting <2..4 x pN:Bits> instead made the members 2..4
  // times as wide as the cell they stood for, and because a member the table
  // refuses is counted as `skipped` rather than as a failure, the fiber kept
  // checking other cells under this cell's name.  The caller now proves every
  // member is in its cell before asking anything.
  for (unsigned AS : {0u, 1u, 270u, 271u, 272u}) {
    for (unsigned PtrBits : {32u, 64u}) {
      if (Bits == PtrBits) {
        Out.push_back(LLT::pointer(AS, PtrBits));
        continue;
      }
      if (Bits % PtrBits)
        continue;
      const unsigned Lanes = Bits / PtrBits;
      if (Lanes < 2 || Lanes > 4)
        continue;
      Out.push_back(LLT::fixed_vector(Lanes, LLT::pointer(AS, PtrBits)));
    }
  }
}

/// Inverse of a table's global -> local map.  Local classes are a partition of
/// the global ones, so the first hit is the only hit.
unsigned localToGlobal(const LegalizerTableDesc &D, unsigned Slot,
                       unsigned Local) {
  for (unsigned G = 0; G < X86NumTypeClasses; ++G)
    if (D.Local[Slot][G] == Local)
      return G;
  llvm_unreachable("local class has no global representative");
}

} // namespace

X86LegalizerTable::X86LegalizerTable(const LegalizerInfo &LI, bool Verify)
    : LI(LI), Cells(X86NumTables), Verify(Verify),
      LogOutsides(X86GISelTableOutsides) {}

LLT X86LegalizerTable::representative(unsigned ClassID) const {
  const LegalizerTypeClass &C = X86TypeClasses[ClassID];
  switch (C.Kind) {
  case Kind_Pointer:
    return LLT::pointer(0, C.ElemBits);
  case Kind_Vector:
    return LLT::fixed_vector(C.Lanes, C.ElemBits);
  case Kind_PointerVector:
    return LLT::fixed_vector(C.Lanes, LLT::pointer(0, C.ElemBits));
  default:
    return LLT::scalar(C.ElemBits);
  }
}

uint32_t X86LegalizerTable::fill(unsigned Table, const LegalityQuery &Q,
                                 ArrayRef<unsigned> Local) const {
  const LegalizerTableDesc &D = X86Tables[Table];
  SmallVector<LLT, 4> Types(Q.Types.begin(), Q.Types.end());
  SmallVector<LegalityQuery::MemDesc, 1> MemDescs(Q.MMODescrs.begin(),
                                                  Q.MMODescrs.end());
  for (unsigned K = 0; K != D.NumSlots; ++K) {
    const int Slot = D.Slots[K];
    if (Slot < 0) {
      // Once per cell, not once per query: the mem axis is appended last by
      // the generator, and the cell index is built from the slot order, so a
      // table whose -1 was not last would read the wrong dimension.  Asserting
      // this on every lookup put an indexed load in the hot path of the mem
      // tables; here it is paid once per cell.
      assert(K + 1 == D.NumSlots && "the mem axis is the last keyed axis");
      // A size-keyed axis stands for one access size, so a scalar of exactly
      // that size answers for the cell.  A class-keyed axis stands for one
      // type, and the class's own representative is the type the rules wrote
      // down.  Alignment is not in the key: every rule asks for "at least"
      // alignment, so the weakest alignment answers for all of them, which is
      // also what the key generator asserts.
      const unsigned Key = static_cast<unsigned>(D.Mem[Local[K]]);
      const LLT MemTy = D.MemIsClass ? representative(Key) : LLT::scalar(Key);
      MemDescs.assign(1, LegalityQuery::MemDesc(MemTy, /*AlignInBits=*/1,
                                                AtomicOrdering::NotAtomic,
                                                AtomicOrdering::NotAtomic));
      continue;
    }
    Types[Slot] = representative(localToGlobal(D, K, Local[K]));
  }

  // Same question, asked with the class representatives instead of the actual
  // types: that is what makes one cell stand for the whole class pair.  The
  // rules are asked directly -- going through LI.getAction() would re-enter
  // this table and recurse.
  //
  // What is kept per cell depends on what the cell stands for.  A cell of a
  // one-question table is exactly one (opcode, types) tuple, so the answer is
  // one value and can be stored; that is what makes the table's fixed cost a
  // load instead of a rule match.  Any other cell stands for a fiber of
  // questions, and one rule answers several of them with different types, so
  // only the rule identity is kept -- see RulesOfCell.
  const LegalityQuery RepQ(Q.Opcode, Types, MemDescs, Q.Immediates);
  const LegalizeRuleSet &RS = LI.getActionDefinitions(Q.Opcode);
  const int64_t RuleIdx = RS.findRule(RepQ);
  if (RulesOfCell.size() >= kCellLimit)
    report_fatal_error("X86 legalizer table has too many filled cells");
  ++Fills;
  RulesOfCell.push_back(RuleIdx);
  // Answers is indexed with the same cell number as RulesOfCell, so it grows
  // in lockstep even for tables that never read it; skipping the push here
  // would shift every later cell's index.
  Answers.push_back(
      D.OneQuestion && RuleIdx >= 0
          ? RS.applyRule(RuleIdx, RepQ)
          : LegalizeActionStep(LegalizeAction::Unsupported, 0, LLT{}));
  return static_cast<uint32_t>(RulesOfCell.size());
}

void X86LegalizerTable::noteOutside(unsigned Why, unsigned Table,
                                      const LegalityQuery &Q) const {
  if (!LogOutsides || OutsideLog.size() >= 4096)
    return;
  std::string S;
  raw_string_ostream OS(S);
  OS << "x86-gisel-table: outside why=" << Why << " table=" << Table
     << " opcode=" << Q.Opcode << " types=(";
  for (unsigned I = 0; I < Q.Types.size(); ++I) {
    if (I)
      OS << ", ";
    Q.Types[I].print(OS);
  }
  OS << ") imms=" << Q.Immediates.size() << " mems=" << Q.MMODescrs.size();
  if (!Q.MMODescrs.empty())
    OS << " mem=" << Q.MMODescrs[0].MemoryTy.getSizeInBits().getKnownMinValue();
  OutsideLog.push_back(OS.str());
}

X86LegalizerTable::Result X86LegalizerTable::query(const LegalityQuery &Q) const {
  return queryImpl(Q, /*AllowCachedAnswer=*/true);
}

X86LegalizerTable::Result
X86LegalizerTable::queryImpl(const LegalityQuery &Q,
                             bool AllowCachedAnswer) const {
  Result R;
  const int Table = x86TableForOpcode(Q.Opcode);
  if (Table < 0) {
    ++Untabulated;
    return R;
  }
  const LegalizerTableDesc &D = X86Tables[Table];
  // The keyed axes are the type slots and, where the rule set reads one, the
  // access size of MMO 0.  A query that carries a memdesc the key does not
  // observe -- a second one, or any memdesc on a types-only table -- asks a
  // different question, so the table must not answer it.  The count says how
  // often that happens.
  //
  // Immediates are not a reason to decline, and that is by construction
  // rather than by luck: the only way to spell a predicate that reads one is
  // raw C++ (LegalizeOpaqueBool), and a rule set holding such a predicate is
  // not tabulated at all -- -gen-legalizer-table lists it under "Not
  // tabulated because they contain a predicate written as raw C++".  Every
  // predicate in a table that exists is therefore a core leaf, and no core
  // leaf reads Q.Immediates.  Measured before the guard was removed: 10
  // queries, all G_SEXT_INREG, were declined for carrying an immediate and
  // the table answered none of them; afterwards it answers them, and every
  // answer matches the chain.
  const bool HasMemAxis = D.Mem != nullptr;
  if (Q.MMODescrs.size() != (HasMemAxis ? 1u : 0u)) {
    ++Declined;
    // 8 = a memdesc count this table's shape does not match.  (7 used to mean
    // "an immediate the key cannot see"; an immediate no longer declines.)
    noteOutside(8u, Table, Q);
    R.Kind = Answer::Declined;
    return R;
  }

  // A table with no keyed axis (NumSlots == 0) answers the same thing for
  // every type, so it must not read one: G_BR carries no types at all.  The
  // mem-only shape is rejected by the generator, so a non-negative slot here
  // is the only case that asks about a type.
  const bool HasFirstTypeSlot = D.NumSlots > 0 && D.Slots[0] >= 0;
  int C0 = 0;
  if (HasFirstTypeSlot)
    C0 = Q.Types.size() > static_cast<size_t>(D.Slots[0])
             ? classifyType(Q.Types[D.Slots[0]])
             : kUnknownClass;
  // Dims[1] > 1 is not the test for "slot 1 is a type slot": a table with one
  // type slot and a mem axis has Dims {classes, mems, 1}, so its second
  // dimension is the mem axis and Dims[1] says nothing about a type.  Reading
  // D.Slots[1] there would be -1, every query would land Outside, and the
  // table would never answer -- a silent hole, not a wrong answer.  Ask the
  // slot list, which is where the mem axis actually lives.
  const bool HasSecondTypeSlot = D.NumSlots > 1 && D.Slots[1] >= 0;
  int C1 = 0;
  if (HasSecondTypeSlot)
    C1 = Q.Types.size() > static_cast<size_t>(D.Slots[1])
             ? classifyType(Q.Types[D.Slots[1]])
             : kUnknownClass;

  if (C0 == kPointerVectorClass || C1 == kPointerVectorClass) {
    ++PointerVectors;
    R.Kind = Answer::PointerVector;
    return R;
  }
  if (C0 == kPointerAddressSpaceClass || C1 == kPointerAddressSpaceClass) {
    ++AddressSpaces;
    noteOutside(6, Table, Q);
    R.Kind = Answer::Outside;
    return R;
  }
  if (C0 < 0 || C1 < 0) {
    ++Outsides;
    noteOutside(1, Table, Q);
    R.Kind = Answer::Outside;
    return R;
  }

  unsigned L[3] = {0, 0, 0};
  L[0] = D.Local[0][C0];
  if (L[0] == 0xFF) {
    ++Outsides;
    noteOutside(2, Table, Q);
    R.Kind = Answer::Outside;
    return R;
  }
  if (HasSecondTypeSlot) {
    L[1] = D.Local[1][C1];
    if (L[1] == 0xFF) {
      ++Outsides;
      noteOutside(3, Table, Q);
      R.Kind = Answer::Outside;
      return R;
    }
  }
  if (HasMemAxis) {
    // Two ways to cut the axis, and the rule set says which one it needs.  A
    // rule that only compares getSizeInBits() (isCompatible) is answered
    // exactly by the access size.  A rule that compares the whole memory type
    // needs the type, because two memory types of the same size can still
    // differ: G_STORE's ValueDiffersFromMem0 is the one that does.
    int MemKey = -1;
    if (D.MemIsClass) {
      MemKey = classifyType(Q.MMODescrs[0].MemoryTy);
      if (MemKey < 0) {
        ++Outsides;
        noteOutside(4, Table, Q);
        R.Kind = Answer::Outside;
        return R;
      }
    } else {
      const TypeSize MemSize = Q.MMODescrs[0].MemoryTy.getSizeInBits();
      if (MemSize.isScalable()) {
        ++Outsides;
        noteOutside(4, Table, Q);
        R.Kind = Answer::Outside;
        return R;
      }
      MemKey = static_cast<int>(MemSize.getFixedValue());
    }
    // The axis is a generated switch over the values the rules name, so this
    // is one lookup rather than a scan; -1 is a value no rule names.
    const int MemLocal = x86MemIndex(Table, MemKey);
    if (MemLocal < 0) {
      ++Outsides;
      noteOutside(5, Table, Q);
      R.Kind = Answer::Outside;
      return R;
    }
    // The generator appends the mem axis last, but it is not always the
    // third dimension: a table with one type slot carries it at index 1, and
    // the cell index is built from the slot order.  Put the local where the
    // axis is.
    L[D.NumSlots - 1] = static_cast<unsigned>(MemLocal);
  }

  std::vector<uint32_t> &Row = Cells[Table];
  if (Row.empty())
    Row.assign(static_cast<size_t>(D.Dims[0]) * D.Dims[1] * D.Dims[2], 0);
  uint32_t &Cell =
      Row[(static_cast<size_t>(L[0]) * D.Dims[1] + L[1]) * D.Dims[2] + L[2]];
  if (!Cell)
    Cell = fill(Table, Q, L);

  // A cell of a one-question table holds the answer itself; there is nothing
  // left to ask.  This is the whole point of the cache: the query's fixed cost
  // becomes a virtual call, a classify and a load, where replaying the rule
  // would cost a rule match -- the same predicate call the chain makes, plus
  // the table's own work on top.
  //
  // Sound because the answer is a function of the cell: the key reads every
  // slot a predicate reads, a class names exactly one LLT, and a tabulated
  // rule reads no immediate.  The generator only sets OneQuestion for tables
  // where the key also covers the slot every rule action writes, so the cell
  // fixes the rule's mutations too.  lookup() verifies this on real queries
  // under -x86-gisel-table-check rather than trusting the argument.
  // A cell's stored answer is the chain's answer for the cell's
  // *representative*, and that is the same answer for every query in the cell
  // only if the key is injective over the LLTs the target can produce.  It is
  // not when LLT kinds are distinct: classifyType() maps INTEGER, FLOAT and
  // ANY_SCALAR onto one scalar class, so an i128 query lands in the s128 cell
  // and would be handed s64 where the chain says i64 -- a076fbbf9e72.  Which
  // LLTs exist is a property of the target, published before any query runs,
  // so that is where the decision belongs: on a target with extended LLTs the
  // cache is off and the rule number is replayed on the real query, which
  // recomputes the mutation instead of copying the representative's.
  //
  // X86 never enables extended LLTs, so this is a relaxed load of a global
  // that is constant for the process; -x86-gisel-extended-llt exists to make
  // the closed path reachable in a test.
  if (D.OneQuestion && AllowCachedAnswer && !LLT::getUseExtended()) {
    const int64_t RuleIdx = RulesOfCell[Cell - 1];
    if (RuleIdx >= 0) {
      RuleIdxSum += static_cast<uint64_t>(RuleIdx);
      ++RuleIdxCount;
      RuleIdxMax = std::max(RuleIdxMax, static_cast<unsigned>(RuleIdx));
    }
    const LegalizeActionStep &S = Answers[Cell - 1];
    ++Hits;
    ++Cached;
    R.Kind = Answer::Hit;
    R.Action = S.Action;
    R.TypeIdx = S.TypeIdx;
    R.NewType = S.NewType;
    return R;
  }

  // Replay the winning rule on *this* query.  The cell was filled from the
  // class representatives, so anything the answer derives from the actual type
  // -- the element type a scalar change lands on, the spelling of a scalar --
  // is recomputed here instead of being copied out of the fill.
  const LegalizeRuleSet &RS = LI.getActionDefinitions(Q.Opcode);
  const int64_t RuleIdx = RulesOfCell[Cell - 1];
  if (RuleIdx >= 0 && !RS.ruleMatches(RuleIdx, Q)) {
    // The rule that answered the representative does not answer this query:
    // the cell merges two questions, so answering would be answering one with
    // the other's rule.  Refuse; the count is the size of the key's gap.
    ++Splits;
    noteOutside(9, Table, Q);
    R.Kind = Answer::Split;
    return R;
  }
  // ruleMatches() proves the cached rule still matches; it cannot prove that
  // no *earlier* rule does, which is what the chain would answer with.  That
  // is sound only because every predicate of a tabulated rule set is constant
  // on a cell: the emitter keys every slot any predicate leaf reads, refuses
  // (fatally) a leaf it does not know, and cuts the mem axis on the type class
  // when a rule reads the memory type as a whole -- so within one cell the
  // first matching rule cannot change.  Check mode tests exactly that on the
  // questions a compilation asks instead of trusting the argument; the release
  // path pays nothing for it.
  if (RuleIdx >= 0 && Verify) {
    const int64_t ChainRule = RS.findRule(Q);
    if (ChainRule != RuleIdx) {
      std::string S;
      raw_string_ostream OS(S);
      OS << "x86-gisel-table-check: the cell's rule is not the chain's rule: "
            "table "
         << RuleIdx << " chain " << ChainRule << " for ";
      Q.print(OS);
      report_fatal_error(StringRef(S));
    }
  }
  if (RuleIdx >= 0) {
    RuleIdxSum += static_cast<uint64_t>(RuleIdx);
    ++RuleIdxCount;
    RuleIdxMax = std::max(RuleIdxMax, static_cast<unsigned>(RuleIdx));
  }
  const LegalizeActionStep S =
      RuleIdx < 0 ? LegalizeActionStep(LegalizeAction::Unsupported, 0, LLT{})
                  : RS.applyRule(RuleIdx, Q);
  ++Hits;
  R.Kind = Answer::Hit;
  R.Action = S.Action;
  R.TypeIdx = S.TypeIdx;
  R.NewType = S.NewType;
  return R;
}

bool X86LegalizerTable::lookup(const LegalityQuery &Q,
                               LegalizeActionStep &Step) const {
  // -x86-gisel-table-once serves one query and then stands aside, leaving the
  // chain to answer everything else.  That is the chain-only cost plus a
  // constant, so a timing run that cannot separate it from
  // -x86-gisel-table=false is not resolving the table at all.
  if (X86GISelTableOnce && OnceServed)
    return false;
  const Result R = query(Q);
  if (R.Kind != Answer::Hit)
    return false;
  if (X86GISelTableOnce)
    OnceServed = true;
  Step = LegalizeActionStep(R.Action, R.TypeIdx, R.NewType);
  if (Verify) {
    // A real query, not a representative: this is the only place a cached
    // answer can be wrong, because the domain check asks the same question
    // the fill asked.  Same comparison `selfCheck` makes, on the other half
    // of the question space.
    ++Checked;
    const LegalizeActionStep Chain =
        LI.getActionDefinitions(Q.Opcode).apply(Q);
    // Compared by spelling, not by LegalizeActionStep::operator==: that
    // operator's LLT comparison treats s64, i64 and f64 as one type, so it
    // cannot see the one disagreement a cached answer can introduce.
    if (!sameAnswer(Step.Action, Step.TypeIdx, Step.NewType, Chain)) {
      std::string S;
      raw_string_ostream OS(S);
      OS << "x86-gisel-table-check: the table answered a query the chain does "
            "not (new type compared by spelling, not by LLT::operator==); "
            "table "
         << static_cast<unsigned>(Step.Action) << "/" << Step.TypeIdx << "/";
      Step.NewType.print(OS);
      OS << ", chain " << static_cast<unsigned>(Chain.Action) << "/"
         << Chain.TypeIdx << "/";
      Chain.NewType.print(OS);
      OS << " for ";
      Q.print(OS);
      report_fatal_error(StringRef(S));
    }
  }
  return true;
}

X86LegalizerTable::~X86LegalizerTable() {
  if (!X86GISelTableStats)
    return;
  errs() << "x86-gisel-table: hit " << Hits << " outside " << Outsides
         << " ptrvec " << PointerVectors << " addrspace " << AddressSpaces
         << " untabulated " << Untabulated << " splits " << Splits
         << " declined " << Declined << " fills " << Fills << " cached "
         << Cached << " checked " << Checked << " rule_mean "
         << format("%.2f",
                   RuleIdxCount ? double(RuleIdxSum) / RuleIdxCount : 0.0)
         << " rule_max " << RuleIdxMax << "\n";
  for (const std::string &S : OutsideLog)
    errs() << S << "\n";
}

void X86LegalizerTable::collectDomain(std::vector<DomainPoint> &Out) const {
  // The domain is generated with the tables: each keyed slot lists the classes
  // its rules name for it, so these are the questions the IR can produce.
  // Iterating the full class set instead would ask the chain about types it is
  // not written for (a pointer in a scalar slot, for one), which measures
  // nothing about the table.
  static_assert(X86NumTableDomains == X86NumTables,
                "every table needs a self-check domain");

  for (unsigned EntryIdx = 0; EntryIdx != X86NumTableOpcodes; ++EntryIdx) {
    const auto &Entry = X86TableOpcodes[EntryIdx];
    const LegalizerTableDesc &D = X86Tables[Entry.Table];
    const LegalizerTableDomain &Domain = X86TableDomains[Entry.Table];
    const unsigned N0 = D.NumSlots > 0 ? Domain.Counts[0] : 1;
    const unsigned N1 = D.NumSlots > 1 ? Domain.Counts[1] : 1;
    const unsigned N2 = D.NumSlots > 2 ? Domain.Counts[2] : 1;

    // A tabulated table reads no type slot outside its key, so an unkeyed
    // slot can use any representative; it is the same one fill() saw.
    int MaxSlot = -1;
    for (unsigned K = 0; K != D.NumSlots; ++K)
      if (D.Slots[K] > MaxSlot)
        MaxSlot = D.Slots[K];

    for (unsigned I0 = 0; I0 != N0; ++I0) {
      for (unsigned I1 = 0; I1 != N1; ++I1) {
        for (unsigned I2 = 0; I2 != N2; ++I2) {
          const unsigned Local[3] = {I0, I1, I2};
          SmallVector<LLT, 3> Types(static_cast<size_t>(MaxSlot + 1),
                                    representative(0));
          unsigned MemBits = 0;
          LLT MemClassTy;
          for (unsigned K = 0; K != D.NumSlots; ++K) {
            if (D.Slots[K] < 0) {
              // A class-keyed cell is exactly one type, so it needs no fiber.
              const unsigned Key = static_cast<unsigned>(D.Mem[Local[K]]);
              if (D.MemIsClass)
                MemClassTy = representative(Key);
              else
                MemBits = Key;
            } else {
              Types[D.Slots[K]] = representative(Domain.Classes[K][Local[K]]);
            }
          }

          // A type slot's class is exactly one type, so its representative is
          // the whole cell.  The mem axis is keyed by access size, so its cell
          // holds every LLT of that size: asking only the scalar would leave
          // the half of the cell the key actually merges untested.
          SmallVector<LLT, 16> MemTypes;
          if (MemClassTy.isValid())
            MemTypes.push_back(MemClassTy);
          else if (MemBits)
            memFiber(MemBits, MemTypes);
          else
            MemTypes.push_back(LLT());

          for (LLT MemTy : MemTypes) {
            // A fiber member is supposed to be another spelling of this cell's
            // key.  One that is not is a defect in this file, not a question
            // about the table: `query` would answer it from the cell its own
            // size names (or refuse it), and the comparison would then be
            // filed under the wrong cell or dropped into `skipped`.
            if (MemTy.isValid() && !MemClassTy.isValid()) {
              const TypeSize MemberSize = MemTy.getSizeInBits();
              if (MemberSize.isScalable() ||
                  MemberSize.getFixedValue() != MemBits) {
                std::string S;
                raw_string_ostream OS(S);
                OS << "x86-gisel-table-check: " << Entry.Name
                   << ": a fiber member for access " << MemBits
                   << " bits is not that size: ";
                MemTy.print(OS);
                report_fatal_error(StringRef(S));
              }
            }
            DomainPoint P;
            P.Opcode = Entry.Opcode;
            P.Name = Entry.Name;
            P.Types = Types;
            P.MemDescs.clear();
            if (MemTy.isValid())
              P.MemDescs.emplace_back(MemTy, /*AlignInBits=*/1,
                                      AtomicOrdering::NotAtomic,
                                      AtomicOrdering::NotAtomic);
            Out.push_back(std::move(P));
          }
        }
      }
    }
  }
}

void X86LegalizerTable::selfCheck(raw_ostream &OS) {
  std::vector<DomainPoint> Domain;
  collectDomain(Domain);

  uint64_t Points = 0;
  uint64_t Compared = 0;
  uint64_t Skipped = 0;
  unsigned Mismatches = 0;

  for (const DomainPoint &P : Domain) {
    const LegalityQuery Q = P.query();
    ++Points;
    // Ask the rules directly.  LI.getAction() consults the installed table
    // first, which would compare the table against itself; the point of this
    // check is the chain.
    const LegalizeActionStep ChainStep =
        LI.getActionDefinitions(Q.Opcode).apply(Q);
    // Uncached: the point of the check is that the table's key and the chain
    // agree, which a stored answer would state about itself.
    const Result R = queryUncached(Q);
    if (R.Kind == Answer::Split) {
      // The cell the key named does not hold this question: that is a defect
      // in the key, not a table that answered wrongly.
      ++Mismatches;
      if (Mismatches <= 20) {
        OS << "x86-gisel-table-check: SPLIT " << P.Name << "(";
        for (unsigned I = 0; I != P.Types.size(); ++I) {
          if (I)
            OS << ", ";
          P.Types[I].print(OS);
        }
        OS << "): the cell's rule does not answer this query\n";
      }
      continue;
    }
    if (R.Kind != Answer::Hit) {
      ++Skipped;
      continue;
    }
    ++Compared;
    if (sameAnswer(R.Action, R.TypeIdx, R.NewType, ChainStep))
      continue;
    ++Mismatches;
    if (Mismatches <= 20) {
      OS << "x86-gisel-table-check: MISMATCH " << P.Name << "(";
      for (unsigned I = 0; I != P.Types.size(); ++I) {
        if (I)
          OS << ", ";
        P.Types[I].print(OS);
      }
      if (!P.MemDescs.empty()) {
        OS << " mem ";
        P.MemDescs[0].MemoryTy.print(OS);
      }
      OS << "): table ";
      OS << static_cast<unsigned>(R.Action) << "/" << R.TypeIdx << "/";
      R.NewType.print(OS);
      OS << " chain " << static_cast<unsigned>(ChainStep.Action) << "/"
         << ChainStep.TypeIdx << "/";
      ChainStep.NewType.print(OS);
      OS << "\n";
    }
  }

  OS << "x86-gisel-table-check: points " << Points << " compared " << Compared
     << " mismatches " << Mismatches << " skipped " << Skipped << " outside "
     << Outsides << " ptrvec " << PointerVectors << " addrspace "
     << AddressSpaces << " untabulated " << Untabulated << " splits " << Splits
     << " declined " << Declined << " fills " << Fills << "\n";
  if (Mismatches)
    report_fatal_error(
        "x86-gisel-table-check: the table and the chain disagree");
}

namespace {
/// Call the table the way a compilation does: through the base class and out
/// of line, so this stays a virtual call.  Calling lookup() on the concrete
/// object would let the compiler devirtualize and inline the classify, which
/// would time a table that never exists in a compilation.
LLVM_ATTRIBUTE_NOINLINE bool
lookupThroughBase(const LegalizerInfo::LegalizeTable &T, const LegalityQuery &Q,
                  LegalizeActionStep &Step) {
  return T.lookup(Q, Step);
}
} // namespace

void X86LegalizerTable::benchmark(raw_ostream &OS, unsigned Reps) {
  std::vector<DomainPoint> Domain;
  collectDomain(Domain);

  using Clock = std::chrono::steady_clock;
  const auto ElapsedNs = [](Clock::time_point A, Clock::time_point B) {
    return std::chrono::duration<double, std::nano>(B - A).count();
  };

  // Warm up: fill every cell and check that the path production takes (table
  // first, chain on a decline) says exactly what the chain alone says.  The
  // first pass is also where the table pays a cost the chain does not have, one
  // fill per key; a compilation pays that once and the pass-level measurement
  // keeps it, so it is warm-up here rather than charged Reps times.
  uint64_t Sink = 0;
  unsigned Hits = 0;
  unsigned Mismatches = 0;
  uint64_t RuleIdxSum = 0;
  unsigned RuleIdxSeen = 0;
  unsigned RuleIdxMax = 0;
  const LegalizerInfo::LegalizeTable &Base = *this;
  for (const DomainPoint &P : Domain) {
    const LegalityQuery Q = P.query();
    const LegalizeActionStep Chain = LI.getActionDefinitions(Q.Opcode).apply(Q);
    LegalizeActionStep Mixed(LegalizeAction::Unsupported, 0, LLT{});
    if (lookupThroughBase(Base, Q, Mixed))
      ++Hits;
    else
      Mixed = Chain;
    if (!sameAnswer(Mixed.Action, Mixed.TypeIdx, Mixed.NewType, Chain))
      ++Mismatches;
    Sink += static_cast<unsigned>(Mixed.Action) + Mixed.TypeIdx;

    // How deep the winning rule sits is how many indirect predicate calls the
    // chain makes before it answers, and what one classify plus one cell load
    // replaces.  Reporting it keeps the timings explainable.
    const int64_t RuleIdx = LI.getActionDefinitions(Q.Opcode).findRule(Q);
    if (RuleIdx >= 0) {
      RuleIdxSum += static_cast<uint64_t>(RuleIdx);
      ++RuleIdxSeen;
      RuleIdxMax = std::max(RuleIdxMax, static_cast<unsigned>(RuleIdx));
    }
  }
  if (Mismatches)
    report_fatal_error(
        "x86-gisel-table-bench: the table and the chain disagree");

  double ChainNs = 0, MixedNs = 0;
  {
    const Clock::time_point T0 = Clock::now();
    for (unsigned R = 0; R != Reps; ++R)
      for (const DomainPoint &P : Domain) {
        const LegalityQuery Q = P.query();
        const LegalizeActionStep S = LI.getActionDefinitions(Q.Opcode).apply(Q);
        Sink += static_cast<unsigned>(S.Action) + S.TypeIdx;
      }
    ChainNs = ElapsedNs(T0, Clock::now());
  }
  {
    const Clock::time_point T0 = Clock::now();
    for (unsigned R = 0; R != Reps; ++R)
      for (const DomainPoint &P : Domain) {
        const LegalityQuery Q = P.query();
        LegalizeActionStep S(LegalizeAction::Unsupported, 0, LLT{});
        if (!lookupThroughBase(Base, Q, S))
          S = LI.getActionDefinitions(Q.Opcode).apply(Q);
        Sink += static_cast<unsigned>(S.Action) + S.TypeIdx;
      }
    MixedNs = ElapsedNs(T0, Clock::now());
  }

  const double Queries = static_cast<double>(Reps) * Domain.size();
  OS << "x86-gisel-table-bench: points " << Domain.size() << " hits " << Hits
     << " reps " << Reps << " chain_ns " << format("%.2f", ChainNs / Queries)
     << " mixed_ns " << format("%.2f", MixedNs / Queries) << " speedup "
     << format("%.3f", ChainNs / MixedNs) << " mean_rule "
     << format("%.2f", RuleIdxSeen ? double(RuleIdxSum) / RuleIdxSeen : 0.0)
     << " max_rule " << RuleIdxMax << " sink " << Sink << "\n";
}
