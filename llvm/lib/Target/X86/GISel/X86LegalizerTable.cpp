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
constexpr uint16_t kOpaqueCell = 0xFFFF;

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
  // Access size in bits for the memory axis, or 0 when the point asks no
  // memory question.  A load's type does not say how wide the access is.
  uint32_t MemBits;
};

} // namespace

#include "X86LegalizerTablePoints.inc"

X86LegalizerTable::X86LegalizerTable(const LegalizerInfo &LI)
    : LI(LI), Cells(X86NumTables), LogOutsides(X86GISelTableOutsides) {}

LLT X86LegalizerTable::representative(unsigned ClassID) {
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

uint16_t X86LegalizerTable::fill(unsigned Table, const LegalityQuery &Q,
                                 ArrayRef<unsigned> Local) const {
  const LegalizerTableDesc &D = X86Tables[Table];
  SmallVector<LLT, 4> Types(Q.Types.begin(), Q.Types.end());
  SmallVector<LegalityQuery::MemDesc, 1> MemDescs(Q.MMODescrs.begin(),
                                                  Q.MMODescrs.end());
  for (unsigned K = 0; K != D.NumSlots; ++K) {
    const int Slot = D.Slots[K];
    if (Slot < 0) {
      // The mem axis stands for one access size, so the representative is a
      // type of exactly that size.  Alignment is not in the key: every rule
      // asks for "at least" alignment, so the weakest alignment answers for
      // all of them, which is also what the key generator asserts.
      MemDescs.assign(1, LegalityQuery::MemDesc(
                             LLT::scalar(static_cast<unsigned>(D.Mem[Local[K]])),
                             /*AlignInBits=*/1, AtomicOrdering::NotAtomic,
                             AtomicOrdering::NotAtomic));
      continue;
    }
    Types[Slot] = representative(localToGlobal(D, K, Local[K]));
  }

  // Same question, asked with the class representatives instead of the actual
  // types: that is what makes one cell stand for the whole class pair.  The
  // rules are asked directly -- going through LI.getAction() would re-enter
  // this table and recurse.
  LegalizeActionStep Step =
      LI.getActionDefinitions(Q.Opcode)
          .apply(LegalityQuery(Q.Opcode, Types, MemDescs, Q.Immediates));
  ++Fills;
  Steps.push_back(Step);
  return static_cast<uint16_t>(Steps.size());
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
  int C1 = 0;
  if (D.Dims[1] > 1)
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
  if (D.Dims[1] > 1) {
    L[1] = D.Local[1][C1];
    if (L[1] == 0xFF) {
      ++Outsides;
      noteOutside(3, Table, Q);
      R.Kind = Answer::Outside;
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
      return R;
    }
    L[2] = static_cast<unsigned>(MemLocal);
  }

  std::vector<uint16_t> &Row = Cells[Table];
  if (Row.empty())
    Row.assign(static_cast<size_t>(D.Dims[0]) * D.Dims[1] * D.Dims[2], 0);
  uint16_t &Cell =
      Row[(static_cast<size_t>(L[0]) * D.Dims[1] + L[1]) * D.Dims[2] + L[2]];
  if (!Cell)
    Cell = fill(Table, Q, L);

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
         << " addrspace " << AddressSpaces << " untabulated " << Untabulated
         << " declined " << Declined << " fills " << Fills << "\n";
  for (const std::string &S : OutsideLog)
    errs() << S << "\n";
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
    // The point carries the access size it was enumerated for; 0 means the
    // question has no memory part.  Both paths then see the same query, which
    // is the only way the comparison means anything.
    SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
    if (P.MemBits)
      MemDescs.emplace_back(LLT::scalar(P.MemBits), /*AlignInBits=*/1,
                            AtomicOrdering::NotAtomic,
                            AtomicOrdering::NotAtomic);
    const LegalityQuery Q(P.Opcode, Types, MemDescs);

    // Ask the rules directly.  LI.getAction() consults the installed table
    // first, which would compare the table against itself when both switches
    // are on; the point of this check is the chain.
    const LegalizeActionStep ChainStep =
        LI.getActionDefinitions(Q.Opcode).apply(Q);
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
      OS << "x86-gisel-table-check: MISMATCH " << P.Name << "(mem "
         << P.MemBits << ")(";
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
     << " outside " << Outsides << " ptrvec " << PointerVectors << " addrspace "
     << AddressSpaces << " untabulated " << Untabulated << " declined "
     << Declined << " fills " << Fills << "\n";
  if (Mismatches)
    report_fatal_error("x86-gisel-table-check: the table and the chain disagree");
}
