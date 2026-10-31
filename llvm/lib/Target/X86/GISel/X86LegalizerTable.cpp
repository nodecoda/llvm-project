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
#include "llvm/Support/raw_ostream.h"

#include <cassert>

using namespace llvm;

static cl::opt<bool> X86GISelTableStats(
    "x86-gisel-table-stats", cl::Hidden, cl::init(false),
    cl::desc("Report how the X86 legalizer table answered"));

static cl::opt<bool> X86GISelTableOutsides(
    "x86-gisel-table-outsides", cl::Hidden, cl::init(false),
    cl::desc("List every query the X86 legalizer table refused, with the reason"));

static cl::opt<bool> X86GISelTableCheckFull(
    "x86-gisel-table-check-full", cl::Hidden, cl::init(false),
    cl::desc("Check every class of every slot, not only the classes the "
             "rules name"));

static cl::opt<bool> X86GISelTableCheckTrace(
    "x86-gisel-table-check-trace", cl::Hidden, cl::init(false),
    cl::desc("Print every point the self-check asks the rules about"));

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
  uint16_t ElemBits;
  uint16_t Lanes;
};

} // namespace

#include "X86LegalizerTable.inc"

namespace {

constexpr int kUnknownClass = -1;
constexpr int kPointerVectorClass = -2;
// A pointer in an address space the table does not model.  The class list is
// built from the named LLT constants, and every one of them names p0.
constexpr int kPointerAddressSpaceClass = -3;
/// A plan step whose rule matched but whose answer the generator could not
/// compute.  Kept apart from a cell that has no plan at all: both fall back to
/// the chain, and neither is an answer the table may hand out.
constexpr uint16_t kNoAnswer = 0xFFFF;

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
/// is the set of LLTs whose size is exactly that one; lanes and address spaces
/// are unbounded, so this is the bounded sample the self-check asks about.  Two
/// members of one cell that disagree are two queries the key merged and the
/// chain did not.
///
/// Every member really has \p Bits.  A vector of pointers whose total size is
/// different belongs to a different cell, and asking it here would spend the
/// check on a query the cell never answers: measured, a fiber that added
/// v2p0/v3p0/v4p0 to the 32- and 64-bit cells produced 590 points whose size
/// no rule names, all of which the chain also rejects -- 12390 rows of noise
/// (all of them, in fact, "outside").
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
  // A pointer element is part of the same cell: a 128-bit access may be a
  // v16s8, a v4s32, or a v2p0, and the key sees one size for all three.  These
  // are the shapes a size-keyed axis could hide a rule's dependence on.
  for (unsigned ElemBits : {32u, 64u}) {
    if (Bits % ElemBits)
      continue;
    const unsigned Lanes = Bits / ElemBits;
    if (Lanes > 8)
      continue;
    for (unsigned AS : {0u, 1u, 270u, 271u, 272u}) {
      const LLT P = LLT::pointer(AS, ElemBits);
      Out.push_back(Lanes == 1 ? P : LLT::fixed_vector(Lanes, P));
    }
  }
}

} // namespace

X86LegalizerTable::X86LegalizerTable(const LegalizerInfo &LI,
                                     unsigned PointerBits, uint32_t Theta)
    : LI(LI), PointerBits(PointerBits), Theta(Theta),
      PlanAnswer(X86NumPlans, kNoAnswer),
      LogOutsides(X86GISelTableOutsides) {
  // One answer per plan, decided once, here.  A plan is a list of theta cubes,
  // so this loop is the whole run-time cost of the table: -O2 lowering of a
  // query then reads one cell and one answer.
  for (unsigned T = 0; T != X86NumTables; ++T) {
    const LegalizerTableDesc &D = X86Tables[T];
    // theta as this table writes it: bit I is the boolean D.Atoms[I] names.
    // A table reads only the booleans its own rules mention, so two tables can
    // read the same subtarget through different bit orders.
    uint32_t Local = 0;
    for (unsigned I = 0; I != D.NumAtoms; ++I)
      if ((Theta >> D.Atoms[I]) & 1u)
        Local |= 1u << I;
    const unsigned Cells = static_cast<unsigned>(D.Dims[0]) * D.Dims[1] *
                           D.Dims[2];
    for (unsigned C = 0; C != Cells; ++C) {
      const uint16_t Cell = D.Cells[C];
      if (!Cell)
        continue;
      const unsigned P = Cell - 1;
      assert(P < X86NumPlans && "cell names a plan that does not exist");
      // A plan is shared by every cell whose answer depends on theta the same
      // way, so it is answered once.
      if (PlanAnswer[P] != kNoAnswer)
        continue;
      const LegalizerPlan &Plan = X86Plans[P];
      for (unsigned S = 0; S != Plan.NumSteps; ++S) {
        const LegalizerPlanStep &Step = X86PlanSteps[Plan.FirstStep + S];
        if ((Local & Step.Mask) == Step.Value) {
          PlanAnswer[P] = Step.Answer;
          break;
        }
      }
    }
  }
}

