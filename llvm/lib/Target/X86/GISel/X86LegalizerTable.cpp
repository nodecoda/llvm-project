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

using namespace llvm;

static cl::opt<bool> X86GISelTableStats(
    "x86-gisel-table-stats", cl::Hidden, cl::init(false),
    cl::desc("Report how the X86 legalizer table answered"));

namespace {

enum LegalizerTypeClassKind : uint8_t {
  Kind_Pointer = 0,
  Kind_Scalar = 1,
  Kind_Vector = 2,
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
constexpr uint16_t kOpaqueCell = 0xFFFF;

/// Class of a type, or kUnknownClass / kPointerVectorClass.
int classifyType(LLT T) {
  if (!T.isValid())
    return kUnknownClass;
  // A vector of pointers is a different LLT from a vector of integers of the
  // same shape, and the class list cannot tell them apart yet.  Refuse rather
  // than merge two types the rules distinguish.
  if (T.isPointerVector())
    return kPointerVectorClass;
  uint8_t Kind;
  unsigned Bits;
  unsigned Lanes = 1;
  if (T.isPointer()) {
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
  for (unsigned I = 0; I < X86NumTypeClasses; ++I)
    if (X86TypeClasses[I].Kind == Kind && X86TypeClasses[I].ElemBits == Bits &&
        X86TypeClasses[I].Lanes == Lanes)
      return static_cast<int>(I);
  return kUnknownClass;
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

struct X86TablePoint {
  unsigned Opcode;
  const char *Name;
  uint8_t NumTypes;
  LLT Types[3];
};

} // namespace

#include "X86LegalizerTablePoints.inc"

X86LegalizerTable::X86LegalizerTable(const LegalizerInfo &LI)
    : LI(LI), Cells(X86NumTables) {}

LLT X86LegalizerTable::representative(unsigned ClassID) {
  const LegalizerTypeClass &C = X86TypeClasses[ClassID];
  switch (C.Kind) {
  case Kind_Pointer:
    return LLT::pointer(0, C.ElemBits);
  case Kind_Vector:
    return LLT::fixed_vector(C.Lanes, C.ElemBits);
  default:
    return LLT::scalar(C.ElemBits);
  }
}

uint16_t X86LegalizerTable::fill(unsigned Table, const LegalityQuery &Q,
                                 unsigned L0, unsigned L1) const {
  const LegalizerTableDesc &D = X86Tables[Table];
  SmallVector<LLT, 4> Types(Q.Types.begin(), Q.Types.end());
  Types[D.Slots[0]] = representative(localToGlobal(D, 0, L0));
  if (D.Dims[1] > 1)
    Types[D.Slots[1]] = representative(localToGlobal(D, 1, L1));

  // Same question, asked with the class representatives instead of the actual
  // types: that is what makes one cell stand for the whole class pair.  The
  // rules are asked directly -- going through LI.getAction() would re-enter
  // this table and recurse.
  LegalizeActionStep Step =
      LI.getActionDefinitions(Q.Opcode)
          .apply(LegalityQuery(Q.Opcode, Types, Q.MMODescrs, Q.Immediates));
  ++Fills;
  Steps.push_back(Step);
  return static_cast<uint16_t>(Steps.size());
}

X86LegalizerTable::Result
X86LegalizerTable::query(const LegalityQuery &Q) const {
  Result R;
  const int Table = x86TableForOpcode(Q.Opcode);
  if (Table < 0) {
    ++Untabulated;
    return R;
  }
  // The table keys types only.  A query that also carries a memdesc or an
  // immediate is a different question with the same type key, so the table
  // must not answer it; those axes arrive later and the count says so.
  if (!Q.MMODescrs.empty() || !Q.Immediates.empty()) {
    ++Declined;
    R.Kind = Answer::Declined;
    return R;
  }
  const LegalizerTableDesc &D = X86Tables[Table];

  const int C0 = Q.Types.size() > D.Slots[0]
                     ? classifyType(Q.Types[D.Slots[0]])
                     : kUnknownClass;
  int C1 = 0;
  if (D.Dims[1] > 1)
    C1 = Q.Types.size() > D.Slots[1] ? classifyType(Q.Types[D.Slots[1]])
                                     : kUnknownClass;

  if (C0 == kPointerVectorClass || C1 == kPointerVectorClass) {
    ++PointerVectors;
    R.Kind = Answer::PointerVector;
    return R;
  }
  if (C0 < 0 || C1 < 0) {
    ++Outsides;
    R.Kind = Answer::Outside;
    return R;
  }

  const uint8_t L0 = D.Local[0][C0];
  if (L0 == 0xFF) {
    ++Outsides;
    R.Kind = Answer::Outside;
    return R;
  }
  uint8_t L1 = 0;
  if (D.Dims[1] > 1) {
    L1 = D.Local[1][C1];
    if (L1 == 0xFF) {
      ++Outsides;
      R.Kind = Answer::Outside;
      return R;
    }
  }

  std::vector<uint16_t> &Row = Cells[Table];
  if (Row.empty())
    Row.assign(static_cast<size_t>(D.Dims[0]) * D.Dims[1], 0);
  uint16_t &Cell = Row[static_cast<size_t>(L0) * D.Dims[1] + L1];
  if (!Cell)
    Cell = fill(Table, Q, L0, L1);

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
  errs() << "x86-gisel-table: hit " << Hits << " escape " << Escapes
         << " outside " << Outsides << " ptrvec " << PointerVectors
         << " untabulated " << Untabulated << " declined " << Declined
         << " fills " << Fills << "\n";
}

void X86LegalizerTable::selfCheck(raw_ostream &OS) {
  SmallVector<X86TablePoint, 0> Points;
  Points.reserve(X86NumTablePoints);
  x86CollectTablePoints(Points);

  unsigned Compared = 0;
  unsigned Mismatches = 0;
  unsigned Skipped = 0;
  for (const X86TablePoint &P : Points) {
    ArrayRef<LLT> Types(P.Types, P.NumTypes);
    // The enumerated domain carries no memdesc; the two rule sets that read
    // one get a fixed, sized memory type so both paths see the same question.
    SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
    if (P.Opcode == TargetOpcode::G_LOAD || P.Opcode == TargetOpcode::G_STORE)
      MemDescs.emplace_back(Types[0], 8, AtomicOrdering::NotAtomic,
                            AtomicOrdering::NotAtomic);
    const LegalityQuery Q(P.Opcode, Types, MemDescs);

    const LegalizeActionStep ChainStep = LI.getAction(Q);
    const Result R = query(Q);
    if (R.Kind != Answer::Hit) {
      ++Skipped;
      continue;
    }
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
     << " outside " << Outsides << " ptrvec " << PointerVectors
     << " untabulated " << Untabulated << " declined " << Declined
     << " fills " << Fills << "\n";
  if (Mismatches)
    report_fatal_error("x86-gisel-table-check: the table and the chain disagree");
}
