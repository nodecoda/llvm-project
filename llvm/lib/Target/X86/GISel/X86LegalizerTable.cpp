//===-- X86LegalizerTable.cpp -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "X86LegalizerTable.h"
#include "X86Subtarget.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <optional>

using namespace llvm;
// The generated evaluators are the rule chain written out: they name the
// actions, the predicates and the mutations the C++ builders would have used.
using namespace llvm::LegalizeActions;
using namespace llvm::LegalityPredicates;
using namespace llvm::LegalizeMutations;

static cl::opt<bool>
    X86GISelTableStats("x86-gisel-table-stats", cl::Hidden, cl::init(false),
                       cl::desc("Report how the X86 legalizer table answered"));

static cl::opt<bool> X86GISelTableResiduals(
    "x86-gisel-table-residuals", cl::Hidden, cl::init(false),
    cl::desc("Report which queries fall outside the X86 legalizer table"));

namespace {

enum LegalizerTypeClassKind : uint8_t {
  Kind_Pointer = 0,
  Kind_Scalar = 1,
  Kind_Vector = 2,
  /// A vector whose element type is a pointer.  It is a distinct LLT from an
  /// integer vector of the same shape, so both axes keep it as its own class.
  Kind_PtrVector = 3,
};

struct LegalizerTypeClass {
  uint8_t Kind;
  uint16_t ElemBits;
  uint16_t Lanes;
};

} // namespace

/// A rule set whose key has no memory-descriptor axis.
constexpr uint8_t kNoMemDesc = 0xFF;

// The generated file fills in the classes and the per-opcode descriptors; the
// shape of a descriptor is part of the reader, not of the generator.
struct LegalizerTableDesc {
  uint8_t Slots[2]; // which type slots the key reads
  uint8_t Dims[2];  // class count per key slot
  uint8_t MemSlot;  // which MMODescr the key reads, or kNoMemDesc
  uint16_t MemDim;  // class count of the descriptor axis (0 without one)
};

#include "X86LegalizerTable.inc"