LLT X86LegalizerTable::answerType(uint16_t NewType) const {
  if (!NewType)
    return LLT();
  const LegalizerLLT &T = X86LLTs[NewType - 1];
  switch (T.Kind) {
  case LK_Pointer:
    return LLT::pointer(0, T.Bits);
  case LK_Vector:
    return LLT::fixed_vector(T.Lanes, T.Bits);
  case LK_PointerVector:
    return LLT::fixed_vector(T.Lanes, LLT::pointer(0, T.Bits));
  case LK_Scalar:
    return LLT::scalar(T.Bits);
  default:
    llvm_unreachable("a frozen new type with no kind");
  }
}

LLT X86LegalizerTable::representative(unsigned ClassID) const {
  const LegalizerTypeClass &C = X86TypeClasses[ClassID];
  switch (C.Kind) {
  case Kind_Pointer:
    return LLT::pointer(0, PointerBits);
  case Kind_Vector:
    return LLT::fixed_vector(C.Lanes, C.ElemBits);
  case Kind_PointerVector:
    return LLT::fixed_vector(C.Lanes, LLT::pointer(0, PointerBits));
  default:
    return LLT::scalar(C.ElemBits);
  }
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
  OS << ")";
  if (!Q.MMODescrs.empty())
    OS << " mem=" << Q.MMODescrs[0].MemoryTy.getSizeInBits().getKnownMinValue();
  OutsideLog.push_back(OS.str());
}

X86LegalizerTable::Result
X86LegalizerTable::query(const LegalityQuery &Q) const {
  Result R;
  const int Table = x86TableForOpcode(Q.Opcode);
  if (Table < 0) {
    ++Untabulated;
    return R;
  }
  const LegalizerTableDesc &D = X86Tables[Table];
  // The keyed axes are the type slots and, where the rule set reads one, the
  // access size of MMO 0.  A query that carries an axis the key does not
  // observe -- an immediate anywhere, a second memdesc, or a memdesc on a
  // types-only table -- is a different question with the same key, so the
  // table must not answer it.  The count says how often that happens.
  const bool HasMemAxis = D.Mem != nullptr;
  if (!Q.Immediates.empty() ||
      Q.MMODescrs.size() != (HasMemAxis ? 1u : 0u)) {
    ++Declined;
    R.Kind = Answer::Declined;
    return R;
  }

  // A table with no keyed axis (NumSlots == 0) answers the same thing for
  // every type, so it must not read one: G_BR carries no types at all.
  const int C0 = D.NumSlots == 0
                     ? 0
                     : (Q.Types.size() > D.Slots[0]
                            ? classifyType(Q.Types[D.Slots[0]])
                            : kUnknownClass);
  // Slot 1 is classified whenever the table keys it -- even when its domain is
  // one class wide.  A domain of one is not "do not look": it is "only this
  // class is modelled", and a type that maps outside it must be refused.  The
  // load/store pointer slot is exactly that case (p0 only), and reading Dims
  // as "is the slot used" let every other address space fall into the p0 cell.
  int C1 = 0;
  if (D.NumSlots > 1 && D.Slots[1] >= 0)
    C1 = Q.Types.size() > D.Slots[1] ? classifyType(Q.Types[D.Slots[1]])
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
    R.Why = 6;
    return R;
  }
  if (C0 < 0 || C1 < 0) {
    ++Outsides;
    noteOutside(1, Table, Q);
    R.Kind = Answer::Outside;
    R.Why = 1;
    return R;
  }

  unsigned L[3] = {0, 0, 0};
  L[0] = D.Local[0][C0];
  if (L[0] == 0xFF) {
    ++Outsides;
    noteOutside(2, Table, Q);
    R.Kind = Answer::Outside;
    R.Why = 2;
    return R;
  }
  if (D.NumSlots > 1 && D.Slots[1] >= 0) {
    L[1] = D.Local[1][C1];
    if (L[1] == 0xFF) {
      ++Outsides;
      noteOutside(3, Table, Q);
      R.Kind = Answer::Outside;
      R.Why = 3;
      return R;
    }
  }
  if (HasMemAxis) {
    // The axis is cut on the access size, and a cell is filled with a scalar of
    // that size as its representative.  Every tabulated memdesc rule compares
    // the memory type by size (isCompatible), so the scalar representative is
    // exact even when the query's MemoryTy is a vector.  A rule set that
    // compares the whole MemoryTy is opaque and is not tabulated.
    // A rule names an access size, not a range (isCompatible compares
    // getSizeInBits()), so the axis is an exact match.  A size no rule names
    // is outside the key: the chain answers, and the fallback is counted.
    const TypeSize MemSize = Q.MMODescrs[0].MemoryTy.getSizeInBits();
    if (MemSize.isScalable()) {
      ++Outsides;
      noteOutside(4, Table, Q);
      R.Kind = Answer::Outside;
      R.Why = 4;
      return R;
    }
    // The axis is a generated switch over the sizes the rules name, so this
    // is one lookup rather than a scan; -1 is a size no rule names.
    const uint64_t Bits = MemSize.getFixedValue();
    const int MemLocal = x86MemIndex(Table, Bits);
    if (MemLocal < 0) {
      ++Outsides;
      noteOutside(5, Table, Q);
      R.Kind = Answer::Outside;
      R.Why = 5;
      return R;
    }
    L[2] = static_cast<unsigned>(MemLocal);
  }

  // A frozen table can only carry the cells its generator was allowed to
  // evaluate and check, which is the rule-named domain.  Count the answered
  // queries whose class sits outside it: those are the ones a frozen table
  // would have to hand back to the chain.
  const LegalizerTableDomain &Dom = X86TableDomains[Table];
  bool InDomain = true;
  for (unsigned K = 0; K != D.NumSlots; ++K) {
    if (D.Slots[K] < 0)
      continue;
    const unsigned C = K == 0 ? static_cast<unsigned>(C0)
                              : (K == 1 ? static_cast<unsigned>(C1) : 0u);
    bool Found = false;
    for (unsigned I = 0; I != Dom.Counts[K]; ++I)
      if (Dom.Classes[K][I] == C) {
        Found = true;
        break;
      }
    if (!Found) {
      InDomain = false;
      break;
    }
  }
  if (!InDomain) {
    ++OutOfDomain;
    if (LogOutsides && OutsideLog.size() < 4096) {
      std::string S;
      raw_string_ostream OS(S);
      OS << "x86-gisel-table: out-of-domain table=" << Table
         << " opcode=" << Q.Opcode << " types=(";
      for (unsigned I = 0; I != Q.Types.size(); ++I) {
        if (I)
          OS << ", ";
        Q.Types[I].print(OS);
      }
      OS << ")";
      OutsideLog.push_back(OS.str());
    }
    // Out of domain is not the same as out of the questions the rules are
    // written for: s128 where the rules name s1..s64 is the same kind the
    // table already answers, while p0 in a scalar slot is not.  Counted
    // apart, because a frozen table has to cover the first and cannot cover
    // the second.
    bool KindOk = true;
    for (unsigned K = 0; K != D.NumSlots; ++K) {
      if (D.Slots[K] < 0)
        continue;
      const unsigned C = K == 0 ? static_cast<unsigned>(C0)
                                : (K == 1 ? static_cast<unsigned>(C1) : 0u);
      const uint8_t Kind = X86TypeClasses[C].Kind;
      bool Found = false;
      for (unsigned I = 0; I != Dom.Counts[K]; ++I)
        if (X86TypeClasses[Dom.Classes[K][I]].Kind == Kind) {
          Found = true;
          break;
        }
      if (!Found) {
        KindOk = false;
        break;
      }
    }
    if (!KindOk) {
      ++OutOfKind;
      if (LogOutsides && OutsideLog.size() < 4096) {
        std::string S;
        raw_string_ostream OS(S);
        OS << "x86-gisel-table: out-of-kind table=" << Table
           << " opcode=" << Q.Opcode << " types=(";
        for (unsigned I = 0; I != Q.Types.size(); ++I) {
          if (I)
            OS << ", ";
          Q.Types[I].print(OS);
        }
        OS << ")";
        OutsideLog.push_back(OS.str());
      }
    }
  }

  // The answer was computed at generation time from the same rules; this is one
  // array load, not a walk over the rule chain.
  const size_t Cell =
      (static_cast<size_t>(L[0]) * D.Dims[1] + L[1]) * D.Dims[2] + L[2];
  const uint16_t CellID = D.Cells[Cell];
  if (!CellID) {
    // The generator declined this cell: an inline opaque predicate, or a
    // mutation the chain itself cannot apply.  Not an answer, and not an
    // out-of-kind query either -- the kind was modelled, the cell is empty.
    ++OutOfDomain;
    noteOutside(7, Table, Q);
    R.Kind = Answer::Outside;
    R.Why = 7;
    return R;
  }
  const uint16_t AnswerID = PlanAnswer[CellID - 1];
  if (AnswerID == kNoAnswer) {
    // The matched rule's answer could not be computed when the table was
    // generated.  Falling back to the chain is the only honest option.
    ++OutOfDomain;
    noteOutside(8, Table, Q);
    R.Kind = Answer::Outside;
    R.Why = 8;
    return R;
  }
  const LegalizerAnswer &A = X86Answers[AnswerID];
  ++Hits;
  R.Kind = Answer::Hit;
  R.Action = A.Action;
  R.TypeIdx = A.TypeIdx;
  R.NewType = answerType(A.NewType);
  return R;
}