namespace {

constexpr int kUnknownClass = -1;
constexpr int kPointerVectorClass = -2;

/// The class arrays are emitted in (Kind, ElemBits, Lanes) order, so a
/// classification is a lower_bound instead of a walk over every class.  The
/// static_assert keeps that invariant honest: a generator change that
/// reorders the arrays fails the build instead of misclassifying a type.
constexpr bool lessClass(const LegalizerTypeClass &A,
                         const LegalizerTypeClass &B) {
  if (A.Kind != B.Kind)
    return A.Kind < B.Kind;
  if (A.ElemBits != B.ElemBits)
    return A.ElemBits < B.ElemBits;
  return A.Lanes < B.Lanes;
}

template <unsigned N>
constexpr bool sortedClasses(const LegalizerTypeClass (&Classes)[N]) {
  for (unsigned I = 1; I < N; ++I)
    if (!lessClass(Classes[I - 1], Classes[I]))
      return false;
  return true;
}

static_assert(sortedClasses(X86TypeClasses),
              "X86TypeClasses must be sorted by (Kind, ElemBits, Lanes)");
static_assert(sortedClasses(X86MemClasses),
              "X86MemClasses must be sorted by (Kind, ElemBits, Lanes)");

template <unsigned N>
int classOf(const LegalizerTypeClass (&Classes)[N], uint8_t Kind,
            unsigned Bits, unsigned Lanes) {
  const LegalizerTypeClass Key{Kind, static_cast<uint16_t>(Bits),
                               static_cast<uint16_t>(Lanes)};
  const auto *End = Classes + N;
  const auto *It = std::lower_bound(Classes, End, Key, lessClass);
  if (It != End && It->Kind == Key.Kind && It->ElemBits == Key.ElemBits &&
      It->Lanes == Key.Lanes)
    return static_cast<int>(It - Classes);
  return kUnknownClass;
}

constexpr uint32_t kOpaqueCell = 0xFFFFFFFFu;

/// Class of a type, or kUnknownClass / kPointerVectorClass.
int classifyType(LLT T) {
  if (!T.isValid())
    return kUnknownClass;
  // A vector of pointers is a different LLT from a vector of integers of the
  // same shape: kind 3 keeps them apart.  A shape the class list does not name
  // is refused rather than merged with an integer vector.
  uint8_t Kind;
  unsigned Bits = 0;
  unsigned Lanes = 1;
  if (T.isPointerVector()) {
    if (T.isScalableVector())
      return kUnknownClass;
    Kind = Kind_PtrVector;
    Lanes = T.getNumElements();
  } else if (T.isPointer()) {
    // There is one pointer class per LegalizerInfo instance.  Its width is
    // fixed for that instance, so it must not be part of the key: p0 is the
    // same class whether the subtarget is 32- or 64-bit.
    return X86TypeClasses[0].Kind == Kind_Pointer ? 0 : kUnknownClass;
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
  const int Idx = classOf(X86TypeClasses, Kind, Bits, Lanes);
  if (Idx >= 0)
    return Idx;
  return Kind == Kind_PtrVector ? kPointerVectorClass : kUnknownClass;
}

/// Class of a memory descriptor's LLT, or kUnknownClass.  This axis keeps the
/// LLT's identity, including a vector of pointers: G_STORE asks whether the
/// value type *is* the memory type, so a shape-only class would merge types
/// the rules distinguish.
int classifyMemType(LLT T) {
  if (!T.isValid())
    return kUnknownClass;
  uint8_t Kind;
  unsigned Bits = 0;
  unsigned Lanes = 1;
  if (T.isPointerVector()) {
    if (T.isScalableVector())
      return kUnknownClass;
    Kind = Kind_PtrVector;
    Lanes = T.getNumElements();
  } else if (T.isPointer()) {
    Kind = Kind_Pointer;
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
  return classOf(X86MemClasses, Kind, Bits, Lanes);
}

/// A pointer whose address space is not zero is outside the table's pointer
/// class.  The key folds every pointer to p0, and a rule such as G_STORE's
/// `Types[0] != MemoryTy` compares exactly what that folding erases, so such
/// a query must be answered by the evaluator with its concrete LLTs.
bool hasNonZeroAddressSpace(LLT T) {
  if (T.isPointer())
    return T.getAddressSpace() != 0;
  if (T.isPointerVector())
    return T.getElementType().getAddressSpace() != 0;
  return false;
}

/// Alignment class: how many of the generated thresholds the alignment meets.
/// The memdesc predicates only ask `Align >= threshold`, so the prefix is the
/// whole answer.
unsigned alignClass(uint64_t Align) {
  unsigned N = 0;
  while (N < X86NumAlignClasses - 1 && Align >= X86AlignThresholds[N])
    ++N;
  return N;
}

struct X86TablePoint {
  unsigned Opcode;
  const char *Name;
  uint8_t NumTypes;
  LLT Types[3];
  LLT MemTy; // valid when HasMem
  uint64_t Align;
  bool HasMem;
};

} // namespace

#include "X86LegalizerTablePoints.inc"

X86LegalizerTable::X86LegalizerTable(const LegalizerInfo &LI,
                                     const X86Subtarget &Subtarget,
                                     unsigned PointerBits)
    : LI(LI), Subtarget(Subtarget), PointerBits(PointerBits),
      Cells(X86NumTables) {}

LLT X86LegalizerTable::representative(unsigned ClassID) const {
  const LegalizerTypeClass &C = X86TypeClasses[ClassID];
  switch (C.Kind) {
  case Kind_Pointer:
    return LLT::pointer(0, PointerBits);
  case Kind_Vector:
    return LLT::fixed_vector(C.Lanes, C.ElemBits);
  case Kind_PtrVector:
    return LLT::fixed_vector(C.Lanes, LLT::pointer(0, PointerBits));
  default:
    return LLT::scalar(C.ElemBits);
  }
}

LLT X86LegalizerTable::memRepresentative(unsigned ClassID) const {
  const LegalizerTypeClass &C = X86MemClasses[ClassID];
  switch (C.Kind) {
  case Kind_Pointer:
    return LLT::pointer(0, PointerBits);
  case Kind_Vector:
    return LLT::fixed_vector(C.Lanes, C.ElemBits);
  case Kind_PtrVector:
    return LLT::fixed_vector(C.Lanes, LLT::pointer(0, PointerBits));
  default:
    return LLT::scalar(C.ElemBits);
  }
}

uint32_t X86LegalizerTable::fill(unsigned Table, const LegalityQuery &Q,
                                 unsigned L0, unsigned L1,
                                 unsigned MemClass) const {
  const LegalizerTableDesc &D = X86Tables[Table];
  SmallVector<LLT, 4> Types(Q.Types.begin(), Q.Types.end());
  // A zero-type query (G_BR) keeps its empty type list: the rule set cannot
  // read a slot that the query does not have, so the representative would be
  // answering on behalf of an operand that does not exist.
  if (Types.size() > D.Slots[0])
    Types[D.Slots[0]] = representative(L0);
  if (D.Dims[1] > 1 && Types.size() > D.Slots[1])
    Types[D.Slots[1]] = representative(L1);

  SmallVector<LegalityQuery::MemDesc, 1> MemDescs(Q.MMODescrs.begin(),
                                                  Q.MMODescrs.end());
  if (D.MemDim) {
    if (D.MemSlot >= MemDescs.size())
      return kOpaqueCell;
    const unsigned Align = MemClass % X86NumAlignClasses;
    const LLT MemTy = memRepresentative(MemClass / X86NumAlignClasses);
    const uint64_t AlignInBits = Align ? X86AlignThresholds[Align - 1] : 0;
    const LegalityQuery::MemDesc &Src = MemDescs[D.MemSlot];
    MemDescs[D.MemSlot] = LegalityQuery::MemDesc(
        MemTy, AlignInBits, Src.Ordering, Src.FailureOrdering);
  }

  // Same question, asked with the class representatives instead of the actual
  // types: that is what makes one cell stand for the whole class tuple.  The
  // answer comes from the rule set's generated evaluator, not from
  // LegalizeRuleSet::apply(): the table is a compilation of the rules, so a
  // hit costs one switch, one classify per key slot and one call, with no
  // comparison chain behind it.  Going through LI.getAction() would also
  // re-enter this table and recurse.
  std::optional<LegalizeActionStep> Step = X86EvalTable(
      Table, Subtarget, LegalityQuery(Q.Opcode, Types, MemDescs, Q.Immediates));
  if (!Step)
    return kOpaqueCell;
  ++Fills;
  Steps.push_back(*Step);
  if (Steps.size() >= static_cast<size_t>(kOpaqueCell))
    return kOpaqueCell;
  return static_cast<uint32_t>(Steps.size());
}

X86LegalizerTable::Result
X86LegalizerTable::query(const LegalityQuery &Q) const {
  Result R;
  const int Table = x86TableForOpcode(Q.Opcode);
  if (Table < 0) {
    ++Untabulated;
    if (X86GISelTableResiduals)
      ++UntabulatedOps[Q.Opcode];
    return R;
  }
  const LegalizerTableDesc &D = X86Tables[Table];

  // A type outside the lattice is not a failure: the evaluator is a function of
  // the query, so it can answer the concrete types directly.  This is the
  // residual path; there is no rule chain behind it.
  // Why a query left the lattice.  Only collected under the diagnostic flag:
  // the answer itself never depends on this.
  auto RecordKey = [&](LLT T) {
    if (!X86GISelTableResiduals || !T.isValid())
      return;
    uint8_t Kind = 0;
    unsigned Bits = 0, Lanes = 1;
    if (T.isPointer()) {
      Kind = Kind_Pointer;
    } else if (T.isPointerVector()) {
      Kind = Kind_PtrVector;
      Lanes = T.isFixedVector() ? T.getNumElements() : 0;
    } else if (T.isVector()) {
      Kind = Kind_Vector;
      Bits = T.getScalarSizeInBits();
      Lanes = T.getNumElements();
    } else if (T.isScalar()) {
      Kind = Kind_Scalar;
      Bits = T.getSizeInBits().getFixedValue();
    }
    ++ResidualKeys[(uint64_t(Kind) << 32) | (uint64_t(Bits) << 8) | Lanes];
  };

  auto AskEvaluator = [&](bool PointerVector) -> Result {
    std::optional<LegalizeActionStep> Step = X86EvalTable(Table, Subtarget, Q);
    if (!Step) {
      ++Escapes;
      R.Kind = Answer::Escape;
      return R;
    }
    ++Residuals;
    if (PointerVector)
      ++PointerVectors;
    R.Kind = Answer::Residual;
    R.Action = Step->Action;
    R.TypeIdx = Step->TypeIdx;
    R.NewType = Step->NewType;
    return R;
  };

  // The key has one pointer class, so it cannot see the address space -- and
  // G_STORE's `Types[0] != MemoryTy` guard is a comparison of two pointers.
  // Ask the evaluator with the concrete LLTs instead: the residual path does
  // not go through a cell, so nothing is folded.
  bool PointerVector = false;
  bool NonZeroAddressSpace = false;
  auto ScanAddressSpace = [&](LLT T) {
    PointerVector |= T.isPointerVector();
    NonZeroAddressSpace |= hasNonZeroAddressSpace(T);
  };
  for (LLT T : Q.Types)
    ScanAddressSpace(T);
  for (const LegalityQuery::MemDesc &MD : Q.MMODescrs)
    ScanAddressSpace(MD.MemoryTy);
  if (NonZeroAddressSpace) {
    ++AddressSpaceResiduals;
    return AskEvaluator(PointerVector);
  }

  // A query with no operand at a keyed slot (G_BR and friends) cannot be
  // influenced by that slot: a rule set that read it would be reading past
  // the end of Q.Types.  Class 0 is therefore as good as any, and the cell
  // answers instead of the query falling out to the evaluator.  This is the
  // same convention as slot 1 below.
  const int C0 = Q.Types.size() > D.Slots[0] ? classifyType(Q.Types[D.Slots[0]])
                                             : 0;
  // A unary query has no slot 1.  The descriptor is uniform, so missing slot 1
  // maps to class 0; the rules in a unary rule set cannot read it.
  int C1 = 0;
  if (D.Dims[1] > 1 && Q.Types.size() > D.Slots[1])
    C1 = classifyType(Q.Types[D.Slots[1]]);

  if (C0 == kPointerVectorClass || C1 == kPointerVectorClass) {
    if (X86GISelTableResiduals) {
      if (C0 == kPointerVectorClass && Q.Types.size() > D.Slots[0])
        RecordKey(Q.Types[D.Slots[0]]);
      if (C1 == kPointerVectorClass && Q.Types.size() > D.Slots[1])
        RecordKey(Q.Types[D.Slots[1]]);
    }
    return AskEvaluator(/*PointerVector=*/true);
  }
  if (C0 < 0 || C1 < 0) {
    if (X86GISelTableResiduals) {
      if (C0 < 0) {
        if (Q.Types.size() > D.Slots[0] && Q.Types[D.Slots[0]].isValid())
          RecordKey(Q.Types[D.Slots[0]]);
        else {
          ++ResidualNoType;
          if (X86GISelTableResiduals)
            ++ResidualNoTypeOps[Q.Opcode];
        }
      }
      if (C1 < 0 && Q.Types.size() > D.Slots[1] &&
          Q.Types[D.Slots[1]].isValid())
        RecordKey(Q.Types[D.Slots[1]]);
    }
    return AskEvaluator(/*PointerVector=*/false);
  }

  const uint8_t L0 = C0;
  const uint8_t L1 = D.Dims[1] > 1 ? C1 : 0;

  unsigned MemClass = 0;
  if (D.MemDim) {
    if (D.MemSlot >= Q.MMODescrs.size()) {
      ++Outsides;
      R.Kind = Answer::Outside;
      return R;
    }
    const LegalityQuery::MemDesc &MD = Q.MMODescrs[D.MemSlot];
    const int CM = classifyMemType(MD.MemoryTy);
    if (CM < 0) {
      RecordKey(MD.MemoryTy);
      return AskEvaluator(/*PointerVector=*/false);
    }
    MemClass = CM * X86NumAlignClasses + alignClass(MD.AlignInBits);
  }

  const size_t MemDim = D.MemDim ? D.MemDim : 1;
  std::vector<uint32_t> &Row = Cells[Table];
  if (Row.empty())
    Row.assign(static_cast<size_t>(D.Dims[0]) * D.Dims[1] * MemDim, 0);
  uint32_t &Cell =
      Row[(static_cast<size_t>(L0) * D.Dims[1] + L1) * MemDim + MemClass];
  if (!Cell)
    Cell = fill(Table, Q, L0, L1, MemClass);

  if (Cell == kOpaqueCell) {
    ++Escapes;
    R.Kind = Answer::Escape;
    return R;
  }

  const LegalizeActionStep &S = Steps[Cell - 1];
  ++Hits;
  R.Kind = Answer::Hit;
  R.Action = S.Action;
  R.TypeIdx = S.TypeIdx;
  R.NewType = S.NewType;
  return R;
}

uint32_t X86LegalizerTable::signature() const {
  return X86FeatureSignature(Subtarget);
}

bool X86LegalizerTable::lookup(const LegalityQuery &Q,
                               LegalizeActionStep &Step) const {
  const Result R = query(Q);
  if (R.Kind != Answer::Hit && R.Kind != Answer::Residual)
    return false;
  Step = LegalizeActionStep(R.Action, R.TypeIdx, R.NewType);
  return true;
}

X86LegalizerTable::~X86LegalizerTable() {
  if (!X86GISelTableStats && !X86GISelTableResiduals)
    return;
  if (X86GISelTableStats)
    errs() << "x86-gisel-table: signature " << format_hex(signature(), 0)
         << " hit " << Hits << " residual " << Residuals
         << " escape " << Escapes << " outside " << Outsides << " ptrvec "
           << PointerVectors << " addrspace " << AddressSpaceResiduals
           << " untabulated " << Untabulated << " fills " << Fills << "\n";
  if (!X86GISelTableResiduals)
    return;
  errs() << "x86-gisel-table residuals: no-type " << ResidualNoType << "\n";
  SmallVector<std::pair<unsigned, uint64_t>, 16> Ops(ResidualNoTypeOps.begin(),
                                                     ResidualNoTypeOps.end());
  llvm::sort(Ops, [](const auto &A, const auto &B) {
    return A.second > B.second;
  });
  for (const auto &Entry : ArrayRef(Ops).take_front(10))
    errs() << "  no-type " << Entry.second << " x " << Entry.first << "\n";
  SmallVector<std::pair<unsigned, uint64_t>, 8> Un(UntabulatedOps.begin(),
                                                  UntabulatedOps.end());
  llvm::sort(Un, [](const auto &A, const auto &B) {
    return A.second > B.second;
  });
  for (const auto &Entry : ArrayRef(Un).take_front(8))
    errs() << "  untabulated " << Entry.second << " x opcode " << Entry.first
           << "\n";
  SmallVector<std::pair<uint64_t, uint64_t>, 16> Keys(ResidualKeys.begin(),
                                                      ResidualKeys.end());
  llvm::sort(Keys, [](const auto &A, const auto &B) {
    return A.second > B.second;
  });
  for (const auto &Entry : ArrayRef(Keys).take_front(10)) {
    const unsigned Kind = Entry.first >> 32;
    const unsigned Bits = (Entry.first >> 8) & 0xFFFFFF;
    const unsigned Lanes = Entry.first & 0xFF;
    const char *Name = Kind == Kind_Pointer   ? "p"
                       : Kind == Kind_Vector  ? "v"
                       : Kind == Kind_PtrVector ? "vp"
                                               : "s";
    errs() << "  " << Entry.second << " x " << Name;
    if (Kind == Kind_Vector || Kind == Kind_PtrVector)
      errs() << Lanes;
    if (Kind != Kind_Pointer)
      errs() << Bits;
    errs() << "\n";
  }
}

void X86LegalizerTable::selfCheck(raw_ostream &OS) {
  SmallVector<X86TablePoint, 0> Points;
  Points.reserve(X86NumTablePoints);
  x86CollectTablePoints(Points, PointerBits);

  unsigned Compared = 0;
  unsigned Mismatches = 0;
  unsigned Skipped = 0;
  unsigned Residual = 0;
  for (const X86TablePoint &P : Points) {
    ArrayRef<LLT> Types(P.Types, P.NumTypes);
    // A memdesc point carries the descriptor it was generated from, so the
    // chain and the table are asked the same question.
    SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
    if (P.HasMem)
      MemDescs.emplace_back(P.MemTy, P.Align, AtomicOrdering::NotAtomic,
                            AtomicOrdering::NotAtomic);
    const LegalityQuery Q(P.Opcode, Types, MemDescs);

    if (X86GISelTableStats) {
      errs() << "x86-gisel-table-check: " << P.Name << " (";
      for (unsigned I = 0; I != P.NumTypes; ++I) {
        if (I)
          errs() << ", ";
        P.Types[I].print(errs());
      }
      errs() << ")\n";
    }
    // Ask the chain directly: LI.getAction() would consult the installed
    // table first, which is the thing being checked.
    const LegalizeActionStep ChainStep =
        LI.getActionDefinitions(Q.Opcode).apply(Q);
    const Result R = query(Q);
    if (R.Kind != Answer::Hit && R.Kind != Answer::Residual) {
      ++Skipped;
      continue;
    }
    if (R.Kind == Answer::Residual)
      ++Residual;
    ++Compared;
    if (R.Action == ChainStep.Action && R.TypeIdx == ChainStep.TypeIdx &&
        R.NewType == ChainStep.NewType)
      continue;
    ++Mismatches;
    if (Mismatches <= 20) {
      OS << "x86-gisel-table-check: MISMATCH " << P.Name << "(";
      for (unsigned I = 0; I < P.NumTypes; ++I) {
        if (I)
          OS << ", ";
        P.Types[I].print(OS);
      }
      OS << "): table ";
      OS << static_cast<unsigned>(R.Action) << "/" << R.TypeIdx << "/";
      R.NewType.print(OS);
      OS << " chain ";
      OS << static_cast<unsigned>(ChainStep.Action) << "/" << ChainStep.TypeIdx
         << "/";
      ChainStep.NewType.print(OS);
      OS << "\n";
    }
  }

  OS << "x86-gisel-table-check: points " << Points.size() << " compared "
     << Compared << " mismatches " << Mismatches << " skipped " << Skipped
     << " residual " << Residual << " outside " << Outsides << " ptrvec "
     << PointerVectors << " addrspace " << AddressSpaceResiduals
     << " untabulated " << Untabulated << " fills " << Fills << "\n";
  if (Mismatches)
    report_fatal_error(
        "x86-gisel-table-check: the table and the chain disagree");
}