bool X86LegalizerTable::lookup(const LegalityQuery &Q,
                               LegalizeActionStep &Step) const {
  const Result R = query(Q);
  if (R.Kind != Answer::Hit)
    return false;
  Step = LegalizeActionStep(R.Action, R.TypeIdx, R.NewType);
  return true;
}

X86LegalizerTable::~X86LegalizerTable() {
  if (!X86GISelTableStats)
    return;
  errs() << "x86-gisel-table: hit " << Hits << " outside " << Outsides
         << " ptrvec " << PointerVectors << " addrspace " << AddressSpaces
         << " untabulated " << Untabulated << " declined " << Declined
         << " outofdomain " << OutOfDomain << " outofkind " << OutOfKind
         << "\n";
  for (const std::string &S : OutsideLog)
    errs() << S << "\n";
}

namespace {

StringRef answerName(X86LegalizerTable::Answer A) {
  switch (A) {
  case X86LegalizerTable::Answer::Hit:
    return "hit";
  case X86LegalizerTable::Answer::Outside:
    return "outside";
  case X86LegalizerTable::Answer::PointerVector:
    return "ptrvec";
  case X86LegalizerTable::Answer::Untabulated:
    return "untabulated";
  case X86LegalizerTable::Answer::Declined:
    return "declined";
  }
  return "?";
}

StringRef orderName(AtomicOrdering O) {
  switch (O) {
  case AtomicOrdering::NotAtomic:
    return "notatomic";
  case AtomicOrdering::Unordered:
    return "unordered";
  case AtomicOrdering::Monotonic:
    return "monotonic";
  case AtomicOrdering::Acquire:
    return "acquire";
  case AtomicOrdering::Release:
    return "release";
  case AtomicOrdering::AcquireRelease:
    return "acqrel";
  case AtomicOrdering::SequentiallyConsistent:
    return "seqcst";
  }
  return "?";
}

void printTypes(raw_ostream &OS, ArrayRef<LLT> Types) {
  if (Types.empty()) {
    OS << "-";
    return;
  }
  for (unsigned I = 0; I != Types.size(); ++I) {
    if (I)
      OS << ",";
    Types[I].print(OS);
  }
}

} // namespace

void X86LegalizerTable::forEachPoint(
    function_ref<void(StringRef Name, unsigned Entry, ArrayRef<unsigned> Local,
                      ArrayRef<LLT> Types, LLT MemTy)>
        F) const {
  // The domain is generated with the tables: each keyed slot lists the classes
  // its rules name for it, so the check asks the questions the IR can produce.
  // Iterating the full class set instead would crash the chain on queries it
  // is not written for (a pointer in a scalar slot, for one), which measures
  // nothing about the table.
  static_assert(X86NumTableDomains == X86NumTables,
                "every table needs a self-check domain");

  for (unsigned EntryIdx = 0; EntryIdx != X86NumTableOpcodes; ++EntryIdx) {
    const auto &Entry = X86TableOpcodes[EntryIdx];
    const LegalizerTableDesc &D = X86Tables[Entry.Table];
    const LegalizerTableDomain &Domain = X86TableDomains[Entry.Table];
    // The domain the tables were built for is the classes the rules name;
    // asking the chain about anything else is a question it is not written
    // for.  -x86-gisel-table-check-full measures exactly how far that is
    // true: it walks every class the key can produce.
    const unsigned N0 = D.NumSlots > 0
                            ? (X86GISelTableCheckFull ? X86NumTypeClasses
                                                      : Domain.Counts[0])
                            : 1;
    const unsigned N1 = D.NumSlots > 1
                            ? (X86GISelTableCheckFull ? X86NumTypeClasses
                                                      : Domain.Counts[1])
                            : 1;
    const unsigned N2 = D.NumSlots > 2
                            ? (X86GISelTableCheckFull ? X86NumTypeClasses
                                                      : Domain.Counts[2])
                            : 1;

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
          for (unsigned K = 0; K != D.NumSlots; ++K) {
            if (D.Slots[K] < 0)
              MemBits = static_cast<unsigned>(D.Mem[Local[K]]);
            else
              Types[D.Slots[K]] = representative(
                  X86GISelTableCheckFull
                      ? static_cast<unsigned>(Local[K])
                      : static_cast<unsigned>(Domain.Classes[K][Local[K]]));
          }

          // A type slot's class is exactly one type, so its representative is
          // the whole cell.  The mem axis is keyed by access size, so its cell
          // holds every LLT of that size: asking only the scalar would leave
          // the half of the cell the key actually merges untested.
          SmallVector<LLT, 16> MemTypes;
          if (MemBits)
            memFiber(MemBits, MemTypes);
          else
            MemTypes.push_back(LLT());

          for (LLT MemTy : MemTypes)
            F(Entry.Name, EntryIdx, Local, Types, MemTy);
        }
      }
    }
  }
}

void X86LegalizerTable::selfCheck(raw_ostream &OS) {
  uint64_t Points = 0;
  uint64_t Compared = 0;
  uint64_t Skipped = 0;
  unsigned Mismatches = 0;

  forEachPoint([&](StringRef Name, unsigned Entry, ArrayRef<unsigned>,
                   ArrayRef<LLT> Types, LLT MemTy) {
    SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
    if (MemTy.isValid())
      MemDescs.emplace_back(MemTy, /*AlignInBits=*/1,
                            AtomicOrdering::NotAtomic,
                            AtomicOrdering::NotAtomic);

    ++Points;
    if (X86GISelTableCheckTrace) {
      errs() << "x86-gisel-table-check: ask " << Name << "(";
      for (unsigned I = 0; I != Types.size(); ++I) {
        if (I)
          errs() << ", ";
        Types[I].print(errs());
      }
      if (MemTy.isValid()) {
        errs() << " mem ";
        MemTy.print(errs());
      }
      errs() << ")\n";
    }
    const LegalityQuery Q(X86TableOpcodes[Entry].Opcode, Types, MemDescs);
    // Ask the rules directly.  LI.getAction() consults the installed table
    // first, which would compare the table against itself; the point of this
    // check is the chain.
    const LegalizeActionStep ChainStep =
        LI.getActionDefinitions(Q.Opcode).apply(Q);
    const Result R = query(Q);
    if (R.Kind != Answer::Hit) {
      ++Skipped;
      return;
    }
    ++Compared;
    if (R.Action == ChainStep.Action && R.TypeIdx == ChainStep.TypeIdx &&
        R.NewType == ChainStep.NewType)
      return;
    ++Mismatches;
    if (Mismatches <= 20) {
      OS << "x86-gisel-table-check: MISMATCH " << Name << "(";
      for (unsigned I = 0; I != Types.size(); ++I) {
        if (I)
          OS << ", ";
        Types[I].print(OS);
      }
      if (MemTy.isValid()) {
        OS << " mem ";
        MemTy.print(OS);
      }
      OS << "): table ";
      OS << static_cast<unsigned>(R.Action) << "/" << R.TypeIdx << "/";
      R.NewType.print(OS);
      OS << " chain ";
      OS << static_cast<unsigned>(ChainStep.Action) << "/"
         << ChainStep.TypeIdx << "/";
      ChainStep.NewType.print(OS);
      OS << "\n";
    }
  });

  OS << "x86-gisel-table-check: points " << Points << " compared " << Compared
     << " mismatches " << Mismatches << " skipped " << Skipped << " outside "
     << Outsides << " ptrvec " << PointerVectors << " addrspace "
     << AddressSpaces << " untabulated " << Untabulated << " declined "
     << Declined << " outofdomain " << OutOfDomain << " outofkind "
     << OutOfKind << "\n";
  if (Mismatches)
    report_fatal_error(
        "x86-gisel-table-check: the table and the chain disagree");
}

void X86LegalizerTable::dumpAnswers(raw_ostream &OS, StringRef CPU,
                                    StringRef Triple, StringRef Features,
                                    StringRef FeatureNames) const {
  // The key ignores both alignment and atomic ordering.  Each memdesc point is
  // therefore written once per variant: if the chain answers every variant the
  // same, the key is not dropping a distinction; if it does not, the rows say
  // which variant, which is the gap.
  static constexpr uint64_t Aligns[] = {1, 8, 16, 32, 64, 128, 256};
  static constexpr AtomicOrdering Orders[] = {AtomicOrdering::NotAtomic,
                                              AtomicOrdering::Monotonic,
                                              AtomicOrdering::AcquireRelease};

  // The rows are buffered: the file starts with the counts, and a count is
  // only known once every point has been asked.
  std::string Body;
  raw_string_ostream BodyOS(Body);

  uint64_t DomainPoints = 0, Rows = 0, Mismatches = 0;
  uint64_t Kinds[5] = {0, 0, 0, 0, 0};
  uint64_t Whys[7] = {0, 0, 0, 0, 0, 0, 0};

  forEachPoint([&](StringRef Name, unsigned Entry, ArrayRef<unsigned> Local,
                   ArrayRef<LLT> Types, LLT MemTy) {
    ++DomainPoints;

    SmallVector<std::pair<uint64_t, AtomicOrdering>, 24> Variants;
    if (MemTy.isValid()) {
      for (uint64_t Align : Aligns)
        for (AtomicOrdering O : Orders)
          Variants.push_back({Align, O});
    } else {
      Variants.push_back({0, AtomicOrdering::NotAtomic});
    }

    for (const auto &V : Variants) {
      SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
      if (MemTy.isValid()) {
        // A valid MMO never has a weaker failure ordering than its success
        // ordering; the weakest pair is (Monotonic, Monotonic).
        const AtomicOrdering Fail =
            V.second == AtomicOrdering::NotAtomic ? AtomicOrdering::NotAtomic
                                                  : AtomicOrdering::Monotonic;
        MemDescs.emplace_back(MemTy, V.first, V.second, Fail);
      }

      const LegalityQuery Q(X86TableOpcodes[Entry].Opcode, Types, MemDescs);
      const LegalizeActionStep ChainStep =
          LI.getActionDefinitions(Q.Opcode).apply(Q);
      const Result R = query(Q);

      const bool Match = R.Kind == Answer::Hit &&
                         R.Action == ChainStep.Action &&
                         R.TypeIdx == ChainStep.TypeIdx &&
                         R.NewType == ChainStep.NewType;
      ++Rows;
      ++Kinds[static_cast<unsigned>(R.Kind)];
      ++Whys[R.Why];
      if (R.Kind == Answer::Hit && !Match)
        ++Mismatches;

      BodyOS << Name << '\t'
             << static_cast<unsigned>(X86TableOpcodes[Entry].Table) << '\t'
             << Local[0] << '\t' << Local[1] << '\t' << Local[2] << '\t';
      printTypes(BodyOS, Types);
      BodyOS << '\t';
      if (MemTy.isValid())
        MemTy.print(BodyOS);
      else
        BodyOS << "-";
      BodyOS << '\t' << V.first << '\t' << orderName(V.second) << '\t'
             << (V.second == AtomicOrdering::NotAtomic ? "notatomic"
                                                       : "monotonic")
             << '\t';

      BodyOS << ChainStep.Action << '\t' << ChainStep.TypeIdx << '\t';
      if (ChainStep.NewType.isValid())
        ChainStep.NewType.print(BodyOS);
      else
        BodyOS << "-";

      BodyOS << '\t' << answerName(R.Kind) << '\t' << R.Why << '\t';
      if (R.Kind == Answer::Hit) {
        BodyOS << R.Action << '\t' << R.TypeIdx << '\t';
        if (R.NewType.isValid())
          R.NewType.print(BodyOS);
        else
          BodyOS << "-";
      } else {
        BodyOS << "-\t-\t-";
      }
      BodyOS << '\t'
             << (R.Kind != Answer::Hit ? "skip"
                                       : (Match ? "ok" : "mismatch"))
             << '\n';
    }
  });
  BodyOS.flush();

  OS << "# x86-gisel-legalizer-dump\n";
  OS << "# triple " << Triple << "\n";
  OS << "# cpu " << CPU << "\n";
  OS << "# features " << Features << "\n";
  OS << "# feature_names " << FeatureNames << "\n";
  OS << "# pointer_bits " << PointerBits << "\n";
  OS << "# tables " << X86NumTables << "\n";
  OS << "# gaps " << X86NumTableGaps << "\n";
  for (unsigned I = 0; I != X86NumTableGaps; ++I)
    OS << "# gap " << X86TableGaps[I].Name << " "
       << (X86TableGaps[I].Why == 1 ? "escape" : "unkeyable") << "\n";
  for (unsigned I = 0; I != X86NumTables; ++I) {
    const LegalizerTableDesc &D = X86Tables[I];
    const LegalizerTableDomain &Domain = X86TableDomains[I];
    OS << "# table " << I << " slots";
    for (unsigned K = 0; K != D.NumSlots; ++K)
      OS << " " << static_cast<int>(D.Slots[K]);
    OS << " dims " << static_cast<unsigned>(D.Dims[0]) << " "
       << static_cast<unsigned>(D.Dims[1]) << " "
       << static_cast<unsigned>(D.Dims[2]) << " domain "
       << static_cast<unsigned>(Domain.Counts[0]) << " "
       << static_cast<unsigned>(Domain.Counts[1]) << " "
       << static_cast<unsigned>(Domain.Counts[2]) << " atoms "
       << static_cast<unsigned>(D.NumAtoms) << "\n";
  }
  OS << "# columns opcode table l0 l1 l2 types mem align order failorder "
        "chain_action chain_idx chain_type table_kind why table_action "
        "table_idx table_type match\n";
  // The last line of the header is the whole answer in one place: how many
  // points, how many rows, how each row was answered, and which axis refused
  // the ones that were refused.  A reader that only wants the counts can stop
  // here.
  OS << "# summary domain_points " << DomainPoints << " rows " << Rows
     << " hit " << Kinds[static_cast<unsigned>(Answer::Hit)] << " outside "
     << Kinds[static_cast<unsigned>(Answer::Outside)] << " ptrvec "
     << Kinds[static_cast<unsigned>(Answer::PointerVector)] << " untabulated "
     << Kinds[static_cast<unsigned>(Answer::Untabulated)] << " declined "
     << Kinds[static_cast<unsigned>(Answer::Declined)] << " mismatch "
     << Mismatches;
  for (unsigned W = 1; W != 7; ++W)
    OS << " why" << W << " " << Whys[W];
  OS << "\n";
  OS << Body;
}
