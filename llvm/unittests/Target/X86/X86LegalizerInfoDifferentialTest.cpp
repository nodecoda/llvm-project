//===-- X86LegalizerInfoDifferentialTest.cpp --------------------*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Exhaustive differential test over a bounded quotient of GlobalISel
// legality queries.  Two tables answer the same queries:
//
//   oracle  X86LegalizerInfoReference  (frozen copy of the hand-written rules)
//   SUT     GeneratedLegalizerInfoForTest (TableGen-generated fragments)
//
// P0 compares the first legalization step only.  That step is a pure function
// of (table, concrete LegalityQuery), so the comparison is unambiguous.  Type
// traces are deliberately out of scope here: simulating them requires
// modelling LegalizerHelper, which is not the table.
//
// The quotient is "exhaustive over the frozen 18-class type vocabulary",
// not "all possible queries".
//
//===----------------------------------------------------------------------===//

#include "GISel/X86LegalizerTable.h"
#include "X86LegalizerInfoReference.h"
#include "X86Subtarget.h"
#include "X86TargetMachine.h"
#include "x86-ctpop-atoms.h"
#include "x86-ctpop-report.h"
#include "x86-legalizer-pins.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VCSRevision.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace llvm;
using namespace TargetOpcode;
using namespace LegalizeActions;
using namespace LegalityPredicates;
using namespace LegalizeMutations;

namespace {

//===----------------------------------------------------------------------===//
// Rule index (test-only)
//===----------------------------------------------------------------------===//
//
// -gen-legalizer-rule-index emits the same evaluator as -gen-legalizer-table,
// but a matching rule stores its step and its 0-based position in the rule
// set's Rules list instead of returning.  That position is the number
// X86LegalizerInfo.core.json records as rule_index, so a test can name the rule
// that won rather than only its effect.  No production TU includes the file.
//
// The domain is generated from the same point emitter the reader's selfCheck()
// walks.  The struct below is the shape that emitter's aggregate initializers
// expect -- the reader's own definition is private to X86LegalizerTable.cpp --
// so the 1397 queries are included, never copied.
struct X86TablePoint {
  unsigned Opcode;
  const char *Name;
  uint8_t NumTypes;
  LLT Types[3];
  LLT MemTy;
  uint64_t Align;
  bool HasMem;
};

#include "X86LegalizerRuleIndex.inc"
#include "X86LegalizerTablePointsForTest.inc"

struct RuleHit {
  unsigned RuleSet;
  unsigned RuleIndex;
  LegalizeActionStep Step;
};

static std::optional<RuleHit> queryRuleIndex(unsigned Table,
                                             const X86Subtarget &Subtarget,
                                             const LegalityQuery &Q) {
  unsigned RuleIndex = 0;
  LegalizeActionStep Step(Unsupported, 0, LLT{});
  if (!X86EvalRuleIndex(Table, Subtarget, Q, RuleIndex, Step))
    return std::nullopt;
  return RuleHit{Table, RuleIndex, Step};
}

// The SaaS-generated fragments are a bare statement sequence that expects
// `Subtarget` and the inherited builder API in scope, exactly as
// X86LegalizerInfo's constructor provides them.
class GeneratedLegalizerInfoForTest : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit GeneratedLegalizerInfoForTest(const X86Subtarget &STI)
      : Subtarget(STI) {
#include "X86LegalizerInfo.inc"
  }

  // Never executed: the test only calls getAction().  The base default is
  // llvm_unreachable, so override with a safe no-op.
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};

// The table reader installed on the same generated fragments.  Its answers are
// compared through query() directly: getAction() falls back to the chain when
// the table declines, which would make a decline look like agreement.
class TableLegalizerInfoForTest : public LegalizerInfo {
  const X86Subtarget &Subtarget;
  X86LegalizerTable *TablePtr = nullptr;

public:
  explicit TableLegalizerInfoForTest(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfo.inc"
    auto T = std::make_unique<X86LegalizerTable>(
        *this, Subtarget, Subtarget.isTarget64BitLP64() ? 64 : 32);
    TablePtr = T.get();
    setTable(std::move(T));
  }

  const X86LegalizerTable &table() const { return *TablePtr; }
  X86LegalizerTable &mutableTable() { return *TablePtr; }

  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};

// Mutation fixtures.  Each one is the committed X86LegalizerInfo.td with
// exactly one rule corrupted by CMake (see x86_add_legalizer_mutant): the
// .inc is generated from the .td, so a mutant cannot drift from the table it
// is supposed to falsify.  The corruption is textual, so the generated
// fragment is still a well-formed statement sequence and needs the same
// `Subtarget` in scope as the SUT above.
namespace mutant1 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant1.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant1

namespace mutant2 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant2.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant2

namespace mutant3 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant3.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant3

struct Config {
  const char *ID;
  const char *Triple;
  const char *FS;
  bool Is64;
  bool AVX512;
  bool VPOPCNTDQ;
  bool VLX;
  bool POPCNT;
};

// C5/C6 exist so that (HasAVX512 vs HasVPOPCNTDQ) and (HasVLX vs
// HasVPOPCNTDQVL) are separable.  Without them a transcription error that
// swaps either pair is invisible on every atom.
static const Config Configs[] = {
    {"C0", "x86_64", "+popcnt", true, false, false, false, true},
    {"C1", "x86_64", "+popcnt,+avx512f,+avx512vpopcntdq", true, true, true,
     false, true},
    {"C2", "x86_64", "+popcnt,+avx512f,+avx512vl,+avx512vpopcntdq", true, true,
     true, true, true},
    {"C3", "x86_64", "", true, false, false, false, false},
    {"C4", "i386", "+popcnt", false, false, false, false, true},
    {"C5", "x86_64", "+popcnt,+avx512f", true, true, false, false, true},
    {"C6", "x86_64", "+popcnt,+avx512f,+avx512vl", true, true, false, true,
     true},
};

// The four fixed full-corpus configurations from the test spec.  They are a
// separate axis from the CTPOP sigma configurations above: the ledger needs
// the negative guards (x87 without SSE, no CMOV) and the AVX-512 extensions
// (DQI/BWI/LZCNT/BMI) that the sigma array does not vary.
static const Config FullConfigs[] = {
    {"C0", "i386", "-x87", false, false, false, false, false},
    {"C1", "x86_64", "", true, false, false, false, false},
    {"C2", "x86_64",
     "+sse,+sse2,+sse4.1,+avx,+avx2,+avx512f,+avx512dq,+avx512bw,+avx512vl,"
     "+popcnt,+avx512vpopcntdq,+lzcnt,+bmi,+cmov",
     true, true, true, true, true},
    {"C3", "i386", "+x87,+popcnt,+lzcnt,+bmi,+cmov", false, false, false, false,
     true},
};

struct TypeClass {
  const char *ID;
  LLT Dst;
  LLT Src;
};

static std::vector<TypeClass> typeClasses() {
  const LLT s7 = LLT::scalar(7), s8 = LLT::scalar(8), s16 = LLT::scalar(16),
            s24 = LLT::scalar(24), s32 = LLT::scalar(32), s64 = LLT::scalar(64),
            s128 = LLT::scalar(128), s200 = LLT::scalar(200);
  const LLT v2s32 = LLT::fixed_vector(2, 32), v4s32 = LLT::fixed_vector(4, 32),
            v8s32 = LLT::fixed_vector(8, 32),
            v16s32 = LLT::fixed_vector(16, 32),
            v2s64 = LLT::fixed_vector(2, 64), v4s64 = LLT::fixed_vector(4, 64),
            v8s64 = LLT::fixed_vector(8, 64), v16s8 = LLT::fixed_vector(16, 8);
  return {
      {"T0", s8, s8},         {"T1", s16, s16},      {"T2", s32, s32},
      {"T3", s64, s64},       {"T4", s128, s128},    {"T5", v4s32, v4s32},
      {"T6", v8s32, v8s32},   {"T7", v2s64, v2s64},  {"T8", v4s64, v4s64},
      {"T9", v16s32, v16s32}, {"T10", v8s64, v8s64}, {"T11", v16s8, v16s8},
      {"T12", v2s32, v2s32},  {"T13", s7, s7},       {"T14", s24, s24},
      {"N0", s128, s64},      {"N1", s64, s32},      {"B0", s200, s200},
  };
}

class X86LegalizerDifferentialTest : public testing::Test {
protected:
  static void SetUpTestCase() {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
  }
};

struct Sigma {
  std::unique_ptr<TargetMachine> TM;
  const X86Subtarget *STI = nullptr;
};

static Sigma makeSigma(const Config &C) {
  Sigma S;
  Triple TT(C.Triple);
  std::string Err;
  const Target *T = TargetRegistry::lookupTarget("", TT, Err);
  EXPECT_NE(T, nullptr) << Err;
  if (!T)
    return S;
  TargetOptions Opts;
  S.TM.reset(T->createTargetMachine(TT, "generic", C.FS, Opts, std::nullopt));
  EXPECT_NE(S.TM, nullptr);
  if (!S.TM)
    return S;
  static LLVMContext Ctx;
  static Module M("t", Ctx);
  if (!M.getFunction("f"))
    Function::Create(FunctionType::get(Type::getVoidTy(Ctx), false),
                     Function::ExternalLinkage, "f", M);
  S.STI = static_cast<const X86Subtarget *>(
      S.TM->getSubtargetImpl(*M.getFunction("f")));
  return S;
}

// LegalizeAction::Lower and Libcall carry no type argument; treating their
// (unused) NewType as a decision field would compare uninitialized data.
static bool actionCarriesType(LegalizeAction Action) {
  switch (Action) {
  case Legal:
  case Unsupported:
  case Custom:
  case Lower:
  case Libcall:
    return false;
  default:
    return true;
  }
}

// The comparison predicate.  It must be sensitive to all three fields:
// comparing only the action would miss a table that widens to the wrong type,
// and comparing only the mutation would miss a table that legalizes where the
// other widens.
static bool stepsAgree(const LegalizeActionStep &A,
                       const LegalizeActionStep &B) {
  if (A.Action != B.Action)
    return false;
  if (!actionCarriesType(A.Action))
    return true;
  return A.TypeIdx == B.TypeIdx && A.NewType == B.NewType;
}

// The spelling the corpus JSON uses ("v4s32", "s200"), which is also what a
// reader of the committed report expects.  LLT's own operator<< prints
// "<4 x s32>", so the two must not be mixed.
static std::string shortLLT(LLT T) {
  std::string S;
  raw_string_ostream OS(S);
  if (T.isVector())
    OS << "v" << T.getNumElements() << "s" << T.getScalarSizeInBits();
  else if (T.isPointer())
    OS << "p" << T.getAddressSpace() << ":" << T.getSizeInBits();
  else
    OS << "s" << T.getSizeInBits();
  return OS.str();
}

// Action names, not enum values: a failure that says "1 idx=1 to=s32" makes
// the reader open the header to find out what 1 means.
static std::string describe(const LegalizeActionStep &S) {
  std::string Out;
  raw_string_ostream OS(Out);
  OS << S.Action;
  if (actionCarriesType(S.Action))
    OS << " idx=" << S.TypeIdx << " to=" << shortLLT(S.NewType);
  return OS.str();
}

//===----------------------------------------------------------------------===//
// Reference provenance artifacts
//===----------------------------------------------------------------------===//
//
// The reference's bytes are pinned by x86-ctpop-atoms.json, but a hash of the
// source says nothing about what the oracle means.  These artifacts pin the
// vocabulary and a behaviour fingerprint instead: every builder signature, the
// LLT spellings the source uses, three concrete queries per table descriptor,
// and the oracle's answer to each of those queries.

static std::optional<std::string> readTestSourceFile(StringRef Name) {
  std::string Path = std::string(X86_LEGALIZER_TEST_SRC_DIR) + "/" + Name.str();
  auto Buffer = MemoryBuffer::getFile(Path);
  if (!Buffer)
    return std::nullopt;
  return (*Buffer)->getBuffer().str();
}

static std::string sha256Hex(StringRef Data) {
  return toHex(SHA256::hash(arrayRefFromStringRef(Data)), /*LowerCase=*/true);
}

static const Config *findConfig(StringRef ID) {
  for (const Config &C : Configs)
    if (ID == C.ID)
      return &C;
  return nullptr;
}

static std::optional<LLT> parseJSONLLT(StringRef S, unsigned PointerBits) {
  if (S.consume_front("p")) {
    unsigned AddressSpace = 0;
    if (S.getAsInteger(10, AddressSpace))
      return std::nullopt;
    return LLT::pointer(AddressSpace, PointerBits);
  }
  if (S.consume_front("v")) {
    const size_t Sep = S.find_first_of("ps");
    if (Sep == StringRef::npos)
      return std::nullopt;
    unsigned Lanes = 0, Elem = 0;
    if (S.take_front(Sep).getAsInteger(10, Lanes) ||
        S.drop_front(Sep + 1).getAsInteger(10, Elem))
      return std::nullopt;
    if (S[Sep] == 'p')
      return LLT::fixed_vector(Lanes, LLT::pointer(Elem, PointerBits));
    return LLT::fixed_vector(Lanes, Elem);
  }
  if (S.consume_front("s")) {
    unsigned Bits = 0;
    if (S.getAsInteger(10, Bits))
      return std::nullopt;
    return LLT::scalar(Bits);
  }
  return std::nullopt;
}

static std::string jsonLLTName(LLT T) {
  std::string S;
  raw_string_ostream OS(S);
  if (T.isPointer())
    OS << "p" << T.getAddressSpace();
  else if (T.isPointerVector())
    OS << "v" << T.getNumElements() << "p"
       << T.getElementType().getAddressSpace();
  else if (T.isVector())
    OS << "v" << T.getNumElements() << "s" << T.getScalarSizeInBits();
  else
    OS << "s" << T.getSizeInBits();
  return OS.str();
}

static std::optional<unsigned> opcodeForName(StringRef Name) {
  unsigned Opcode = StringSwitch<unsigned>(Name)
                        .Case("G_ADD", G_ADD)
                        .Case("G_AND", G_AND)
                        .Case("G_ANYEXT", G_ANYEXT)
                        .Case("G_BR", G_BR)
                        .Case("G_BRCOND", G_BRCOND)
                        .Case("G_BSWAP", G_BSWAP)
                        .Case("G_BUILD_VECTOR", G_BUILD_VECTOR)
                        .Case("G_CONCAT_VECTORS", G_CONCAT_VECTORS)
                        .Case("G_CONSTANT", G_CONSTANT)
                        .Case("G_CONSTANT_POOL", G_CONSTANT_POOL)
                        .Case("G_CTLZ", G_CTLZ)
                        .Case("G_CTPOP", G_CTPOP)
                        .Case("G_CTTZ", G_CTTZ)
                        .Case("G_CTTZ_ZERO_POISON", G_CTTZ_ZERO_POISON)
                        .Case("G_DYN_STACKALLOC", G_DYN_STACKALLOC)
                        .Case("G_EXTRACT", G_EXTRACT)
                        .Case("G_FABS", G_FABS)
                        .Case("G_FADD", G_FADD)
                        .Case("G_FCMP", G_FCMP)
                        .Case("G_FCONSTANT", G_FCONSTANT)
                        .Case("G_FCOS", G_FCOS)
                        .Case("G_FNEG", G_FNEG)
                        .Case("G_FPEXT", G_FPEXT)
                        .Case("G_FPEXTLOAD", G_FPEXTLOAD)
                        .Case("G_FPTOSI", G_FPTOSI)
                        .Case("G_FPTOUI", G_FPTOUI)
                        .Case("G_FPTRUNC", G_FPTRUNC)
                        .Case("G_FPTRUNCSTORE", G_FPTRUNCSTORE)
                        .Case("G_FRAME_INDEX", G_FRAME_INDEX)
                        .Case("G_FSQRT", G_FSQRT)
                        .Case("G_GET_ROUNDING", G_GET_ROUNDING)
                        .Case("G_GLOBAL_VALUE", G_GLOBAL_VALUE)
                        .Case("G_ICMP", G_ICMP)
                        .Case("G_IMPLICIT_DEF", G_IMPLICIT_DEF)
                        .Case("G_INSERT", G_INSERT)
                        .Case("G_INTRINSIC", G_INTRINSIC)
                        .Case("G_INTRINSIC_ROUNDEVEN", G_INTRINSIC_ROUNDEVEN)
                        .Case("G_INTTOPTR", G_INTTOPTR)
                        .Case("G_INVOKE_REGION_START", G_INVOKE_REGION_START)
                        .Case("G_IS_FPCLASS", G_IS_FPCLASS)
                        .Case("G_LOAD", G_LOAD)
                        .Case("G_LROUND", G_LROUND)
                        .Case("G_MEMCPY", G_MEMCPY)
                        .Case("G_MERGE_VALUES", G_MERGE_VALUES)
                        .Case("G_MUL", G_MUL)
                        .Case("G_PTRTOINT", G_PTRTOINT)
                        .Case("G_PTR_ADD", G_PTR_ADD)
                        .Case("G_SDIV", G_SDIV)
                        .Case("G_SELECT", G_SELECT)
                        .Case("G_SEXT", G_SEXT)
                        .Case("G_SEXTLOAD", G_SEXTLOAD)
                        .Case("G_SEXT_INREG", G_SEXT_INREG)
                        .Case("G_SHL", G_SHL)
                        .Case("G_SITOFP", G_SITOFP)
                        .Case("G_SMULH", G_SMULH)
                        .Case("G_STORE", G_STORE)
                        .Case("G_TRAP", G_TRAP)
                        .Case("G_TRUNC", G_TRUNC)
                        .Case("G_UADDE", G_UADDE)
                        .Case("G_UITOFP", G_UITOFP)
                        .Case("G_UMIN", G_UMIN)
                        .Case("G_UNMERGE_VALUES", G_UNMERGE_VALUES)
                        .Case("G_ZEXTLOAD", G_ZEXTLOAD)
                        .Default(~0u);
  if (Opcode == ~0u)
    return std::nullopt;
  return Opcode;
}

//===----------------------------------------------------------------------===//
// Domain enumeration (test-only)
//===----------------------------------------------------------------------===//
//
// The domain is built from the metadata the rule-index backend emits, not from
// a copy of the table: the type axis is X86TestTypeClasses[], the descriptor
// axis is X86TestMemClasses[]/X86TestAlignThresholds[], and the per-rule-set
// shape is X86TestTables[].NumSlots.  A rule set that reads Q.Types[1] gets a
// two-slot query even when the second slot is named through a pair builder or
// a slot-indexed predicate; a one-slot rule set would otherwise make the
// evaluator read past the end of LegalityQuery::Types.

enum class DomainKind { ClassPair, Boundary, AScalar, ASVector };

static LLT testTypeClassLLT(const X86TestTypeClass &C, unsigned PointerBits) {
  switch (C.Kind) {
  case X86TestKind_Pointer:
    return LLT::pointer(0, PointerBits);
  case X86TestKind_Scalar:
    return LLT::scalar(C.ElemBits);
  case X86TestKind_Vector:
    return LLT::fixed_vector(C.Lanes, C.ElemBits);
  case X86TestKind_PtrVector:
    return LLT::fixed_vector(C.Lanes, LLT::pointer(0, PointerBits));
  }
  llvm_unreachable("bad test type class");
}

static std::vector<LLT> typeClassReps(unsigned PointerBits) {
  std::vector<LLT> Reps;
  Reps.reserve(X86TestNumTypeClasses);
  for (unsigned I = 0; I != X86TestNumTypeClasses; ++I)
    Reps.push_back(testTypeClassLLT(X86TestTypeClasses[I], PointerBits));
  return Reps;
}

static std::vector<LLT> memClassReps(unsigned PointerBits) {
  std::vector<LLT> Reps;
  Reps.reserve(X86TestNumMemClasses);
  for (unsigned I = 0; I != X86TestNumMemClasses; ++I)
    Reps.push_back(testTypeClassLLT(X86TestMemClasses[I], PointerBits));
  return Reps;
}

// The reviewed 12-atom panel: scalars the class list does not name and three
// vectors that exercise the element-count axis.
static ArrayRef<LLT> boundaryPanel() {
  static const std::vector<LLT> Panel = {LLT::scalar(3),
                                         LLT::scalar(7),
                                         LLT::scalar(24),
                                         LLT::scalar(33),
                                         LLT::scalar(65),
                                         LLT::scalar(100),
                                         LLT::scalar(129),
                                         LLT::scalar(200),
                                         LLT::scalar(256),
                                         LLT::fixed_vector(3, 32),
                                         LLT::fixed_vector(16, 16),
                                         LLT::fixed_vector(64, 8)};
  return Panel;
}

// Representative alignment for an alignment class.  Class 0 is "below every
// threshold"; class N is the N-1th threshold.
static uint64_t alignForClass(unsigned Class) {
  return Class == 0 ? 0 : X86TestAlignThresholds[Class - 1];
}

// The axes the domain enumeration walks.  They come from the metadata the
// rule-index backend emits, never from a copy of the table.
struct TableDomainAxis {
  std::vector<LLT> Classes;
  std::vector<LLT> MemClasses;
  LLT P0;
  LLT P1;
  LLT P2;
  LLT V2P1;
  LLT V4P1;
};

static TableDomainAxis tableDomainAxis(unsigned PointerBits) {
  TableDomainAxis A;
  A.Classes = typeClassReps(PointerBits);
  A.MemClasses = memClassReps(PointerBits);
  A.P0 = A.Classes.front();
  A.P1 = LLT::pointer(1, PointerBits);
  A.P2 = LLT::pointer(2, PointerBits);
  A.V2P1 = LLT::fixed_vector(2, A.P1);
  A.V4P1 = LLT::fixed_vector(4, A.P1);
  return A;
}

// Class-pair breadth.  A memory rule set adds the descriptor axis, so its
// second type slot cycles the class list too; the query shape is the one the
// table's descriptor keys.
template <typename Sink>
static void enumerateClassPairPoints(const X86TestTable &T,
                                     const TableDomainAxis &A, Sink &&Emit) {
  if (T.HasMem) {
    for (const LLT &Value : A.Classes)
      for (const LLT &Other : A.Classes)
        for (const LLT &Mem : A.MemClasses)
          for (unsigned Align = 0; Align != X86TestNumAlignClasses; ++Align)
            Emit(ArrayRef<LLT>({Value, Other}), true, Mem,
                 alignForClass(Align));
  } else if (T.NumSlots == 1) {
    for (const LLT &Ty : A.Classes)
      Emit(ArrayRef<LLT>({Ty}), false, LLT{}, 0);
  } else {
    for (const LLT &L : A.Classes)
      for (const LLT &R : A.Classes)
        Emit(ArrayRef<LLT>({L, R}), false, LLT{}, 0);
  }
}

// Boundary depth: the panel in slot 0 for a one-slot rule set, and in both
// directions for a two-slot one.  A memory rule set alternates the panel
// between the value type and the descriptor's memory type.
template <typename Sink>
static void enumerateBoundaryPoints(const X86TestTable &T,
                                    const TableDomainAxis &A, Sink &&Emit) {
  const ArrayRef<LLT> Boundary = boundaryPanel();
  if (T.HasMem) {
    for (const LLT &C : A.Classes)
      for (const LLT &B : Boundary)
        for (unsigned Align = 0; Align != X86TestNumAlignClasses; ++Align) {
          Emit(ArrayRef<LLT>({C, A.P0}), true, B, alignForClass(Align));
          Emit(ArrayRef<LLT>({B, A.P0}), true, C, alignForClass(Align));
        }
  } else if (T.NumSlots == 1) {
    for (const LLT &B : Boundary)
      Emit(ArrayRef<LLT>({B}), false, LLT{}, 0);
  } else {
    for (const LLT &C : A.Classes)
      for (const LLT &B : Boundary) {
        Emit(ArrayRef<LLT>({C, B}), false, LLT{}, 0);
        Emit(ArrayRef<LLT>({B, C}), false, LLT{}, 0);
      }
  }
}

// Scalar address-space points.  A non-zero AS is answered by the evaluator on
// the original query, so it is a domain point even though the table's key
// folds every pointer to p0.
template <typename Sink>
static void enumerateScalarASPoints(const X86TestTable &T,
                                    const TableDomainAxis &A, Sink &&Emit) {
  const uint64_t Alignment = alignForClass(1);
  if (T.HasMem) {
    // slot 0
    Emit(ArrayRef<LLT>({A.P1, A.P0}), true, A.P0, Alignment);
    Emit(ArrayRef<LLT>({A.P2, A.P0}), true, A.P0, Alignment);
    // slot 1
    Emit(ArrayRef<LLT>({A.P0, A.P1}), true, A.P0, Alignment);
    Emit(ArrayRef<LLT>({A.P0, A.P2}), true, A.P0, Alignment);
    // value AS x memory AS
    Emit(ArrayRef<LLT>({A.P1, A.P0}), true, A.P1, Alignment);
    Emit(ArrayRef<LLT>({A.P1, A.P0}), true, A.P2, Alignment);
    Emit(ArrayRef<LLT>({A.P2, A.P0}), true, A.P1, Alignment);
    Emit(ArrayRef<LLT>({A.P2, A.P0}), true, A.P2, Alignment);
    // the G_STORE (p0)[mem p1] half
    Emit(ArrayRef<LLT>({A.P0, A.P0}), true, A.P1, Alignment);
    Emit(ArrayRef<LLT>({A.P0, A.P0}), true, A.P2, Alignment);
  } else if (T.NumSlots == 1) {
    Emit(ArrayRef<LLT>({A.P1}), false, LLT{}, 0);
    Emit(ArrayRef<LLT>({A.P2}), false, LLT{}, 0);
  } else {
    Emit(ArrayRef<LLT>({A.P1, A.P0}), false, LLT{}, 0);
    Emit(ArrayRef<LLT>({A.P2, A.P0}), false, LLT{}, 0);
    Emit(ArrayRef<LLT>({A.P0, A.P1}), false, LLT{}, 0);
    Emit(ArrayRef<LLT>({A.P0, A.P2}), false, LLT{}, 0);
  }
}

// Pointer-vector address-space points.
template <typename Sink>
static void enumerateVectorASPoints(const X86TestTable &T,
                                    const TableDomainAxis &A, Sink &&Emit) {
  const uint64_t Alignment = alignForClass(1);
  if (T.HasMem) {
    Emit(ArrayRef<LLT>({A.V2P1, A.P0}), true, A.P0, Alignment);
    Emit(ArrayRef<LLT>({A.V4P1, A.P0}), true, A.P0, Alignment);
    Emit(ArrayRef<LLT>({A.P0, A.V2P1}), true, A.P0, Alignment);
    Emit(ArrayRef<LLT>({A.P0, A.V4P1}), true, A.P0, Alignment);
    // The per-mem-set vector point is counted separately by the spec even
    // though it repeats the slot-0 point above.
    Emit(ArrayRef<LLT>({A.V2P1, A.P0}), true, A.P0, Alignment);
    Emit(ArrayRef<LLT>({A.V4P1, A.P0}), true, A.P0, Alignment);
  } else if (T.NumSlots == 1) {
    Emit(ArrayRef<LLT>({A.V2P1}), false, LLT{}, 0);
    Emit(ArrayRef<LLT>({A.V4P1}), false, LLT{}, 0);
  } else {
    Emit(ArrayRef<LLT>({A.V2P1, A.P0}), false, LLT{}, 0);
    Emit(ArrayRef<LLT>({A.V4P1, A.P0}), false, LLT{}, 0);
    Emit(ArrayRef<LLT>({A.P0, A.V2P1}), false, LLT{}, 0);
    Emit(ArrayRef<LLT>({A.P0, A.V4P1}), false, LLT{}, 0);
  }
}

template <typename Sink>
static void enumerateTableDomain(unsigned Table, unsigned PointerBits,
                                 Sink &&Emit) {
  const X86TestTable &T = X86TestTables[Table];
  const std::optional<unsigned> Opcode = opcodeForName(T.Opcode);
  if (!Opcode) {
    ADD_FAILURE() << "no TargetOpcode for " << T.Opcode;
    return;
  }
  const TableDomainAxis A = tableDomainAxis(PointerBits);
  enumerateClassPairPoints(
      T, A, [&](ArrayRef<LLT> Types, bool HasMem, LLT MemTy, uint64_t Align) {
        Emit(DomainKind::ClassPair, Types, HasMem, MemTy, Align);
      });
  enumerateBoundaryPoints(
      T, A, [&](ArrayRef<LLT> Types, bool HasMem, LLT MemTy, uint64_t Align) {
        Emit(DomainKind::Boundary, Types, HasMem, MemTy, Align);
      });
  enumerateScalarASPoints(
      T, A, [&](ArrayRef<LLT> Types, bool HasMem, LLT MemTy, uint64_t Align) {
        Emit(DomainKind::AScalar, Types, HasMem, MemTy, Align);
      });
  enumerateVectorASPoints(
      T, A, [&](ArrayRef<LLT> Types, bool HasMem, LLT MemTy, uint64_t Align) {
        Emit(DomainKind::ASVector, Types, HasMem, MemTy, Align);
      });
}

// The witness boundary sample is the reviewed 12-atom panel applied once per
// rule set -- 63 x 12 = 756 atoms, not a second domain.  A one-slot rule set
// takes the panel in slot 0; a two-slot one alternates it between the keyed
// slots; a memory rule set alternates it between the value type and the
// descriptor's memory type.
template <typename Sink>
static void enumerateWitnessBoundaryPanel(const X86TestTable &T,
                                          const TableDomainAxis &A,
                                          Sink &&Emit) {
  const ArrayRef<LLT> Panel = boundaryPanel();
  const uint64_t Align = alignForClass(1);
  // The non-panel slot is a class representative, but not p0: X86_CTPOP and
  // friends end with an unguarded scalarSameSizeAs that would mutate a pointer.
  // Keeping the representative scalar means every boundary atom is a shape the
  // reference can answer.
  const LLT Rep = LLT::scalar(32);
  for (unsigned I = 0; I != Panel.size(); ++I) {
    const LLT &B = Panel[I];
    if (T.HasMem) {
      if (I % 2 == 0)
        Emit(I, ArrayRef<LLT>({B, A.P0}), true, Rep, Align);
      else
        Emit(I, ArrayRef<LLT>({Rep, A.P0}), true, B, Align);
    } else if (T.NumSlots == 1) {
      Emit(I, ArrayRef<LLT>({B}), false, LLT{}, 0);
    } else if (I % 2 == 0) {
      Emit(I, ArrayRef<LLT>({B, Rep}), false, LLT{}, 0);
    } else {
      Emit(I, ArrayRef<LLT>({Rep, B}), false, LLT{}, 0);
    }
  }
}

// The witness scalar-AS slice follows the construction in the test spec
// (slot-0, slot-1, memory cross product and the memory-symmetric half) but
// keeps the 220-atom budget of the G003 assignment:
//
//   220 = 29*2 single-slot + 28*4 double-slot + 6*10 memory
//         - 5*2 skipped inferred slot-1 points.
//
// Five rule sets read slot 1 only through an inferred scalar-width predicate
// (sizeNotPow2(1) / scalarNarrowerThan(1) / scalarWiderThan(1)); their slot-1
// AS points are the ten that the full-domain count (230) has and this slice
// does not.  Their slot-0 AS points are still emitted; the full-domain count
// stays asserted by DomainCountsMatchSpec.
static bool skipsWitnessSlot1AS(StringRef RuleSet) {
  return RuleSet == "X86_MERGE_VALUES" || RuleSet == "X86_UNMERGE_VALUES" ||
         RuleSet == "X86_ANYEXT" || RuleSet == "X86_SEXT" ||
         RuleSet == "X86_FCMP";
}

template <typename Sink>
static void enumerateWitnessScalarAS(const X86TestTable &T,
                                     const TableDomainAxis &A, Sink &&Emit) {
  unsigned Serial = 0;
  enumerateScalarASPoints(
      T, A, [&](ArrayRef<LLT> Types, bool HasMem, LLT MemTy, uint64_t Align) {
        const bool IsSlot1 = Types.size() == 2 && Types[0] == A.P0 &&
                             (Types[1] == A.P1 || Types[1] == A.P2) &&
                             (!HasMem || MemTy == A.P0);
        if (IsSlot1 && skipsWitnessSlot1AS(T.Name))
          return;
        Emit(Serial++, Types, HasMem, MemTy, Align);
      });
}

// The counts are a property of the emitted metadata, so the test walks the
// same enumeration the ledger uses instead of recomputing a formula and
// hoping the two agree.
TEST_F(X86LegalizerDifferentialTest, DomainCountsMatchSpec) {
  ASSERT_EQ(X86TestNumTables, 63u);

  unsigned SingleSlot = 0, DoubleSlot = 0, MemTables = 0, Slot1Readers = 0;
  for (unsigned I = 0; I != X86TestNumTables; ++I) {
    const X86TestTable &T = X86TestTables[I];
    if (T.HasMem) {
      ++MemTables;
      if (T.NumSlots < 2)
        ADD_FAILURE() << T.Name << " has a memory descriptor but NumSlots="
                      << unsigned(T.NumSlots);
    } else if (T.NumSlots == 1) {
      ++SingleSlot;
    } else if (T.NumSlots == 2) {
      ++DoubleSlot;
    } else {
      ADD_FAILURE() << T.Name
                    << " has unsupported NumSlots=" << unsigned(T.NumSlots);
    }
    if (T.NumSlots >= 2)
      ++Slot1Readers;
  }

  uint64_t ClassPair = 0, Boundary = 0, AScalar = 0, ASVector = 0;
  for (unsigned I = 0; I != X86TestNumTables; ++I)
    enumerateTableDomain(I, 64,
                         [&](DomainKind Kind, ArrayRef<LLT> Types, bool HasMem,
                             LLT MemTy, uint64_t Align) {
                           EXPECT_FALSE(Types.empty());
                           EXPECT_EQ(HasMem, MemTy.isValid());
                           if (HasMem)
                             EXPECT_LT(Align, 2u) << "alignment " << Align
                                                  << " outside the current "
                                                  << "two alignment classes";
                           switch (Kind) {
                           case DomainKind::ClassPair:
                             ++ClassPair;
                             break;
                           case DomainKind::Boundary:
                             ++Boundary;
                             break;
                           case DomainKind::AScalar:
                             ++AScalar;
                             break;
                           case DomainKind::ASVector:
                             ++ASVector;
                             break;
                           }
                         });

  const uint64_t AS = AScalar + ASVector;
  const uint64_t PerConfig = ClassPair + Boundary + AS;
  const uint64_t FourConfigs = 4 * PerConfig;
  const uint64_t Deep60 = 3 * (ClassPair + 5 * Boundary + AS);

  llvm::outs() << "legalizer domain:\n"
               << "  tables=" << X86TestNumTables
               << " typeClasses=" << X86TestNumTypeClasses
               << " memClasses=" << X86TestNumMemClasses
               << " alignClasses=" << X86TestNumAlignClasses << "\n"
               << "  slot shape: single=" << SingleSlot
               << " double-nonmem=" << DoubleSlot << " mem=" << MemTables
               << " slot1-readers=" << Slot1Readers << "\n"
               << "  classPair=" << ClassPair << " (spec 182712)\n"
               << "  boundary=" << Boundary << " (spec 23388)\n"
               << "  AS scalar=" << AScalar << " vector=" << ASVector
               << " total=" << AS << " (spec 230/206/436)\n"
               << "  per-config=" << PerConfig << " (spec 206536)\n"
               << "  4 configs=" << FourConfigs << " (spec 826144)\n"
               << "  deep 60-atom formula=" << Deep60 << " (spec 900264)\n";

  // v9 corrects the slot shape.  v8 counted a slot-1 read only when the rule
  // text said typeIs(1, ...) or Q.Types[1].  X86_MERGE_VALUES,
  // X86_UNMERGE_VALUES, X86_ANYEXT, X86_SEXT and X86_FCMP read slot 1 through
  // sizeNotPow2(1) / scalarNarrowerThan(1, ...) / scalarWiderThan(1, ...),
  // which are just as much Q.Types[1] accesses; a one-slot query would read
  // past the end of LegalityQuery::Types.
  EXPECT_EQ(SingleSlot, 29u);
  EXPECT_EQ(DoubleSlot, 28u);
  EXPECT_EQ(MemTables, 6u);
  EXPECT_EQ(Slot1Readers, 34u);
  EXPECT_EQ(ClassPair, 182712u);
  EXPECT_EQ(Boundary, 23388u);
  EXPECT_EQ(AScalar, 230u);
  EXPECT_EQ(ASVector, 206u);
  EXPECT_EQ(AS, 436u);
  EXPECT_EQ(PerConfig, 206536u);
  EXPECT_EQ(FourConfigs, 826144u);
  EXPECT_EQ(3 * (182712 + 116940 + 436), 900264);
  EXPECT_EQ(230u, 63u * 2 + 34u * 2 + 6u * 4 + 6u * 2);
  EXPECT_EQ(206u, 63u * 2 + 34u * 2 + 6u * 2);
}

struct BehaviorQuery {
  std::string Id;
  std::string Descriptor;
  std::string Kind;
  std::string Config;
  std::string Opcode;
  SmallVector<std::string, 3> Types;
  bool HasMemDesc = false;
  std::string MemTy;
  uint64_t AlignBits = 0;
};

static std::optional<BehaviorQuery> parseBehaviorQuery(const json::Object &O) {
  BehaviorQuery Q;
  auto Id = O.getString("id");
  auto Descriptor = O.getString("descriptor");
  auto Kind = O.getString("kind");
  auto Config = O.getString("config");
  auto Opcode = O.getString("opcode");
  if (!Id || !Descriptor || !Kind || !Config || !Opcode)
    return std::nullopt;
  Q.Id = Id->str();
  Q.Descriptor = Descriptor->str();
  Q.Kind = Kind->str();
  Q.Config = Config->str();
  Q.Opcode = Opcode->str();

  const json::Array *Types = O.getArray("types");
  if (!Types)
    return std::nullopt;
  for (const json::Value &V : *Types) {
    auto S = V.getAsString();
    if (!S)
      return std::nullopt;
    Q.Types.push_back(S->str());
  }

  if (const json::Object *MD = O.getObject("memdesc")) {
    auto MemTy = MD->getString("memory_ty");
    auto Align = MD->getInteger("align_bits");
    if (!MemTy || !Align)
      return std::nullopt;
    Q.HasMemDesc = true;
    Q.MemTy = MemTy->str();
    Q.AlignBits = *Align;
  } else if (const json::Value *MDV = O.get("memdesc")) {
    if (!MDV->getAsNull())
      return std::nullopt;
  }
  return Q;
}

static std::optional<unsigned>
buildBehaviorQuery(const BehaviorQuery &BQ, unsigned PointerBits,
                   SmallVectorImpl<LLT> &Types,
                   SmallVectorImpl<LegalityQuery::MemDesc> &MemDescs) {
  for (const std::string &Name : BQ.Types) {
    auto T = parseJSONLLT(Name, PointerBits);
    if (!T)
      return std::nullopt;
    Types.push_back(*T);
  }
  if (BQ.HasMemDesc) {
    auto T = parseJSONLLT(BQ.MemTy, PointerBits);
    if (!T)
      return std::nullopt;
    MemDescs.emplace_back(*T, BQ.AlignBits, AtomicOrdering::NotAtomic,
                          AtomicOrdering::NotAtomic);
  }
  return opcodeForName(BQ.Opcode);
}

TEST(ReferenceProvenance, ArtifactIsConsistent) {
  auto ProvText = readTestSourceFile("x86-legalizer-reference-provenance.json");
  ASSERT_TRUE(ProvText.has_value());
  auto Prov = json::parse(*ProvText);
  ASSERT_TRUE(!!Prov) << toString(Prov.takeError());
  const json::Object *P = Prov->getAsObject();
  ASSERT_NE(P, nullptr);

  EXPECT_EQ(P->getInteger("version").value_or(-1), 1);
  EXPECT_EQ(P->getInteger("builder_count").value_or(-1), 58);
  const json::Array *Builders = P->getArray("builders");
  ASSERT_NE(Builders, nullptr);
  EXPECT_EQ(Builders->size(), 58u);
  std::set<std::string> UniqueBuilders;
  for (const json::Value &V : *Builders) {
    auto S = V.getAsString();
    ASSERT_TRUE(S.has_value());
    UniqueBuilders.insert(S->str());
  }
  EXPECT_EQ(UniqueBuilders.size(), 58u)
      << "builder signatures must stay unique after sorting";

  auto QueriesText = readTestSourceFile("x86-legalizer-behavior-queries.json");
  ASSERT_TRUE(QueriesText.has_value());
  auto QueriesValue = json::parse(*QueriesText);
  ASSERT_TRUE(!!QueriesValue) << toString(QueriesValue.takeError());
  const json::Object *Q = QueriesValue->getAsObject();
  ASSERT_NE(Q, nullptr);
  EXPECT_EQ(Q->getInteger("count").value_or(-1), 189);
  EXPECT_EQ(P->getInteger("behavior_query_count").value_or(-1), 189);
  EXPECT_EQ(Q->getString("config_default").value_or("").str(), "C2");
  const json::Array *QueryArray = Q->getArray("queries");
  ASSERT_NE(QueryArray, nullptr);
  ASSERT_EQ(QueryArray->size(), 189u);

  std::set<std::string> Ids;
  std::map<std::string, std::set<std::string>> KindsByDescriptor;
  for (const json::Value &V : *QueryArray) {
    const json::Object *O = V.getAsObject();
    ASSERT_NE(O, nullptr);
    auto Id = O->getString("id");
    auto Descriptor = O->getString("descriptor");
    auto Kind = O->getString("kind");
    ASSERT_TRUE(Id.has_value());
    ASSERT_TRUE(Descriptor.has_value());
    ASSERT_TRUE(Kind.has_value());
    Ids.insert(Id->str());
    KindsByDescriptor[Descriptor->str()].insert(Kind->str());
  }
  EXPECT_EQ(Ids.size(), 189u);
  ASSERT_EQ(KindsByDescriptor.size(), 63u);
  for (const auto &Entry : KindsByDescriptor) {
    EXPECT_EQ(Entry.second.size(), 3u) << Entry.first;
    for (const char *Kind : {"hit", "boundary", "as"})
      EXPECT_TRUE(Entry.second.count(Kind)) << Entry.first << "/" << Kind;
  }

  auto ProvSha = P->getString("behavior_queries_sha256");
  ASSERT_TRUE(ProvSha.has_value());
  EXPECT_EQ(ProvSha->str(), sha256Hex(*QueriesText));

  auto RefSha = P->getString("reference_sha256");
  ASSERT_TRUE(RefSha.has_value());
  auto ReferenceText = readTestSourceFile("X86LegalizerInfoReference.cpp");
  ASSERT_TRUE(ReferenceText.has_value());
  EXPECT_EQ(RefSha->str(), sha256Hex(*ReferenceText));

  auto LiteralsText = readTestSourceFile("x86-legalizer-llt-literals.json");
  ASSERT_TRUE(LiteralsText.has_value());
  auto LiteralsValue = json::parse(*LiteralsText);
  ASSERT_TRUE(!!LiteralsValue) << toString(LiteralsValue.takeError());
  const json::Object *L = LiteralsValue->getAsObject();
  ASSERT_NE(L, nullptr);
  const json::Array *LiteralArray = L->getArray("literals");
  ASSERT_NE(LiteralArray, nullptr);
  EXPECT_EQ(L->getInteger("count").value_or(-1), (int64_t)LiteralArray->size());
  EXPECT_EQ(LiteralArray->size(), 24u);
  for (const json::Value &V : *LiteralArray) {
    auto S = V.getAsString();
    ASSERT_TRUE(S.has_value());
    EXPECT_NE(ReferenceText->find(*S), std::string::npos)
        << "literal not present in the reference source: " << *S;
  }
}

// The manifest records the coverage denominators derived from the Core IR.
// Recompute every field from the build product; the .td text is deliberately
// not parsed here, and no source text is pinned.
TEST(RuleCountsManifest, MatchesCoreIR) {
  auto ManifestText = readTestSourceFile("x86-legalizer-rule-counts.json");
  ASSERT_TRUE(ManifestText.has_value());
  auto ManifestValue = json::parse(*ManifestText);
  ASSERT_TRUE(!!ManifestValue) << toString(ManifestValue.takeError());
  const json::Object *M = ManifestValue->getAsObject();
  ASSERT_NE(M, nullptr);

  auto CoreBuffer = MemoryBuffer::getFile(X86_LEGALIZER_CORE_JSON);
  ASSERT_TRUE(!!CoreBuffer) << X86_LEGALIZER_CORE_JSON;
  auto CoreValue = json::parse((*CoreBuffer)->getBuffer());
  ASSERT_TRUE(!!CoreValue) << toString(CoreValue.takeError());
  const json::Object *Core = CoreValue->getAsObject();
  ASSERT_NE(Core, nullptr);

  const json::Array *Rulesets = Core->getArray("rulesets");
  ASSERT_NE(Rulesets, nullptr);
  const json::Array *Escapes = Core->getArray("escapes");
  ASSERT_NE(Escapes, nullptr);

  std::set<std::pair<std::string, int64_t>> LiteralRules;
  std::set<std::string> LiteralSetNames;
  int64_t CoreEntryCount = 0;
  for (const json::Value &RulesetValue : *Rulesets) {
    const json::Object *Ruleset = RulesetValue.getAsObject();
    ASSERT_NE(Ruleset, nullptr);
    auto Def = Ruleset->getString("def");
    ASSERT_TRUE(Def.has_value());
    LiteralSetNames.insert(Def->str());
    const json::Array *Rules = Ruleset->getArray("rules");
    ASSERT_NE(Rules, nullptr);
    for (const json::Value &RuleValue : *Rules) {
      const json::Object *Rule = RuleValue.getAsObject();
      ASSERT_NE(Rule, nullptr);
      auto RuleIndex = Rule->getInteger("rule_index");
      ASSERT_TRUE(RuleIndex.has_value());
      LiteralRules.emplace(Def->str(), *RuleIndex);
      ++CoreEntryCount;
    }
  }

  int64_t EscapedInLiteralSets = 0;
  std::set<std::string> EscapedSetNames;
  for (const json::Value &EscapeValue : *Escapes) {
    const json::Object *Escape = EscapeValue.getAsObject();
    ASSERT_NE(Escape, nullptr);
    auto Ruleset = Escape->getString("ruleset");
    ASSERT_TRUE(Ruleset.has_value());
    EscapedSetNames.insert(Ruleset->str());
    if (LiteralSetNames.count(Ruleset->str()))
      ++EscapedInLiteralSets;
  }

  std::set<std::string> OnlyEscapedSetNames;
  for (const std::string &Name : EscapedSetNames)
    if (!LiteralSetNames.count(Name))
      OnlyEscapedSetNames.insert(Name);

  const int64_t LiteralRuleCount = static_cast<int64_t>(LiteralRules.size());
  const int64_t LiteralSetCount = static_cast<int64_t>(LiteralSetNames.size());
  const int64_t EscapedRuleCount = static_cast<int64_t>(Escapes->size());
  const int64_t EscapedInAllEscapedSets =
      EscapedRuleCount - EscapedInLiteralSets;
  const int64_t OnlyEscapedSetCount =
      static_cast<int64_t>(OnlyEscapedSetNames.size());
  const int64_t TableCount = LiteralSetCount + OnlyEscapedSetCount;
  const int64_t TDRuleCount = LiteralRuleCount + EscapedRuleCount;

  auto ExpectField = [](const char *Name, int64_t Manifest,
                        int64_t Recomputed) {
    EXPECT_EQ(Manifest, Recomputed)
        << "field " << Name << ": manifest=" << Manifest
        << " recomputed=" << Recomputed;
  };

  ExpectField("table_count", M->getInteger("table_count").value_or(-1),
              TableCount);
  ExpectField("literal_bearing_rule_set_count",
              M->getInteger("literal_bearing_rule_set_count").value_or(-1),
              LiteralSetCount);
  ExpectField("literal_td_rule_count",
              M->getInteger("literal_td_rule_count").value_or(-1),
              LiteralRuleCount);
  ExpectField("core_entry_count",
              M->getInteger("core_entry_count").value_or(-1), CoreEntryCount);
  ExpectField("all_escaped_rule_set_count",
              M->getInteger("all_escaped_rule_set_count").value_or(-1),
              OnlyEscapedSetCount);
  ExpectField("escaped_td_rule_count",
              M->getInteger("escaped_td_rule_count").value_or(-1),
              EscapedRuleCount);
  ExpectField("escaped_in_literal_sets",
              M->getInteger("escaped_in_literal_sets").value_or(-1),
              EscapedInLiteralSets);
  ExpectField("escaped_in_all_escaped_sets",
              M->getInteger("escaped_in_all_escaped_sets").value_or(-1),
              EscapedInAllEscapedSets);
  ExpectField("td_rule_count", M->getInteger("td_rule_count").value_or(-1),
              TDRuleCount);

  EXPECT_EQ(TDRuleCount, LiteralRuleCount + EscapedRuleCount);
  EXPECT_EQ(EscapedRuleCount, EscapedInLiteralSets + EscapedInAllEscapedSets);
  EXPECT_EQ(TableCount, LiteralSetCount + OnlyEscapedSetCount);

  // The eight rule sets that only occur in the escape list, sorted.
  const std::vector<std::string> ExpectedOnlyEscaped = {
      "X86_CONSTANT_POOL", "X86_FPEXTLOAD",    "X86_FPTRUNCSTORE",
      "X86_FRAME_INDEX",   "X86_GLOBAL_VALUE", "X86_INTTOPTR",
      "X86_SEXTLOAD",      "X86_ZEXTLOAD"};
  std::vector<std::string> ActualOnlyEscaped(OnlyEscapedSetNames.begin(),
                                             OnlyEscapedSetNames.end());
  ASSERT_EQ(ActualOnlyEscaped.size(), ExpectedOnlyEscaped.size());
  for (size_t I = 0; I < ExpectedOnlyEscaped.size(); ++I)
    EXPECT_EQ(ActualOnlyEscaped[I], ExpectedOnlyEscaped[I]) << "index " << I;
}

TEST_F(X86LegalizerDifferentialTest, ReferenceBehaviorFingerprint) {
  auto ProvText = readTestSourceFile("x86-legalizer-reference-provenance.json");
  ASSERT_TRUE(ProvText.has_value());
  auto Prov = json::parse(*ProvText);
  ASSERT_TRUE(!!Prov) << toString(Prov.takeError());
  const json::Object *P = Prov->getAsObject();
  ASSERT_NE(P, nullptr);
  auto Want = P->getString("reference_behavior_sha256");
  ASSERT_TRUE(Want.has_value());

  auto QueriesText = readTestSourceFile("x86-legalizer-behavior-queries.json");
  ASSERT_TRUE(QueriesText.has_value());
  auto QueriesValue = json::parse(*QueriesText);
  ASSERT_TRUE(!!QueriesValue) << toString(QueriesValue.takeError());
  const json::Object *Q = QueriesValue->getAsObject();
  ASSERT_NE(Q, nullptr);
  const json::Array *QueryArray = Q->getArray("queries");
  ASSERT_NE(QueryArray, nullptr);

  std::vector<std::pair<std::string, const json::Object *>> Sorted;
  for (const json::Value &V : *QueryArray) {
    const json::Object *O = V.getAsObject();
    ASSERT_NE(O, nullptr);
    auto Id = O->getString("id");
    ASSERT_TRUE(Id.has_value());
    Sorted.emplace_back(Id->str(), O);
  }
  std::sort(Sorted.begin(), Sorted.end(),
            [](const auto &A, const auto &B) { return A.first < B.first; });

  std::map<std::string, Sigma> Sigmas;
  std::map<std::string, std::unique_ptr<X86LegalizerInfoReference>> Oracles;
  std::string Encoded;
  raw_string_ostream OS(Encoded);
  for (const auto &Entry : Sorted) {
    auto BQ = parseBehaviorQuery(*Entry.second);
    ASSERT_TRUE(BQ.has_value()) << Entry.first;
    const Config *C = findConfig(BQ->Config);
    ASSERT_NE(C, nullptr) << BQ->Config;

    auto SigmaIt = Sigmas.find(BQ->Config);
    if (SigmaIt == Sigmas.end())
      SigmaIt = Sigmas.emplace(BQ->Config, makeSigma(*C)).first;
    ASSERT_TRUE(SigmaIt->second.STI) << BQ->Config;

    auto &Oracle = Oracles[BQ->Config];
    if (!Oracle)
      Oracle = std::make_unique<X86LegalizerInfoReference>(
          *SigmaIt->second.STI,
          *static_cast<const X86TargetMachine *>(SigmaIt->second.TM.get()));

    SmallVector<LLT, 3> Types;
    SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
    auto Opcode = buildBehaviorQuery(*BQ, C->Is64 ? 64 : 32, Types, MemDescs);
    ASSERT_TRUE(Opcode.has_value()) << BQ->Opcode;
    LegalityQuery Query(*Opcode, Types, MemDescs);

    LegalizeActionStep Step = Oracle->getAction(Query);
    std::string ActionText;
    raw_string_ostream ActionOS(ActionText);
    ActionOS << Step.Action;
    ActionOS.flush();
    std::string NewType =
        Step.NewType.isValid() ? jsonLLTName(Step.NewType) : "none";
    OS << "{\"id\":\"" << BQ->Id << "\",\"action\":\"" << ActionText
       << "\",\"type_idx\":" << Step.TypeIdx << ",\"new_type\":\"" << NewType
       << "\"}\n";
  }
  OS.flush();

  const std::string Computed = sha256Hex(Encoded);
  if (Computed != Want->str())
    ADD_FAILURE() << "computed=" << Computed;
  EXPECT_EQ(Computed, Want->str());
}

TEST_F(X86LegalizerDifferentialTest, BehaviorQueryHitPointsAreTableHits) {
  auto QueriesText = readTestSourceFile("x86-legalizer-behavior-queries.json");
  ASSERT_TRUE(QueriesText.has_value());
  auto QueriesValue = json::parse(*QueriesText);
  ASSERT_TRUE(!!QueriesValue) << toString(QueriesValue.takeError());
  const json::Object *Q = QueriesValue->getAsObject();
  ASSERT_NE(Q, nullptr);
  const json::Array *QueryArray = Q->getArray("queries");
  ASSERT_NE(QueryArray, nullptr);

  std::map<std::string, Sigma> Sigmas;
  std::map<std::string, std::unique_ptr<TableLegalizerInfoForTest>> Tables;
  unsigned Checked = 0;
  for (const json::Value &V : *QueryArray) {
    const json::Object *O = V.getAsObject();
    ASSERT_NE(O, nullptr);
    auto BQ = parseBehaviorQuery(*O);
    ASSERT_TRUE(BQ.has_value());
    if (BQ->Kind != "hit")
      continue;

    const Config *C = findConfig(BQ->Config);
    ASSERT_NE(C, nullptr) << BQ->Config;
    auto SigmaIt = Sigmas.find(BQ->Config);
    if (SigmaIt == Sigmas.end())
      SigmaIt = Sigmas.emplace(BQ->Config, makeSigma(*C)).first;
    ASSERT_TRUE(SigmaIt->second.STI) << BQ->Config;

    auto &Table = Tables[BQ->Config];
    if (!Table)
      Table = std::make_unique<TableLegalizerInfoForTest>(*SigmaIt->second.STI);

    SmallVector<LLT, 3> Types;
    SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
    auto Opcode = buildBehaviorQuery(*BQ, C->Is64 ? 64 : 32, Types, MemDescs);
    ASSERT_TRUE(Opcode.has_value()) << BQ->Opcode;
    LegalityQuery Query(*Opcode, Types, MemDescs);
    X86LegalizerTable::Result R = Table->table().query(Query);
    EXPECT_EQ(R.Kind, X86LegalizerTable::Answer::Hit) << BQ->Id;
    ++Checked;
  }
  EXPECT_EQ(Checked, 63u);
}

TEST_F(X86LegalizerDifferentialTest, FeatureBoolsPerConfig) {
  for (const Config &C : Configs) {
    Sigma S = makeSigma(C);
    ASSERT_TRUE(S.STI) << C.ID;
    EXPECT_EQ(S.STI->is64Bit(), C.Is64) << C.ID;
    EXPECT_EQ(S.STI->hasAVX512(), C.AVX512) << C.ID;
    EXPECT_EQ(S.STI->hasVPOPCNTDQ(), C.VPOPCNTDQ) << C.ID;
    EXPECT_EQ(S.STI->hasVLX(), C.VLX) << C.ID;
    EXPECT_EQ(S.STI->hasPOPCNT(), C.POPCNT) << C.ID;
  }
}

// FNV-1a over the canonical rendering of every atom in table order.  The
// expected value lives in x86-ctpop-atoms.json; CMake extracts it into the
// generated header above, so any drift in this table breaks the build's test.
static uint64_t canonicalAtomHash() {
  uint64_t H = 1469598103934665603ull;
  auto Mix = [&](StringRef S) {
    for (unsigned char C : S) {
      H ^= C;
      H *= 1099511628211ull;
    }
  };
  std::string Line;
  raw_string_ostream OS(Line);
  for (const Config &C : Configs)
    for (const TypeClass &T : typeClasses()) {
      OS << C.ID << "|G_CTPOP|" << shortLLT(T.Dst) << "|" << shortLLT(T.Src)
         << "|none|unused\n";
      OS.flush();
      Mix(Line);
      Line.clear();
    }
  return H;
}

// Falsification of the comparator itself.  Each case flips exactly one field
// and must be reported as a difference; a comparator that ignored any one of
// them would make the differential silently vacuous.
TEST_F(X86LegalizerDifferentialTest, ComparatorDetectsEachField) {
  const LLT S16 = LLT::scalar(16), S32 = LLT::scalar(32), S64 = LLT::scalar(64);
  LegalizeActionStep Base(WidenScalar, 0, S32);
  EXPECT_TRUE(stepsAgree(Base, Base));

  LegalizeActionStep OtherAction(Legal, 0, S32);
  EXPECT_FALSE(stepsAgree(Base, OtherAction)) << "Action flip not detected";

  LegalizeActionStep OtherIdx(WidenScalar, 1, S32);
  EXPECT_FALSE(stepsAgree(Base, OtherIdx)) << "TypeIdx flip not detected";

  LegalizeActionStep OtherTy(WidenScalar, 0, S64);
  EXPECT_FALSE(stepsAgree(Base, OtherTy)) << "NewType flip not detected";

  EXPECT_FALSE(stepsAgree(Base, LegalizeActionStep(WidenScalar, 0, S16)))
      << "NewType comparison must be exact";
}

// The JSON is the reviewed source of truth; the C++ tables are its hand
// transcription.  The hash covers the atom identities only, so every field is
// also compared directly: a sigma whose feature string drifted, or a type whose
// LLT changed while keeping its ID, would otherwise pass.
TEST_F(X86LegalizerDifferentialTest, FrozenCorpusMatchesJson) {
  EXPECT_EQ(typeClasses().size() * std::size(Configs),
            (size_t)X86_CTPOP_ATOM_COUNT);
  char Buf[32];
  snprintf(Buf, sizeof(Buf), "%016llx",
           (unsigned long long)canonicalAtomHash());
  EXPECT_STREQ(X86_CTPOP_ATOMS_FNV1A64, Buf)
      << "atom table drifted from x86-ctpop-atoms.json";

  ASSERT_EQ(std::size(X86_CTPOP_SIGMAS), std::size(Configs));
  for (size_t I = 0; I != std::size(Configs); ++I) {
    const X86AtomSigma &J = X86_CTPOP_SIGMAS[I];
    const Config &C = Configs[I];
    EXPECT_STREQ(J.ID, C.ID);
    EXPECT_STREQ(J.Triple, C.Triple) << C.ID;
    EXPECT_STREQ(J.Features, C.FS) << C.ID;
    EXPECT_EQ(J.Is64, C.Is64) << C.ID;
    EXPECT_EQ(J.AVX512, C.AVX512) << C.ID;
    EXPECT_EQ(J.VPOPCNTDQ, C.VPOPCNTDQ) << C.ID;
    EXPECT_EQ(J.VLX, C.VLX) << C.ID;
    EXPECT_EQ(J.POPCNT, C.POPCNT) << C.ID;
  }

  std::vector<TypeClass> Types = typeClasses();
  ASSERT_EQ(std::size(X86_CTPOP_TYPES), Types.size());
  for (size_t I = 0; I != Types.size(); ++I) {
    const X86AtomType &J = X86_CTPOP_TYPES[I];
    const TypeClass &T = Types[I];
    EXPECT_STREQ(J.ID, T.ID);
    EXPECT_EQ(J.Dst, shortLLT(T.Dst)) << T.ID;
    EXPECT_EQ(J.Src, shortLLT(T.Src)) << T.ID;
  }
}

//===----------------------------------------------------------------------===//
// Report
//===----------------------------------------------------------------------===//
//
// One JSON object per atom in corpus order, then a summary object.  Nothing
// here reads the clock, the filesystem, or a hash table's iteration order, so
// two runs in one process are byte-identical (TC-27) and the same text is what
// a `--json` CLI would print (TC-26).

// A step's new type can be a pointer vector (G_IMPLICIT_DEF clamps one), so
// it is spelled with the same full-fidelity LLT names as a query.  The
// non-pointer spellings are identical to shortLLT's.
static std::string witnessLLTName(LLT T);

static void renderStepJson(raw_ostream &OS, const LegalizeActionStep &S) {
  OS << "{\"action\":\"" << S.Action << "\"";
  if (actionCarriesType(S.Action))
    OS << ",\"idx\":" << S.TypeIdx << ",\"to\":\"" << witnessLLTName(S.NewType)
       << "\"";
  OS << "}";
}

struct Report {
  std::string Text;
  std::string Diagnostics;
  unsigned Atoms = 0;
  unsigned Diffs = 0;
  unsigned LegalBoth = 0;
  unsigned UnsupportedBoth = 0;
  unsigned Customs = 0;
};

static Report renderReport() {
  Report R;
  raw_string_ostream Json(R.Text);
  raw_string_ostream Diag(R.Diagnostics);
  for (const Config &C : Configs) {
    Sigma S = makeSigma(C);
    EXPECT_TRUE(S.STI) << "no subtarget for " << C.ID;
    if (!S.STI)
      continue;
    X86LegalizerInfoReference Oracle(
        *S.STI, *static_cast<const X86TargetMachine *>(S.TM.get()));
    GeneratedLegalizerInfoForTest SUT(*S.STI);
    for (const TypeClass &T : typeClasses()) {
      LLT Types[] = {T.Dst, T.Src};
      LegalityQuery Q(G_CTPOP, Types);
      LegalizeActionStep A = Oracle.getAction(Q);
      LegalizeActionStep B = SUT.getAction(Q);
      ++R.Atoms;
      if (A.Action == Legal && B.Action == Legal)
        ++R.LegalBoth;
      if (A.Action == Unsupported && B.Action == Unsupported)
        ++R.UnsupportedBoth;
      const bool IsCustom = A.Action == Custom || B.Action == Custom;
      if (IsCustom) {
        ++R.Customs;
        Diag << "CUSTOM " << C.ID << "/" << T.ID << " oracle=" << A.Action
             << " sut=" << B.Action << "\n";
      }
      const bool Same = stepsAgree(A, B);
      if (!Same)
        ++R.Diffs;

      Json << "{\"atom\":\"x86/g_ctpop/" << C.ID << "/" << T.ID
           << "\",\"sigma\":\"" << C.FS
           << "\",\"opcode\":\"G_CTPOP\",\"types\":[\"" << shortLLT(T.Dst)
           << "\",\"" << shortLLT(T.Src) << "\"],\"expected\":";
      renderStepJson(Json, A);
      Json << ",\"actual\":";
      renderStepJson(Json, B);
      Json << ",\"status\":\"" << (Same ? "ok" : "diff")
           << "\",\"custom\":" << (IsCustom ? "true" : "false") << "}\n";

      if (!Same)
        Diag << "FAIL x86/g_ctpop/" << C.ID << "/" << T.ID
             << "\n  sigma:    " << C.Triple << " / " << C.FS
             << "\n  query:    G_CTPOP (" << T.Dst << ", " << T.Src
             << ")\n  expected: " << describe(A)
             << "\n  actual:   " << describe(B) << "\n";
    }
  }
  Json << "{\"atoms\":" << R.Atoms << ",\"diffs\":" << R.Diffs
       << ",\"legal_both\":" << R.LegalBoth
       << ",\"unsupported_both\":" << R.UnsupportedBoth
       << ",\"custom\":" << R.Customs << "}\n";
  return R;
}

TEST_F(X86LegalizerDifferentialTest, FirstStepDifferential) {
  Report R = renderReport();
  EXPECT_EQ(R.Atoms, 126u);
  EXPECT_GT(R.LegalBoth, 0u) << "corpus has no jointly-legal atom";
  EXPECT_GT(R.UnsupportedBoth, 0u) << "corpus has no jointly-unsupported atom";
  // FR-8: both fixtures stub legalizeCustom out, so a Custom action would mean
  // the table asked for a C++ callback and the differential silently stopped
  // comparing.
  EXPECT_EQ(R.Customs, 0u) << "the tables answered Custom\n" << R.Diagnostics;
  EXPECT_EQ(R.Diffs, 0u) << R.Diagnostics;
}

// The table must agree with the oracle on every atom it claims to answer.
// Declines are allowed -- the class list deliberately does not cover odd
// scalar widths -- but they are counted, so a table that silently stopped
// hitting most of the corpus would fail instead of looking clean.
TEST_F(X86LegalizerDifferentialTest, TableAnswersCorpus) {
  unsigned Hits = 0, Declines = 0, Diffs = 0;
  std::string Diagnostics;
  raw_string_ostream Diag(Diagnostics);
  for (const Config &C : Configs) {
    Sigma S = makeSigma(C);
    ASSERT_TRUE(S.STI) << C.ID;
    X86LegalizerInfoReference Oracle(
        *S.STI, *static_cast<const X86TargetMachine *>(S.TM.get()));
    TableLegalizerInfoForTest Table(*S.STI);
    for (const TypeClass &T : typeClasses()) {
      LLT Types[] = {T.Dst, T.Src};
      LegalityQuery Q(G_CTPOP, Types);
      X86LegalizerTable::Result R = Table.table().query(Q);
      if (R.Kind != X86LegalizerTable::Answer::Hit) {
        ++Declines;
        continue;
      }
      ++Hits;
      LegalizeActionStep Got(R.Action, R.TypeIdx, R.NewType);
      LegalizeActionStep Want = Oracle.getAction(Q);
      if (stepsAgree(Got, Want))
        continue;
      ++Diffs;
      Diag << C.ID << "/" << T.ID << ": table " << describe(Got) << ", oracle "
           << describe(Want) << "\n";
    }
  }
  EXPECT_EQ(Hits + Declines, 126u);
  EXPECT_EQ(Diffs, 0u) << Diag.str();
  // s7, s24 and s200 are the only corpus types outside the class list, so the
  // remaining 105 atoms must be answered by the table, not by a fallback.
  EXPECT_EQ(Hits, 105u)
      << "table declined more atoms than the 21 non-class ones";
  EXPECT_EQ(Declines, 21u);
}

// One G_STORE probe shared by the address-space tests: the table result and the
// oracle answer for the same query, with the memory descriptor alignment the
// generated points use.
struct StoreProbe {
  X86LegalizerTable::Result TableResult;
  LegalizeActionStep OracleStep;
};

static StoreProbe probeStore(const X86LegalizerTable &Table,
                             const X86LegalizerInfoReference &Oracle,
                             LLT ValueTy, LLT PtrTy, LLT MemTy) {
  LLT Types[] = {ValueTy, PtrTy};
  LegalityQuery::MemDesc MemDescs[] = {{MemTy, /*AlignInBits=*/8,
                                        AtomicOrdering::NotAtomic,
                                        AtomicOrdering::NotAtomic}};
  LegalityQuery Q(G_STORE, Types, MemDescs);
  return {Table.query(Q), Oracle.getAction(Q)};
}

// Every enumerated domain point must agree with the rule chain.  selfCheck()
// reports a mismatch with report_fatal_error, so the count assertions here are
// the non-fatal half of the same contract.
TEST_F(X86LegalizerDifferentialTest, SelfCheckDomainSmoke) {
  const Config &C = Configs[0];
  Sigma S = makeSigma(C);
  ASSERT_TRUE(S.STI) << C.ID;
  TableLegalizerInfoForTest Table(*S.STI);

  std::string Log;
  raw_string_ostream OS(Log);
  Table.mutableTable().selfCheck(OS);
  OS.flush();

  EXPECT_NE(Log.find("points 1397"), std::string::npos) << Log;
  EXPECT_NE(Log.find("mismatches 0"), std::string::npos) << Log;
  EXPECT_NE(Log.find("skipped 0"), std::string::npos) << Log;
}

// The rule index must agree with the reference, not just the table: it is a
// second emission of the same .td rules, and if its guards drifted the step it
// reports would drift with them.  A query the evaluator declines must be one
// the table declines too, so a decline cannot hide a missing rule.
TEST_F(X86LegalizerDifferentialTest, RuleIndexAgreesWithReference) {
  // C0 has neither AVX nor AVX-512, C2 has both: the feature-guarded rules the
  // rule index emits must be exercised, not just the unconditional ones.
  unsigned Matched = 0, NoRule = 0, Opaque = 0, OutOfRange = 0, Diffs = 0;
  std::string Diagnostics;
  raw_string_ostream Diag(Diagnostics);
  for (const Config *C : {&Configs[0], &Configs[2]}) {
    Sigma S = makeSigma(*C);
    ASSERT_TRUE(S.STI) << C->ID;
    TableLegalizerInfoForTest Table(*S.STI);
    X86LegalizerInfoReference Oracle(
        *S.STI, *static_cast<const X86TargetMachine *>(S.TM.get()));

    SmallVector<X86TablePoint, 0> Points;
    Points.reserve(X86NumTablePoints);
    x86CollectTablePoints(Points, S.STI->isTarget64BitLP64() ? 64 : 32);
    ASSERT_EQ(Points.size(), 1397u);

    for (const X86TablePoint &P : Points) {
      ArrayRef<LLT> Types(P.Types, P.NumTypes);
      SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
      if (P.HasMem)
        MemDescs.emplace_back(P.MemTy, P.Align, AtomicOrdering::NotAtomic,
                              AtomicOrdering::NotAtomic);
      const LegalityQuery Q(P.Opcode, Types, MemDescs);

      const int TableID = x86RuleIndexTableForOpcode(P.Opcode);
      ASSERT_GE(TableID, 0) << P.Name;
      const char *Set = X86RuleIndexTableNames[TableID];
      const LegalizeActionStep Want = Oracle.getAction(Q);
      const X86LegalizerTable::Result TableResult = Table.table().query(Q);
      const std::optional<RuleHit> Hit = queryRuleIndex(TableID, *S.STI, Q);

      if (!Hit) {
        // No rule matched.  The table's evaluator is the same translation, so
        // it reports the chain's default; a table that declines instead is
        // opaque for this rule set, which is the other way the index can have
        // no answer.
        if (TableResult.Kind == X86LegalizerTable::Answer::Escape) {
          ++Opaque;
          continue;
        }
        ++NoRule;
        if (Want.Action == Unsupported)
          continue;
        ++Diffs;
        Diag << C->ID << "/" << P.Name << " [" << Set
             << "] no rule matched, oracle " << describe(Want) << "\n";
        continue;
      }

      ++Matched;
      if (Hit->RuleIndex >= X86RuleIndexNumRules[TableID]) {
        ++OutOfRange;
        Diag << C->ID << "/" << P.Name << " [" << Set << "] index "
             << Hit->RuleIndex << " outside [0, "
             << X86RuleIndexNumRules[TableID] << ")\n";
      }
      if (!stepsAgree(Hit->Step, Want)) {
        ++Diffs;
        Diag << C->ID << "/" << P.Name << " [" << Set << "#" << Hit->RuleIndex
             << "] index " << describe(Hit->Step) << ", oracle "
             << describe(Want) << "\n";
      }
      // The index names the rule the table's own evaluator answers with, so the
      // two emissions of the evaluator cannot drift apart.
      if (TableResult.Kind == X86LegalizerTable::Answer::Hit ||
          TableResult.Kind == X86LegalizerTable::Answer::Residual) {
        if (!stepsAgree(Hit->Step, LegalizeActionStep(TableResult.Action,
                                                      TableResult.TypeIdx,
                                                      TableResult.NewType))) {
          ++Diffs;
          Diag << C->ID << "/" << P.Name << " [" << Set << "#" << Hit->RuleIndex
               << "] index " << describe(Hit->Step) << ", table "
               << describe(LegalizeActionStep(TableResult.Action,
                                              TableResult.TypeIdx,
                                              TableResult.NewType))
               << "\n";
        }
      }
    }
  }

  EXPECT_EQ(Matched + NoRule + Opaque, 2 * X86NumTablePoints);
  EXPECT_EQ(Diffs, 0u) << Diag.str();
  EXPECT_EQ(OutOfRange, 0u) << Diag.str();
  // The domain is the table's own: a decline here would mean the rules stopped
  // matching queries the reader still asks.
  EXPECT_EQ(Matched, 2164u) << Diag.str();
  EXPECT_EQ(NoRule, 630u) << Diag.str();
  EXPECT_EQ(Opaque, 0u) << Diag.str();
}

// A rule number that was always zero would still agree on every step above, so
// these probes pin the number to the .td rule that actually wins.  The expected
// values are X86LegalizerInfo.core.json's rule_index for the same rule, which
// is the rule's 0-based position in the rule set's Rules list.
TEST_F(X86LegalizerDifferentialTest, RuleIndexPinsFirstMatch) {
  const Config &C = Configs[0];
  Sigma S = makeSigma(C);
  ASSERT_TRUE(S.STI) << C.ID;
  X86LegalizerInfoReference Oracle(
      *S.STI, *static_cast<const X86TargetMachine *>(S.TM.get()));

  struct Probe {
    const char *Name;
    unsigned Opcode;
    LLT Types[2];
    unsigned NumTypes;
    bool HasMem;
    LLT MemTy;
    unsigned RuleIndex;
    LegalizeActionStep Want;
  } Probes[] = {
      // X86_STORE: the six LegalForMemDesc<Ty, p0, Ty> entries come first, so a
      // value whose type differs from the memory type reaches the opaque
      // `Types[0] != MemoryTy` rule at index 6 and is customized.
      {"G_STORE(s32)[s64]",
       G_STORE,
       {LLT::scalar(32), LLT::pointer(0, 64)},
       2,
       true,
       LLT::scalar(64),
       6,
       LegalizeActionStep(Custom, 0, LLT{})},
      // The same store with a matching memory type is the first rule.
      {"G_STORE(s32)[s32]",
       G_STORE,
       {LLT::scalar(32), LLT::pointer(0, 64)},
       2,
       true,
       LLT::scalar(32),
       0,
       LegalizeActionStep(Legal, 0, LLT{})},
      // X86_ANYEXT: the four legal pairs and the 64-bit one precede the
      // WidenScalarToNextPow2<0, 8> rule, so a non-power-of-two scalar is
      // widened by rule 2.
      {"G_ANYEXT(s48)",
       G_ANYEXT,
       {LLT::scalar(48), LLT::scalar(48)},
       2,
       false,
       LLT{},
       2,
       LegalizeActionStep(WidenScalar, 0, LLT::scalar(64))},
  };

  for (const Probe &P : Probes) {
    ArrayRef<LLT> Types(P.Types, P.NumTypes);
    SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
    if (P.HasMem)
      MemDescs.emplace_back(P.MemTy, /*AlignInBits=*/1,
                            AtomicOrdering::NotAtomic,
                            AtomicOrdering::NotAtomic);
    const LegalityQuery Q(P.Opcode, Types, MemDescs);

    const int TableID = x86RuleIndexTableForOpcode(P.Opcode);
    ASSERT_GE(TableID, 0) << P.Name;
    const std::optional<RuleHit> Hit = queryRuleIndex(TableID, *S.STI, Q);
    ASSERT_TRUE(Hit) << P.Name << " [" << X86RuleIndexTableNames[TableID]
                     << "] matched no rule";
    EXPECT_EQ(Hit->RuleIndex, P.RuleIndex)
        << P.Name << " [" << X86RuleIndexTableNames[TableID] << "]";
    EXPECT_TRUE(stepsAgree(Hit->Step, P.Want))
        << P.Name << ": index " << describe(Hit->Step) << ", want "
        << describe(P.Want);
    EXPECT_TRUE(stepsAgree(P.Want, Oracle.getAction(Q)))
        << P.Name << ": want " << describe(P.Want) << ", oracle "
        << describe(Oracle.getAction(Q));
  }
}

//===----------------------------------------------------------------------===//
// Approved coverage exceptions
//===----------------------------------------------------------------------===//
//
// A literal rule that cannot first-match in the fixed C0-C3 domain is a hole in
// the ledger, so it must be approved explicitly instead of silently skipped.
// The committed file names the rule, why the domain cannot reach it, and the
// evidence.  A missing field or an empty reason/proof is a failure, and so is
// an entry for a rule the domain does reach: a stale exception hides a
// regression exactly like an unexplained miss.

struct LegalizerException {
  std::string Kind;
  std::string RuleSet;
  unsigned RuleIndex = 0;
  std::string Reason;
  std::string Proof;
  std::string LinkedLedger;
  std::string Approver;
  std::string PR;
};

static std::optional<std::vector<LegalizerException>>
readLegalizerExceptions(std::string &Error) {
  std::optional<std::string> Text =
      readTestSourceFile("x86-legalizer-exceptions.json");
  if (!Text) {
    Error = "cannot read x86-legalizer-exceptions.json from " +
            std::string(X86_LEGALIZER_TEST_SRC_DIR);
    return std::nullopt;
  }
  auto Value = json::parse(*Text);
  if (!Value) {
    Error = toString(Value.takeError());
    return std::nullopt;
  }
  const json::Object *Root = Value->getAsObject();
  if (!Root) {
    Error = "top level is not an object";
    return std::nullopt;
  }
  auto Schema = Root->getString("schema");
  if (!Schema || *Schema != "llvm.gisel.legalizer-exceptions") {
    Error = "schema must be llvm.gisel.legalizer-exceptions";
    return std::nullopt;
  }
  auto Version = Root->getInteger("version");
  if (!Version || *Version != 1) {
    Error = "version must be 1";
    return std::nullopt;
  }
  const json::Array *Entries = Root->getArray("exceptions");
  if (!Entries) {
    Error = "missing field 'exceptions'";
    return std::nullopt;
  }

  auto Required = [&](const json::Object &O, StringRef Key,
                      std::string &Out) -> bool {
    auto S = O.getString(Key);
    if (!S) {
      Error = "missing field '" + Key.str() + "'";
      return false;
    }
    Out = S->str();
    return true;
  };

  std::vector<LegalizerException> Out;
  for (const json::Value &V : *Entries) {
    const json::Object *O = V.getAsObject();
    if (!O) {
      Error = "exception is not an object";
      return std::nullopt;
    }
    LegalizerException E;
    if (!Required(*O, "kind", E.Kind) || !Required(*O, "rule_set", E.RuleSet) ||
        !Required(*O, "reason", E.Reason) || !Required(*O, "proof", E.Proof) ||
        !Required(*O, "linked_ledger", E.LinkedLedger) ||
        !Required(*O, "approver", E.Approver) || !Required(*O, "PR", E.PR))
      return std::nullopt;
    auto RuleIndex = O->getInteger("rule_index");
    if (!RuleIndex || *RuleIndex < 0) {
      Error = "missing or negative field 'rule_index'";
      return std::nullopt;
    }
    E.RuleIndex = static_cast<unsigned>(*RuleIndex);
    const std::string Name = E.RuleSet + "#" + std::to_string(E.RuleIndex);
    if (E.Kind != "unreachable") {
      Error =
          Name + ": only kind 'unreachable' is accepted, got '" + E.Kind + "'";
      return std::nullopt;
    }
    if (E.Reason.empty()) {
      Error = Name + ": empty 'reason'";
      return std::nullopt;
    }
    if (E.Proof.empty()) {
      Error = Name + ": empty 'proof'";
      return std::nullopt;
    }
    Out.push_back(E);
  }
  return Out;
}

// Directed guard atoms.  These are not boundary points and do not change the
// domain counts; they exist so a rule whose guard is only reachable on a shape
// outside the 12-atom panel still earns a first-match witness.
// X86_BUILD_VECTOR#5 (numElementsNotPow2) needs a non-power-of-two lane count
// whose element type no earlier clamp rule names.  v3s1 and v3p0 are the two
// such shapes; the clamp rules only name s8/s16/s32/s64 elements.
struct GuardAtom {
  const char *Opcode;
  LLT Type;
};

static ArrayRef<GuardAtom> guardAtoms() {
  static const GuardAtom Atoms[] = {
      {"G_BUILD_VECTOR", LLT::fixed_vector(3, 1)},
      {"G_BUILD_VECTOR", LLT::fixed_vector(3, LLT::pointer(0, 64))},
      // The escaped clamp rules name lane counts outside the class list.  On
      // the low side, G_ADD#7 and G_MUL#9 want an s16 vector narrower than 8
      // lanes; on the high side, G_MUL#14 clamps an s64 vector wider than 8.
      // The class list starts at v8s16 and stops at v8s64, so these are the
      // first shapes each guard can fire on.
      {"G_ADD", LLT::fixed_vector(2, 16)},
      {"G_MUL", LLT::fixed_vector(2, 16)},
      {"G_MUL", LLT::fixed_vector(16, 64)},
  };
  return Atoms;
}

using RuleKey = std::pair<unsigned, unsigned>;

struct LiteralTarget {
  unsigned Table = 0;
  unsigned RuleIndex = 0;
  std::string Set;
  int64_t Line = 0;
};

// The literal denominator: every (rule set, rule_index) pair in the emitted
// Core IR.  The ledger and the witness generator must agree on it, so both
// read it here instead of each parsing the file.
static std::optional<std::map<RuleKey, LiteralTarget>>
readLiteralTargets(std::string &Error) {
  auto CoreBuffer = MemoryBuffer::getFile(X86_LEGALIZER_CORE_JSON);
  if (!CoreBuffer) {
    Error = std::string("cannot read ") + X86_LEGALIZER_CORE_JSON;
    return std::nullopt;
  }
  auto CoreValue = json::parse((*CoreBuffer)->getBuffer());
  if (!CoreValue) {
    Error = toString(CoreValue.takeError());
    return std::nullopt;
  }
  const json::Object *Core = CoreValue->getAsObject();
  if (!Core) {
    Error = "core.json top level is not an object";
    return std::nullopt;
  }
  const json::Array *Rulesets = Core->getArray("rulesets");
  if (!Rulesets) {
    Error = "core.json has no 'rulesets'";
    return std::nullopt;
  }

  std::map<std::string, unsigned> TableForSet;
  for (unsigned I = 0; I != X86TestNumTables; ++I)
    TableForSet.emplace(X86RuleIndexTableNames[I], I);

  std::map<RuleKey, LiteralTarget> Targets;
  for (const json::Value &RulesetValue : *Rulesets) {
    const json::Object *Ruleset = RulesetValue.getAsObject();
    if (!Ruleset) {
      Error = "ruleset is not an object";
      return std::nullopt;
    }
    auto Def = Ruleset->getString("def");
    if (!Def) {
      Error = "ruleset has no 'def'";
      return std::nullopt;
    }
    auto TableIt = TableForSet.find(Def->str());
    if (TableIt == TableForSet.end()) {
      Error = "ruleset names an unknown table: " + Def->str();
      return std::nullopt;
    }
    const json::Array *Rules = Ruleset->getArray("rules");
    if (!Rules) {
      Error = "ruleset has no 'rules'";
      return std::nullopt;
    }
    for (const json::Value &RuleValue : *Rules) {
      const json::Object *Rule = RuleValue.getAsObject();
      if (!Rule) {
        Error = "rule is not an object";
        return std::nullopt;
      }
      auto RuleIndex = Rule->getInteger("rule_index");
      if (!RuleIndex || *RuleIndex < 0) {
        Error = "rule has no non-negative 'rule_index'";
        return std::nullopt;
      }
      int64_t Line = 0;
      if (const json::Object *Origin = Rule->getObject("origin"))
        Line = Origin->getInteger("line").value_or(0);
      const RuleKey Key{TableIt->second, static_cast<unsigned>(*RuleIndex)};
      Targets.emplace(Key,
                      LiteralTarget{Key.first, Key.second, Def->str(), Line});
    }
  }
  return Targets;
}

// One escaped rule: the .td rule could not be written as a literal Core IR
// entry, so it gets a descriptor atom instead of a per-rule first-match atom.
struct EscapeTarget {
  std::string RuleSet;
  unsigned RuleIndex = 0;
};

// The escape list is the other half of the .td denominator.  The literal
// manifest names 189 rules; these 93 are the same rule sets' remaining rules,
// named by the (def, rule_index) pair the Core IR records.
static std::optional<std::vector<EscapeTarget>>
readEscapeTargets(std::string &Error) {
  auto CoreBuffer = MemoryBuffer::getFile(X86_LEGALIZER_CORE_JSON);
  if (!CoreBuffer) {
    Error = std::string("cannot read ") + X86_LEGALIZER_CORE_JSON;
    return std::nullopt;
  }
  auto CoreValue = json::parse((*CoreBuffer)->getBuffer());
  if (!CoreValue) {
    Error = toString(CoreValue.takeError());
    return std::nullopt;
  }
  const json::Object *Core = CoreValue->getAsObject();
  if (!Core) {
    Error = "core.json top level is not an object";
    return std::nullopt;
  }
  const json::Array *Escapes = Core->getArray("escapes");
  if (!Escapes) {
    Error = "core.json has no 'escapes'";
    return std::nullopt;
  }

  std::vector<EscapeTarget> Targets;
  Targets.reserve(Escapes->size());
  for (const json::Value &EscapeValue : *Escapes) {
    const json::Object *Escape = EscapeValue.getAsObject();
    if (!Escape) {
      Error = "escape is not an object";
      return std::nullopt;
    }
    auto RuleSet = Escape->getString("ruleset");
    if (!RuleSet) {
      Error = "escape has no 'ruleset'";
      return std::nullopt;
    }
    auto RuleIndex = Escape->getInteger("rule_index");
    if (!RuleIndex || *RuleIndex < 0) {
      Error = "escape has no non-negative 'rule_index'";
      return std::nullopt;
    }
    Targets.push_back(
        EscapeTarget{RuleSet->str(), static_cast<unsigned>(*RuleIndex)});
  }
  return Targets;
}

static bool isEscapedTarget(ArrayRef<EscapeTarget> Escapes, StringRef RuleSet,
                            unsigned RuleIndex) {
  for (const EscapeTarget &E : Escapes)
    if (E.RuleSet == RuleSet && E.RuleIndex == RuleIndex)
      return true;
  return false;
}

struct RuleWitness {
  unsigned Table = 0;
  unsigned RuleIndex = 0;
  unsigned ConfigIndex = 0;
  X86TablePoint Point;
};

struct RuleWitnessLedger {
  std::map<RuleKey, RuleWitness> Witnesses;
  std::map<RuleKey, unsigned> FirstConfig;
  std::array<std::set<RuleKey>, 4> ConfigHits;
};

// The rule-index backend applies the matching rule's mutations while it
// reports the step.  A domain point can put a pointer where a scalar mutation
// is applied, which asserts inside LLT, so the search and its rechecks run
// with mutations off: only the rule identity is under test.
struct RuleIndexMutationsOff {
  bool &Flag;
  explicit RuleIndexMutationsOff(bool &F) : Flag(F) { Flag = false; }
  ~RuleIndexMutationsOff() { Flag = true; }
};

// A mutating rule applied to a pointer type asserts inside LLT
// (changeElementSize).  The full domain deliberately includes such shapes; a
// first-match witness must not, because the witness records the reference's
// decision and the reference applies the mutation.
static bool stepAppliesMutation(const LegalizeActionStep &Step) {
  return actionCarriesType(Step.Action);
}

// The element-size mutations rebuild an LLT whose element type may not be a
// pointer, so they assert on pointer and pointer-vector types.  The
// element-count mutations build a fresh vector instead and accept a pointer
// element type, so they are safe on pointer vectors.
static bool actionChangesElementSize(LegalizeAction Action) {
  return Action == NarrowScalar || Action == WidenScalar;
}

static bool queryHasPointer(ArrayRef<LLT> Types, bool HasMem, LLT MemTy) {
  for (LLT T : Types)
    if (T.isPointer() || T.isPointerVector())
      return true;
  return HasMem && (MemTy.isPointer() || MemTy.isPointerVector());
}

// The first-match search shared by the ledger test and the witness generator.
// The configuration order, the domain walk and the directed guard atoms are
// the ones the ledger documents; extracting them keeps the generated corpus
// and the ledger from disagreeing about which query represents a rule.
static RuleWitnessLedger
collectRuleWitnesses(ArrayRef<Sigma> Sigmas, const std::set<RuleKey> &Targets) {
  RuleWitnessLedger Ledger;
  RuleIndexMutationsOff MutationsOffGuard(X86EvalRuleIndexMutations);

  // The test spec assigns each rule to the first configuration that can
  // first-match it, trying the most capable configuration first: C2, C1, C3,
  // C0.  The hit sets are order-independent; the first-match distribution is
  // this order's.
  static const unsigned ConfigOrder[] = {2, 1, 3, 0};
  for (unsigned OrderIdx = 0; OrderIdx != 4; ++OrderIdx) {
    const unsigned CI = ConfigOrder[OrderIdx];
    const X86Subtarget &STI = *Sigmas[CI].STI;
    for (unsigned TI = 0; TI != X86TestNumTables; ++TI) {
      const X86TestTable &T = X86TestTables[TI];
      const std::optional<unsigned> Opcode = opcodeForName(T.Opcode);
      if (!Opcode) {
        ADD_FAILURE() << "no TargetOpcode for " << T.Opcode;
        continue;
      }

      auto Consider = [&](ArrayRef<LLT> Types, bool HasMem, LLT MemTy,
                          uint64_t Align) {
        EXPECT_LE(Types.size(), 3u);
        SmallVector<LLT, 3> TypeVec(Types.begin(), Types.end());
        SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
        if (HasMem)
          MemDescs.emplace_back(MemTy, Align, AtomicOrdering::NotAtomic,
                                AtomicOrdering::NotAtomic);
        const LegalityQuery Q(*Opcode, TypeVec, MemDescs);
        const std::optional<RuleHit> Hit = queryRuleIndex(TI, STI, Q);
        if (!Hit)
          return;
        EXPECT_EQ(Hit->RuleSet, TI);
        EXPECT_LT(Hit->RuleIndex, X86RuleIndexNumRules[TI]);
        const RuleKey Key{TI, Hit->RuleIndex};
        if (!Targets.count(Key))
          return;
        if (stepAppliesMutation(Hit->Step) &&
            queryHasPointer(Types, HasMem, MemTy))
          return;
        Ledger.ConfigHits[CI].insert(Key);
        Ledger.FirstConfig.emplace(Key, CI);
        if (Ledger.Witnesses.count(Key))
          return;
        RuleWitness W;
        W.Table = TI;
        W.RuleIndex = Hit->RuleIndex;
        W.ConfigIndex = CI;
        W.Point.Opcode = *Opcode;
        W.Point.Name = T.Opcode;
        W.Point.NumTypes = static_cast<uint8_t>(TypeVec.size());
        std::copy(TypeVec.begin(), TypeVec.end(), W.Point.Types);
        W.Point.MemTy = MemTy;
        W.Point.Align = Align;
        W.Point.HasMem = HasMem;
        Ledger.Witnesses.emplace(Key, W);
      };

      enumerateTableDomain(
          TI, 64,
          [&](DomainKind, ArrayRef<LLT> Types, bool HasMem, LLT MemTy,
              uint64_t Align) { Consider(Types, HasMem, MemTy, Align); });

      // Directed guard atoms join the first-match search only.  They are not
      // boundary points, so the domain counts above are unchanged.
      for (const GuardAtom &Atom : guardAtoms())
        if (StringRef(T.Opcode) == Atom.Opcode)
          Consider(ArrayRef<LLT>(&Atom.Type, 1), false, LLT{}, 0);
    }
  }
  return Ledger;
}

// The denominator is the manifest's 189 literal `.td` rules, keyed by the same
// (ruleSet, ruleIndex) the Core IR records.  The ledger records the first-match
// witness of every rule the domain can reach.  A rule that cannot first-match
// must be approved in x86-legalizer-exceptions.json, so a new miss is a hard
// failure rather than a silently reduced denominator.
TEST_F(X86LegalizerDifferentialTest, LiteralRuleFirstMatchLedger) {
  std::string Error;
  std::optional<std::map<RuleKey, LiteralTarget>> Targets =
      readLiteralTargets(Error);
  ASSERT_TRUE(Targets.has_value()) << Error;
  ASSERT_EQ(Targets->size(), 189u)
      << "literal rule denominator must be the manifest's 189 distinct "
         "(def, rule_index) pairs";

  std::map<std::string, unsigned> TableForSet;
  for (unsigned I = 0; I != X86TestNumTables; ++I)
    TableForSet.emplace(X86RuleIndexTableNames[I], I);

  // Approved coverage exceptions come from the committed JSON, not a literal
  // list in this file.  Every entry must have a reason and proof, and only
  // `unreachable` is accepted.  X86_BUILD_VECTOR#5 is deliberately not an
  // exception: the directed guard atom below supplies the non-power-of-two
  // shape the 12-atom panel lacks.
  std::string ExceptionsError;
  std::optional<std::vector<LegalizerException>> Exceptions =
      readLegalizerExceptions(ExceptionsError);
  ASSERT_TRUE(Exceptions.has_value()) << ExceptionsError;
  std::optional<std::vector<EscapeTarget>> EscapeTargets =
      readEscapeTargets(ExceptionsError);
  ASSERT_TRUE(EscapeTargets.has_value()) << ExceptionsError;

  std::map<RuleKey, const LegalizerException *> Approved;
  for (const LegalizerException &E : *Exceptions) {
    auto TableIt = TableForSet.find(E.RuleSet);
    ASSERT_NE(TableIt, TableForSet.end())
        << "exception names an unknown rule set: " << E.RuleSet;
    const RuleKey Key{TableIt->second, E.RuleIndex};
    const bool IsLiteral = Targets->count(Key) != 0;
    const bool IsEscaped =
        isEscapedTarget(*EscapeTargets, E.RuleSet, E.RuleIndex);
    ASSERT_NE(IsLiteral, IsEscaped)
        << "exception names a rule that is neither a literal nor an escaped "
           "rule: "
        << E.RuleSet << "#" << E.RuleIndex;
    // An escaped rule's exception belongs to the descriptor ledger, not the
    // literal first-match ledger.
    if (!IsLiteral)
      continue;
    ASSERT_TRUE(Approved.emplace(Key, &E).second)
        << "duplicate exception for " << E.RuleSet << "#" << E.RuleIndex;
    EXPECT_EQ(E.Kind, "unreachable")
        << E.RuleSet << "#" << E.RuleIndex << " must be kind unreachable";
  }

  std::vector<Sigma> Sigmas;
  Sigmas.reserve(4);
  for (unsigned CI = 0; CI != 4; ++CI) {
    Sigmas.push_back(makeSigma(FullConfigs[CI]));
    ASSERT_TRUE(Sigmas.back().STI) << FullConfigs[CI].ID;
  }

  std::set<RuleKey> TargetKeys;
  for (const auto &Entry : *Targets)
    TargetKeys.insert(Entry.first);
  const RuleWitnessLedger Ledger = collectRuleWitnesses(Sigmas, TargetKeys);
  const auto &Witnesses = Ledger.Witnesses;
  const auto &ConfigHits = Ledger.ConfigHits;
  std::set<RuleKey> Union;
  for (const auto &Entry : Witnesses)
    Union.insert(Entry.first);

  // Every witness must reproduce the rule number it was recorded for.  This
  // catches a ledger that remembers a hit the evaluator does not actually make.
  RuleIndexMutationsOff RecheckGuard(X86EvalRuleIndexMutations);
  unsigned RecheckFailures = 0;
  for (const auto &Entry : Witnesses) {
    const RuleKey &Key = Entry.first;
    const RuleWitness &W = Entry.second;
    const X86Subtarget &STI = *Sigmas[W.ConfigIndex].STI;
    ArrayRef<LLT> Types(W.Point.Types, W.Point.NumTypes);
    SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
    if (W.Point.HasMem)
      MemDescs.emplace_back(W.Point.MemTy, W.Point.Align,
                            AtomicOrdering::NotAtomic,
                            AtomicOrdering::NotAtomic);
    const LegalityQuery Q(W.Point.Opcode, Types, MemDescs);
    const std::optional<RuleHit> Hit = queryRuleIndex(Key.first, STI, Q);
    if (!Hit || Hit->RuleSet != Key.first || Hit->RuleIndex != Key.second) {
      ++RecheckFailures;
      ADD_FAILURE() << "witness " << X86RuleIndexTableNames[Key.first] << "#"
                    << Key.second << " (config "
                    << FullConfigs[W.ConfigIndex].ID
                    << ") no longer reproduces its rule index";
    }
  }

  std::map<unsigned, unsigned> PerSet;
  for (const RuleKey &Key : Union)
    ++PerSet[Key.first];
  unsigned PerSetSum = 0;
  for (const auto &Entry : PerSet)
    PerSetSum += Entry.second;

  llvm::outs() << "literal rule first-match ledger:\n";
  for (unsigned CI = 0; CI != 4; ++CI) {
    unsigned First = 0;
    for (const auto &Entry : Ledger.FirstConfig)
      if (Entry.second == CI)
        ++First;
    std::set<unsigned> HitSets;
    for (const RuleKey &Key : ConfigHits[CI])
      HitSets.insert(Key.first);
    llvm::outs() << "  " << FullConfigs[CI].ID
                 << " hit=" << ConfigHits[CI].size() << "/" << Targets->size()
                 << " first-match=" << First << " sets=" << HitSets.size()
                 << "/" << PerSet.size() << "\n";
  }
  llvm::outs() << "  union=" << Union.size() << "/" << Targets->size()
               << " not-first-match=" << (Targets->size() - Union.size())
               << " per-set-sum=" << PerSetSum << "\n";

  unsigned UnexpectedMissing = 0;
  for (const auto &Entry : *Targets) {
    const bool Hit = Union.count(Entry.first) != 0;
    const bool Known = Approved.count(Entry.first) != 0;
    if (!Hit && !Known) {
      ++UnexpectedMissing;
      const LiteralTarget &T = Entry.second;
      ADD_FAILURE() << "no first-match witness for " << T.Set << "#"
                    << T.RuleIndex << " (.td line " << T.Line
                    << "); no first-match witness over the C0-C3 domain";
      continue;
    }
    if (Hit && Known)
      ADD_FAILURE() << X86RuleIndexTableNames[Entry.first.first] << "#"
                    << Entry.first.second
                    << " is now first-match reachable; update the ledger and "
                       "the manifest";
  }

  llvm::outs() << "  exceptions=" << Approved.size() << ":";
  for (const auto &Entry : Approved)
    llvm::outs() << " " << X86RuleIndexTableNames[Entry.first.first] << "#"
                 << Entry.first.second;
  llvm::outs() << "\n";
  llvm::outs() << "  covered=" << Union.size()
               << " exceptions=" << Approved.size()
               << " denominator=" << Targets->size() << "\n";

  const size_t Reachable = Targets->size() - Approved.size();
  EXPECT_EQ(Union.size(), Reachable);
  EXPECT_EQ(Union.size() + Approved.size(), Targets->size());
  EXPECT_EQ(PerSetSum, Reachable);
  EXPECT_EQ(UnexpectedMissing, 0u);
  EXPECT_EQ(RecheckFailures, 0u);
}

//===----------------------------------------------------------------------===//
// Committed witness corpus
//===----------------------------------------------------------------------===//
//
// The witness is the persistent baseline the default CI walks.  It is
// generated from the frozen reference, never hand-written: every atom records
// the query and the reference's (Action, TypeIdx, NewType), so a change to the
// table shows up as a diff in a checked-in file rather than as a silently
// missing question.
//
// This commit lands the part that can be generated mechanically:
//   188 per-rule first-match atoms
//   756 boundary atoms (63 rule sets x the reviewed 12-atom panel)
//   220 scalar address-space atoms
// The 93 escaped descriptor atoms are deferred: they need a per-rule guard
// search this generator does not perform yet, and inventing queries for them
// would make the corpus claim more than it proves.

// Pointer spellings carry their bit width.  A per-rule witness may be assigned
// to the 32-bit C0 configuration while the search enumerated a 64-bit p0; the
// committed query must rebuild that exact LLT, not a config-dependent one.
static std::string witnessLLTName(LLT T) {
  std::string S;
  raw_string_ostream OS(S);
  if (T.isPointer()) {
    OS << "p" << T.getAddressSpace() << ":" << T.getSizeInBits();
  } else if (T.isPointerVector()) {
    OS << "v" << T.getNumElements() << "p"
       << T.getElementType().getAddressSpace() << ":"
       << T.getElementType().getSizeInBits();
  } else if (T.isVector()) {
    OS << "v" << T.getNumElements() << "s" << T.getScalarSizeInBits();
  } else {
    OS << "s" << T.getSizeInBits();
  }
  return OS.str();
}

static std::optional<LLT> parseWitnessLLT(StringRef S) {
  if (S.consume_front("v")) {
    const size_t Sep = S.find_first_of("ps");
    if (Sep == StringRef::npos)
      return std::nullopt;
    unsigned Lanes = 0;
    if (S.take_front(Sep).getAsInteger(10, Lanes))
      return std::nullopt;
    StringRef Elem = S.drop_front(Sep + 1);
    if (S[Sep] == 's') {
      unsigned Bits = 0;
      if (Elem.getAsInteger(10, Bits))
        return std::nullopt;
      return LLT::fixed_vector(Lanes, Bits);
    }
    const size_t Colon = Elem.find(':');
    unsigned AS = 0, Bits = 0;
    if (Colon == StringRef::npos ||
        Elem.take_front(Colon).getAsInteger(10, AS) ||
        Elem.drop_front(Colon + 1).getAsInteger(10, Bits))
      return std::nullopt;
    return LLT::fixed_vector(Lanes, LLT::pointer(AS, Bits));
  }
  if (S.consume_front("p")) {
    const size_t Colon = S.find(':');
    unsigned AS = 0, Bits = 0;
    if (Colon == StringRef::npos || S.take_front(Colon).getAsInteger(10, AS) ||
        S.drop_front(Colon + 1).getAsInteger(10, Bits))
      return std::nullopt;
    return LLT::pointer(AS, Bits);
  }
  if (S.consume_front("s")) {
    unsigned Bits = 0;
    if (S.getAsInteger(10, Bits))
      return std::nullopt;
    return LLT::scalar(Bits);
  }
  return std::nullopt;
}

static std::string witnessQueryText(const X86TablePoint &P) {
  std::string S;
  raw_string_ostream OS(S);
  OS << P.Name << " (";
  for (unsigned I = 0; I != P.NumTypes; ++I)
    OS << (I ? ", " : "") << witnessLLTName(P.Types[I]);
  OS << ")";
  if (P.HasMem)
    OS << " mem(" << witnessLLTName(P.MemTy) << ", align " << P.Align << ")";
  return OS.str();
}

// Normalized identity of a witness query.  Two atoms with the same key ask
// the same subtarget the same question, so they are the same evidence no
// matter which origin produced them.  The configuration is part of the key:
// the same shape can get a different answer from C0 than from C2.
static std::string witnessQueryKey(StringRef ConfigID, StringRef Opcode,
                                   ArrayRef<LLT> Types, bool HasMem, LLT MemTy,
                                   uint64_t Align) {
  std::string S;
  raw_string_ostream OS(S);
  OS << ConfigID << "|" << Opcode << "|";
  for (const LLT &T : Types)
    OS << witnessLLTName(T) << ",";
  if (HasMem)
    OS << "|mem:" << witnessLLTName(MemTy) << "@" << Align;
  return OS.str();
}

static std::optional<LegalizeAction> parseLegalizeAction(StringRef S) {
  return StringSwitch<std::optional<LegalizeAction>>(S)
      .Case("Legal", Legal)
      .Case("NarrowScalar", NarrowScalar)
      .Case("WidenScalar", WidenScalar)
      .Case("FewerElements", FewerElements)
      .Case("MoreElements", MoreElements)
      .Case("Bitcast", Bitcast)
      .Case("Lower", Lower)
      .Case("Libcall", Libcall)
      .Case("Custom", Custom)
      .Case("Unsupported", Unsupported)
      .Default(std::nullopt);
}

struct WitnessAtom {
  std::string Id;
  std::string Origin;
  std::string RuleSet;
  int64_t RuleIndex = -1;
  std::string ConfigID;
  std::string QueryText;
  X86TablePoint Point;
  LegalizeActionStep Expected{Unsupported, 0, LLT{}};
};

static std::string witnessQueryKey(const WitnessAtom &A) {
  return witnessQueryKey(A.ConfigID, A.Point.Name,
                         ArrayRef<LLT>(A.Point.Types, A.Point.NumTypes),
                         A.Point.HasMem, A.Point.MemTy, A.Point.Align);
}

// Query construction must keep the type and descriptor storage alive, so the
// caller uses the query inside the callback instead of returning one.
template <typename Sink>
static void withAtomQuery(const X86TablePoint &P, Sink &&Use) {
  ArrayRef<LLT> Types(P.Types, P.NumTypes);
  SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
  if (P.HasMem)
    MemDescs.emplace_back(P.MemTy, P.Align, AtomicOrdering::NotAtomic,
                          AtomicOrdering::NotAtomic);
  Use(LegalityQuery(P.Opcode, Types, MemDescs));
}

// Not a test.  Regenerate the witness corpus after a change that is meant to
// be visible in review:
//
//   X86_WITNESS_OUT=<abs path> X86Tests --gtest_also_run_disabled_tests \
//     --gtest_filter='*GenerateWitness*'
//
// The output is deterministic: atoms are sorted by (origin, rule set, rule
// index, query text), no timestamp is written, and the expected decision comes
// from the frozen reference.  Two runs in one tree are byte-identical.
TEST_F(X86LegalizerDifferentialTest, DISABLED_GenerateWitness) {
  const char *OutPath = std::getenv("X86_WITNESS_OUT");
  if (!OutPath)
    GTEST_SKIP() << "set X86_WITNESS_OUT to the absolute output path";

  std::string Error;
  std::optional<std::map<RuleKey, LiteralTarget>> Targets =
      readLiteralTargets(Error);
  ASSERT_TRUE(Targets.has_value()) << Error;
  ASSERT_EQ(Targets->size(), 189u);

  std::optional<std::vector<LegalizerException>> Exceptions =
      readLegalizerExceptions(Error);
  ASSERT_TRUE(Exceptions.has_value()) << Error;

  std::optional<std::vector<EscapeTarget>> Escapes = readEscapeTargets(Error);
  ASSERT_TRUE(Escapes.has_value()) << Error;
  ASSERT_EQ(Escapes->size(), 93u);

  std::map<std::string, unsigned> TableForSet;
  for (unsigned I = 0; I != X86TestNumTables; ++I)
    TableForSet.emplace(X86RuleIndexTableNames[I], I);

  std::vector<Sigma> Sigmas;
  Sigmas.reserve(4);
  for (const Config &C : FullConfigs) {
    Sigmas.push_back(makeSigma(C));
    ASSERT_TRUE(Sigmas.back().STI) << C.ID;
  }

  std::map<std::string, std::unique_ptr<X86LegalizerInfoReference>> Oracles;
  auto OracleFor = [&](unsigned CI) -> X86LegalizerInfoReference & {
    auto &Oracle = Oracles[FullConfigs[CI].ID];
    if (!Oracle)
      Oracle = std::make_unique<X86LegalizerInfoReference>(
          *Sigmas[CI].STI,
          *static_cast<const X86TargetMachine *>(Sigmas[CI].TM.get()));
    return *Oracle;
  };
  auto ExpectFor = [&](const X86TablePoint &P, unsigned CI) {
    LegalizeActionStep Step(Unsupported, 0, LLT{});
    withAtomQuery(
        P, [&](const LegalityQuery &Q) { Step = OracleFor(CI).getAction(Q); });
    return Step;
  };

  std::set<RuleKey> TargetKeys;
  for (const auto &Entry : *Targets)
    TargetKeys.insert(Entry.first);

  const RuleWitnessLedger Ledger = collectRuleWitnesses(Sigmas, TargetKeys);
  ASSERT_EQ(Ledger.Witnesses.size(), 188u);

  std::set<RuleKey> ApprovedLiteral, ApprovedEscaped;
  for (const LegalizerException &E : *Exceptions) {
    auto TableIt = TableForSet.find(E.RuleSet);
    ASSERT_NE(TableIt, TableForSet.end()) << E.RuleSet;
    const RuleKey Key{TableIt->second, E.RuleIndex};
    const bool IsLiteral = Targets->count(Key) != 0;
    const bool IsEscaped = isEscapedTarget(*Escapes, E.RuleSet, E.RuleIndex);
    ASSERT_NE(IsLiteral, IsEscaped)
        << "exception names a rule that is neither a literal nor an escaped "
           "rule: "
        << E.RuleSet << "#" << E.RuleIndex;
    if (IsLiteral) {
      ApprovedLiteral.insert(Key);
      EXPECT_EQ(Ledger.Witnesses.count(Key), 0u)
          << "an approved exception must not also have a first-match witness";
    } else {
      ASSERT_TRUE(ApprovedEscaped.insert(Key).second)
          << "duplicate exception for " << E.RuleSet << "#" << E.RuleIndex;
    }
  }
  ASSERT_EQ(Ledger.Witnesses.size() + ApprovedLiteral.size(), Targets->size())
      << "every literal rule must be either witnessed or approved";

  std::vector<WitnessAtom> Atoms;
  Atoms.reserve(1256);

  // Per-rule atoms: the first query the first-match search found for the rule,
  // in the configuration the search assigned it.
  for (const auto &Entry : Ledger.Witnesses) {
    const RuleWitness &W = Entry.second;
    const X86TestTable &T = X86TestTables[W.Table];
    WitnessAtom A;
    A.Origin = "rule";
    A.RuleSet = T.Name;
    A.RuleIndex = W.RuleIndex;
    A.ConfigID = FullConfigs[W.ConfigIndex].ID;
    A.Point = W.Point;
    A.Id = "x86/rule/" + A.RuleSet + "#" + std::to_string(W.RuleIndex);
    A.QueryText = witnessQueryText(A.Point);
    A.Expected = ExpectFor(A.Point, W.ConfigIndex);
    Atoms.push_back(std::move(A));
  }

  // Boundary and address-space atoms use the default configuration.
  constexpr unsigned DefaultConfig = 2;
  ASSERT_EQ(std::string(FullConfigs[DefaultConfig].ID), "C2");
  const TableDomainAxis Axis = tableDomainAxis(64);
  for (unsigned TI = 0; TI != X86TestNumTables; ++TI) {
    const X86TestTable &T = X86TestTables[TI];
    const std::optional<unsigned> Opcode = opcodeForName(T.Opcode);
    ASSERT_TRUE(Opcode.has_value()) << T.Opcode;

    auto MakePoint = [&](ArrayRef<LLT> Types, bool HasMem, LLT MemTy,
                         uint64_t Align) {
      X86TablePoint P;
      P.Opcode = *Opcode;
      P.Name = T.Opcode;
      P.NumTypes = static_cast<uint8_t>(Types.size());
      std::copy(Types.begin(), Types.end(), P.Types);
      P.MemTy = MemTy;
      P.Align = Align;
      P.HasMem = HasMem;
      return P;
    };

    enumerateWitnessBoundaryPanel(
        T, Axis,
        [&](unsigned PanelIndex, ArrayRef<LLT> Types, bool HasMem, LLT MemTy,
            uint64_t Align) {
          WitnessAtom A;
          A.Origin = "boundary";
          A.RuleSet = T.Name;
          A.ConfigID = FullConfigs[DefaultConfig].ID;
          A.Id = "x86/boundary/" + A.RuleSet + "#" + std::to_string(PanelIndex);
          A.Point = MakePoint(Types, HasMem, MemTy, Align);
          A.QueryText = witnessQueryText(A.Point);
          A.Expected = ExpectFor(A.Point, DefaultConfig);
          Atoms.push_back(std::move(A));
        });

    enumerateWitnessScalarAS(T, Axis,
                             [&](unsigned Serial, ArrayRef<LLT> Types,
                                 bool HasMem, LLT MemTy, uint64_t Align) {
                               WitnessAtom A;
                               A.Origin = "as";
                               A.RuleSet = T.Name;
                               A.ConfigID = FullConfigs[DefaultConfig].ID;
                               A.Id = "x86/as/" + A.RuleSet + "#" +
                                      std::to_string(Serial);
                               A.Point = MakePoint(Types, HasMem, MemTy, Align);
                               A.QueryText = witnessQueryText(A.Point);
                               A.Expected = ExpectFor(A.Point, DefaultConfig);
                               Atoms.push_back(std::move(A));
                             });
  }

  // Descriptor atoms: an escaped rule has no literal Core IR entry, so its
  // evidence is a query that makes the rule's own guard true.  The search
  // walks the full domain plus the directed guard atoms, trying C0..C3 in
  // order, and records the reference's decision for the query it finds.  A
  // descriptor query must not duplicate a per-rule, boundary or address-space
  // atom: two origins for one query would not be independent evidence.
  std::map<std::string, std::string> UsedKeys;
  for (const WitnessAtom &A : Atoms)
    UsedKeys.emplace(witnessQueryKey(A), A.Origin + " " + A.Id);
  unsigned DescriptorAtoms = 0, DescriptorExceptions = 0, Collisions = 0,
           Unmatched = 0;
  // Only the rule identity matters here, so mutations stay off, exactly as in
  // the first-match search.
  RuleIndexMutationsOff MutationsOffGuard(X86EvalRuleIndexMutations);
  for (const EscapeTarget &E : *Escapes) {
    auto TableIt = TableForSet.find(E.RuleSet);
    ASSERT_NE(TableIt, TableForSet.end()) << E.RuleSet;
    const unsigned TI = TableIt->second;
    ASSERT_LT(E.RuleIndex, X86RuleIndexNumRules[TI])
        << E.RuleSet << "#" << E.RuleIndex;
    const X86TestTable &T = X86TestTables[TI];
    const std::optional<unsigned> Opcode = opcodeForName(T.Opcode);
    ASSERT_TRUE(Opcode.has_value()) << T.Opcode;

    bool Found = false;
    for (unsigned CI = 0; CI != 4 && !Found && Collisions == 0; ++CI) {
      const X86Subtarget &STI = *Sigmas[CI].STI;
      auto Consider = [&](ArrayRef<LLT> Types, bool HasMem, LLT MemTy,
                          uint64_t Align) {
        if (Found || Collisions != 0)
          return;
        SmallVector<LLT, 3> TypeVec(Types.begin(), Types.end());
        SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
        if (HasMem)
          MemDescs.emplace_back(MemTy, Align, AtomicOrdering::NotAtomic,
                                AtomicOrdering::NotAtomic);
        const LegalityQuery Q(*Opcode, TypeVec, MemDescs);
        if (!X86EvalRuleGuard(TI, E.RuleIndex, STI, Q))
          return;
        // The atom records the reference's decision, and the reference
        // applies the first match's mutation.  A mutating step on a pointer
        // shape asserts inside LLT, so that shape cannot be a witness.
        const std::optional<RuleHit> Hit = queryRuleIndex(TI, STI, Q);
        if (!Hit)
          return;
        if (actionChangesElementSize(Hit->Step.Action) &&
            queryHasPointer(TypeVec, HasMem, MemTy))
          return;
        WitnessAtom A;
        A.Origin = "escape:" + E.RuleSet + "#" + std::to_string(E.RuleIndex);
        A.RuleSet = E.RuleSet;
        A.RuleIndex = E.RuleIndex;
        A.ConfigID = FullConfigs[CI].ID;
        A.Id = "x86/escape/" + E.RuleSet + "#" + std::to_string(E.RuleIndex);
        A.Point.Opcode = *Opcode;
        A.Point.Name = T.Opcode;
        A.Point.NumTypes = static_cast<uint8_t>(TypeVec.size());
        std::copy(TypeVec.begin(), TypeVec.end(), A.Point.Types);
        A.Point.MemTy = MemTy;
        A.Point.Align = Align;
        A.Point.HasMem = HasMem;
        A.QueryText = witnessQueryText(A.Point);
        A.Expected = ExpectFor(A.Point, CI);
        // Two escaped rules can share a trigger (a clamp's two rules, for
        // example): that is still one atom per rule.  Reusing a per-rule,
        // boundary or AS query is not allowed, because the corpus would then
        // offer one shape as evidence for two different claims.
        const std::string Key = witnessQueryKey(A);
        auto Prev = UsedKeys.find(Key);
        if (Prev != UsedKeys.end()) {
          ++Collisions;
          ADD_FAILURE() << "descriptor collision: " << A.Origin
                        << " reuses the query of " << Prev->second;
          return;
        }
        Atoms.push_back(std::move(A));
        Found = true;
      };
      enumerateTableDomain(
          TI, 64,
          [&](DomainKind, ArrayRef<LLT> Types, bool HasMem, LLT MemTy,
              uint64_t Align) { Consider(Types, HasMem, MemTy, Align); });
      for (const GuardAtom &Atom : guardAtoms())
        if (StringRef(T.Opcode) == Atom.Opcode && !Found)
          Consider(ArrayRef<LLT>(&Atom.Type, 1), false, LLT{}, 0);
    }
    if (Found) {
      ++DescriptorAtoms;
    } else if (Collisions == 0) {
      // An escaped rule whose guard no LLT can satisfy is skipped only when it
      // is registered as an `unreachable` exception.  An unregistered miss is
      // still a hard failure.
      if (ApprovedEscaped.count({TI, E.RuleIndex})) {
        ++DescriptorExceptions;
        llvm::outs() << "descriptor exception: " << E.RuleSet << "#"
                     << E.RuleIndex << "\n";
      } else {
        ++Unmatched;
        ADD_FAILURE() << "no guard trigger for escape " << E.RuleSet << "#"
                      << E.RuleIndex << " in the C0..C3 full domain";
      }
    }
  }
  ASSERT_EQ(Collisions, 0u)
      << "a descriptor query duplicated a per-rule, boundary or AS atom";
  ASSERT_EQ(Unmatched, 0u) << "an escaped rule's guard was not reachable";
  EXPECT_EQ(DescriptorExceptions, ApprovedEscaped.size())
      << "an approved escaped exception did not miss its guard";
  EXPECT_EQ(DescriptorAtoms + DescriptorExceptions, Escapes->size())
      << "every escaped rule must have a descriptor atom or an exception";

  unsigned RuleAtoms = 0, BoundaryAtoms = 0, ASAtoms = 0, DescriptorCount = 0;
  for (const WitnessAtom &A : Atoms) {
    if (A.Origin == "rule")
      ++RuleAtoms;
    else if (A.Origin == "boundary")
      ++BoundaryAtoms;
    else if (A.Origin == "as")
      ++ASAtoms;
    else if (StringRef(A.Origin).starts_with("escape:"))
      ++DescriptorCount;
    else
      ADD_FAILURE() << "unknown witness origin " << A.Origin;
  }
  EXPECT_EQ(RuleAtoms, 188u);
  EXPECT_EQ(BoundaryAtoms, 756u);
  EXPECT_EQ(ASAtoms, 220u);
  EXPECT_EQ(DescriptorCount, DescriptorAtoms);
  EXPECT_EQ(DescriptorCount + DescriptorExceptions, Escapes->size());
  EXPECT_EQ(Atoms.size(), 1256u);

  std::sort(Atoms.begin(), Atoms.end(),
            [](const WitnessAtom &L, const WitnessAtom &R) {
              return std::tie(L.Origin, L.RuleSet, L.RuleIndex, L.QueryText) <
                     std::tie(R.Origin, R.RuleSet, R.RuleIndex, R.QueryText);
            });

  std::optional<std::string> ReferenceText =
      readTestSourceFile("X86LegalizerInfoReference.cpp");
  ASSERT_TRUE(ReferenceText.has_value());
  const std::string ReferenceSha = sha256Hex(*ReferenceText);
  std::optional<std::string> TDText =
      readTestSourceFile("../../../lib/Target/X86/GISel/X86LegalizerInfo.td");
  ASSERT_TRUE(TDText.has_value());
  const std::string TDSha = sha256Hex(*TDText);
  static const char *Policy =
      "witness-v1:per-rule=first-match(C2,C1,C3,C0);boundary=12-panel;"
      "as=slot0+slot1(29)+mem;default=C2";
  const std::string GeneratorHash =
      sha256Hex(std::string(Policy) + "|" + ReferenceSha + "|" + TDSha);

  std::error_code EC;
  raw_fd_ostream OS(OutPath, EC);
  ASSERT_FALSE(EC) << OutPath << ": " << EC.message();

  OS << "{\n";
  OS << "  \"schema\": \"llvm.gisel.x86-legalizer-witness\",\n";
  OS << "  \"version\": 1,\n";
  OS << "  \"comment\": \"Generated from X86LegalizerInfoReference by "
        "DISABLED_GenerateWitness.  A literal rule has a first-match "
        "witness; every escaped rule whose guard an LLT can satisfy has a "
        "descriptor atom.  An escaped rule whose guard no LLT can satisfy is "
        "reported as a descriptor exception when it is approved as "
        "unreachable.\",\n";
  OS << "  \"run\": \"X86_WITNESS_OUT=<abs path> X86Tests "
        "--gtest_also_run_disabled_tests "
        "--gtest_filter='*GenerateWitness*'\",\n";
#ifdef LLVM_REVISION
  OS << "  \"source_commit\": \"" << LLVM_REVISION << "\",\n";
#else
  OS << "  \"source_commit\": \"unknown\",\n";
#endif
  OS << "  \"reference_sha256\": \"" << ReferenceSha << "\",\n";
  OS << "  \"generator_hash\": \"" << GeneratorHash << "\",\n";
  OS << "  \"atom_count\": " << Atoms.size() << ",\n";
  OS << "  \"per_rule_hits\": " << RuleAtoms << ",\n";
  OS << "  \"boundary_atoms\": " << BoundaryAtoms << ",\n";
  OS << "  \"as_atoms\": " << ASAtoms << ",\n";
  OS << "  \"descriptor_atoms\": " << DescriptorCount << ",\n";
  OS << "  \"descriptor_exceptions\": " << DescriptorExceptions << ",\n";
  OS << "  \"witness_config_default\": \"C2\",\n";
  OS << "  \"atoms\": [\n";
  for (size_t I = 0; I != Atoms.size(); ++I) {
    const WitnessAtom &A = Atoms[I];
    OS << "    {\"id\":\"" << A.Id << "\",\"origin\":\"" << A.Origin
       << "\",\"rule_set\":\"" << A.RuleSet << "\",\"rule_index\":";
    if (A.RuleIndex >= 0)
      OS << A.RuleIndex;
    else
      OS << "null";
    if (StringRef(A.Origin).starts_with("escape:"))
      OS << ",\"basis\":\"escape\"";
    OS << ",\"config\":\"" << A.ConfigID << "\",\"query\":{\"types\":[";
    for (unsigned T = 0; T != A.Point.NumTypes; ++T)
      OS << (T ? "," : "") << "\"" << witnessLLTName(A.Point.Types[T]) << "\"";
    OS << "]";
    if (A.Point.HasMem)
      OS << ",\"mem\":{\"type\":\"" << witnessLLTName(A.Point.MemTy)
         << "\",\"align\":" << A.Point.Align << "}";
    OS << "},\"expected\":";
    renderStepJson(OS, A.Expected);
    OS << "}" << (I + 1 == Atoms.size() ? "\n" : ",\n");
  }
  OS << "  ]\n}\n";
  OS.flush();
  ASSERT_FALSE(OS.has_error()) << OutPath;
  llvm::outs() << "wrote " << Atoms.size()
               << " witness atoms (descriptor=" << DescriptorCount
               << " exceptions=" << DescriptorExceptions
               << " collisions=" << Collisions << ") to " << OutPath << "\n";
}

// T1: every committed atom must get the reference's decision from the table.
// The witness is the default-CI baseline, so this test is the one that turns
// the generated corpus into a gate: it rebuilds each query from the committed
// (types, memdesc, config) triple, asks the reader's table, and compares the
// three fields with the frozen reference.  The committed `expected` is checked
// too, so a corpus regenerated from a drifted reference cannot pass unnoticed.
TEST_F(X86LegalizerDifferentialTest, WitnessDecisionDifferential) {
  std::optional<std::string> Text =
      readTestSourceFile("x86-legalizer-witness.json");
  ASSERT_TRUE(Text.has_value())
      << "cannot read x86-legalizer-witness.json from "
      << X86_LEGALIZER_TEST_SRC_DIR;
  auto Value = json::parse(*Text);
  ASSERT_TRUE(!!Value) << toString(Value.takeError());
  const json::Object *Root = Value->getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getInteger("atom_count").value_or(-1), 1256);
  EXPECT_EQ(Root->getInteger("per_rule_hits").value_or(-1), 188);
  EXPECT_EQ(Root->getInteger("boundary_atoms").value_or(-1), 756);
  EXPECT_EQ(Root->getInteger("as_atoms").value_or(-1), 220);
  EXPECT_EQ(Root->getInteger("descriptor_atoms").value_or(-1), 92);
  EXPECT_EQ(Root->getInteger("descriptor_exceptions").value_or(-1), 1);
  EXPECT_EQ(Root->getString("witness_config_default").value_or(""), "C2");

  const json::Array *AtomArray = Root->getArray("atoms");
  ASSERT_NE(AtomArray, nullptr);
  EXPECT_EQ(AtomArray->size(), 1256u);

  std::string Error;
  std::optional<std::map<RuleKey, LiteralTarget>> Targets =
      readLiteralTargets(Error);
  ASSERT_TRUE(Targets.has_value()) << Error;
  std::optional<std::vector<LegalizerException>> Exceptions =
      readLegalizerExceptions(Error);
  ASSERT_TRUE(Exceptions.has_value()) << Error;
  std::optional<std::vector<EscapeTarget>> EscapeTargets =
      readEscapeTargets(Error);
  ASSERT_TRUE(EscapeTargets.has_value()) << Error;

  std::map<std::string, unsigned> TableForSet;
  for (unsigned I = 0; I != X86TestNumTables; ++I)
    TableForSet.emplace(X86RuleIndexTableNames[I], I);

  // Every escaped rule is the denominator of the descriptor slice.  An
  // escaped rule whose guard no LLT can satisfy must be approved as
  // `unreachable` in the exceptions file; it is then removed from the expected
  // descriptor set instead of being named here.  A miss without an approved
  // exception is a hard failure.
  std::set<RuleKey> ExpectedEscapePairs;
  for (const EscapeTarget &E : *EscapeTargets) {
    auto TableIt = TableForSet.find(E.RuleSet);
    ASSERT_NE(TableIt, TableForSet.end()) << E.RuleSet;
    ExpectedEscapePairs.insert({TableIt->second, E.RuleIndex});
  }
  ASSERT_EQ(ExpectedEscapePairs.size(), 93u);

  // The manifest's literal rules minus every approved literal exception is the
  // set of rules a per-rule atom must name; approved escaped exceptions leave
  // the descriptor denominator instead.
  std::set<RuleKey> ExpectedRulePairs;
  for (const auto &Entry : *Targets)
    ExpectedRulePairs.insert(Entry.first);
  std::set<RuleKey> ApprovedEscapePairs;
  for (const LegalizerException &E : *Exceptions) {
    auto TableIt = TableForSet.find(E.RuleSet);
    ASSERT_NE(TableIt, TableForSet.end()) << E.RuleSet;
    const RuleKey Key{TableIt->second, E.RuleIndex};
    if (Targets->count(Key)) {
      ASSERT_EQ(ExpectedRulePairs.erase(Key), 1u)
          << "exception is not in the literal manifest: " << E.RuleSet << "#"
          << E.RuleIndex;
    } else {
      ASSERT_EQ(ExpectedEscapePairs.erase(Key), 1u)
          << "exception is neither literal nor escaped: " << E.RuleSet << "#"
          << E.RuleIndex;
      ASSERT_TRUE(ApprovedEscapePairs.insert(Key).second)
          << "duplicate exception for " << E.RuleSet << "#" << E.RuleIndex;
    }
  }
  ASSERT_EQ(ExpectedRulePairs.size(), 188u);
  ASSERT_EQ(ExpectedEscapePairs.size(), 92u);
  ASSERT_EQ(ApprovedEscapePairs.size(), 1u);

  struct Fixture {
    Sigma S;
    std::unique_ptr<TableLegalizerInfoForTest> Table;
    std::unique_ptr<X86LegalizerInfoReference> Oracle;
  };
  std::map<std::string, Fixture> Fixtures;
  for (const Config &C : FullConfigs) {
    Fixture F;
    F.S = makeSigma(C);
    ASSERT_TRUE(F.S.STI) << C.ID;
    F.Table = std::make_unique<TableLegalizerInfoForTest>(*F.S.STI);
    F.Oracle = std::make_unique<X86LegalizerInfoReference>(
        *F.S.STI, *static_cast<const X86TargetMachine *>(F.S.TM.get()));
    Fixtures.emplace(C.ID, std::move(F));
  }

  unsigned Atoms = 0, Diffs = 0, ExpectedDiffs = 0;
  unsigned Hits = 0, Residuals = 0, Escapes = 0;
  unsigned RuleAtoms = 0, BoundaryAtoms = 0, ASAtoms = 0, DescriptorAtoms = 0;
  std::set<std::string> Ids;
  std::set<RuleKey> RulePairs, DescriptorPairs;
  struct AtomKey {
    std::string Key;
    std::string Origin;
    bool Descriptor = false;
  };
  std::vector<AtomKey> Keys;

  for (const json::Value &AtomValue : *AtomArray) {
    const json::Object *O = AtomValue.getAsObject();
    ASSERT_NE(O, nullptr) << "atom is not an object";
    auto Id = O->getString("id");
    auto Origin = O->getString("origin");
    auto RuleSet = O->getString("rule_set");
    auto ConfigID = O->getString("config");
    ASSERT_TRUE(Id && Origin && RuleSet && ConfigID);
    ASSERT_TRUE(Ids.insert(Id->str()).second) << "duplicate atom id " << *Id;

    auto TableIt = TableForSet.find(RuleSet->str());
    ASSERT_NE(TableIt, TableForSet.end()) << *RuleSet;
    const unsigned Table = TableIt->second;
    const X86TestTable &T = X86TestTables[Table];
    const std::optional<unsigned> Opcode = opcodeForName(T.Opcode);
    ASSERT_TRUE(Opcode.has_value()) << T.Opcode;

    int64_t RuleIndex = -1;
    if (auto R = O->getInteger("rule_index"))
      RuleIndex = *R;
    else
      ASSERT_TRUE(O->get("rule_index")) << *Id << ": missing rule_index";

    const json::Object *Query = O->getObject("query");
    ASSERT_NE(Query, nullptr) << *Id;
    const json::Array *TypeArray = Query->getArray("types");
    ASSERT_NE(TypeArray, nullptr) << *Id;
    SmallVector<LLT, 3> Types;
    for (const json::Value &TV : *TypeArray) {
      auto Name = TV.getAsString();
      ASSERT_TRUE(Name.has_value()) << *Id;
      auto Ty = parseWitnessLLT(*Name);
      ASSERT_TRUE(Ty.has_value()) << *Id << ": bad type " << *Name;
      Types.push_back(*Ty);
    }
    SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
    if (const json::Object *Mem = Query->getObject("mem")) {
      auto MemName = Mem->getString("type");
      auto Align = Mem->getInteger("align");
      ASSERT_TRUE(MemName && Align) << *Id;
      auto MemTy = parseWitnessLLT(*MemName);
      ASSERT_TRUE(MemTy.has_value()) << *Id << ": bad mem type " << *MemName;
      MemDescs.emplace_back(*MemTy, static_cast<uint64_t>(*Align),
                            AtomicOrdering::NotAtomic,
                            AtomicOrdering::NotAtomic);
    }
    const LegalityQuery Q(*Opcode, Types, MemDescs);

    const json::Object *ExpectedObj = O->getObject("expected");
    ASSERT_NE(ExpectedObj, nullptr) << *Id;
    auto ExpectedAction = ExpectedObj->getString("action");
    ASSERT_TRUE(ExpectedAction.has_value()) << *Id;
    auto Action = parseLegalizeAction(*ExpectedAction);
    ASSERT_TRUE(Action.has_value())
        << *Id << ": unknown expected action " << *ExpectedAction;
    LLT NewType = LLT{};
    if (auto To = ExpectedObj->getString("to")) {
      auto Parsed = parseWitnessLLT(*To);
      ASSERT_TRUE(Parsed.has_value()) << *Id << ": bad expected type " << *To;
      NewType = *Parsed;
    }
    const LegalizeActionStep Expected(
        *Action,
        static_cast<unsigned>(ExpectedObj->getInteger("idx").value_or(0)),
        NewType);

    auto FixtureIt = Fixtures.find(ConfigID->str());
    ASSERT_NE(FixtureIt, Fixtures.end()) << *Id << ": unknown config";
    Fixture &F = FixtureIt->second;
    const X86LegalizerTable::Result R = F.Table->table().query(Q);
    const LegalizeActionStep Got(R.Action, R.TypeIdx, R.NewType);
    const LegalizeActionStep Want = F.Oracle->getAction(Q);

    StringRef Basis;
    switch (R.Kind) {
    case X86LegalizerTable::Answer::Hit:
      Basis = "hit";
      ++Hits;
      break;
    case X86LegalizerTable::Answer::Residual:
      Basis = "residual";
      ++Residuals;
      break;
    default:
      Basis = "escape";
      ++Escapes;
      break;
    }

    ++Atoms;
    const bool IsDescriptor = StringRef(Origin->str()).starts_with("escape:");
    if (Origin->str() == "rule") {
      ++RuleAtoms;
      ASSERT_GE(RuleIndex, 0) << *Id;
      RulePairs.insert({Table, static_cast<unsigned>(RuleIndex)});
    } else if (Origin->str() == "boundary") {
      ++BoundaryAtoms;
    } else if (Origin->str() == "as") {
      ++ASAtoms;
      EXPECT_EQ(R.Kind, X86LegalizerTable::Answer::Residual)
          << "T1 rule-set=" << *RuleSet << " rule=#"
          << (RuleIndex >= 0 ? std::to_string(RuleIndex) : "-")
          << " atom=" << *Id
          << ": a non-zero address space must reach the router";
    } else if (IsDescriptor) {
      ++DescriptorAtoms;
      ASSERT_GE(RuleIndex, 0) << *Id;
      auto BasisText = O->getString("basis");
      ASSERT_TRUE(BasisText.has_value()) << *Id << ": descriptor has no basis";
      EXPECT_EQ(BasisText->str(), "escape") << *Id;
      EXPECT_TRUE(X86EvalRuleGuard(Table, static_cast<unsigned>(RuleIndex),
                                   *F.S.STI, Q))
          << *Id << ": descriptor query does not fire the rule guard";
      DescriptorPairs.insert({Table, static_cast<unsigned>(RuleIndex)});
    } else {
      ADD_FAILURE() << *Id << ": unknown origin " << *Origin;
    }

    Keys.push_back(
        {witnessQueryKey(*ConfigID, T.Opcode, Types, !MemDescs.empty(),
                         MemDescs.empty() ? LLT{} : MemDescs[0].MemoryTy,
                         MemDescs.empty() ? 0 : MemDescs[0].AlignInBits),
         Origin->str(), IsDescriptor});

    if (!stepsAgree(Expected, Want)) {
      ++ExpectedDiffs;
      ADD_FAILURE() << "T1 corpus atom " << *Id
                    << ": committed expected disagrees with the reference"
                    << "\n  committed: " << describe(Expected)
                    << "\n  oracle:    " << describe(Want);
    }

    if (!stepsAgree(Got, Want)) {
      ++Diffs;
      std::string QueryText;
      raw_string_ostream QOS(QueryText);
      QOS << T.Opcode << " (";
      for (unsigned I = 0; I != Types.size(); ++I)
        QOS << (I ? ", " : "") << witnessLLTName(Types[I]);
      QOS << ")";
      if (!MemDescs.empty())
        QOS << " mem(" << witnessLLTName(MemDescs[0].MemoryTy) << ", align "
            << MemDescs[0].AlignInBits << ")";
      ADD_FAILURE() << "T1 rule-set=" << *RuleSet << " rule=#"
                    << (RuleIndex >= 0 ? std::to_string(RuleIndex) : "-")
                    << " basis=" << Basis << " origin=" << *Origin
                    << " config=" << *ConfigID << "\n  query:  " << QueryText
                    << "\n  table:  " << describe(Got)
                    << "\n  oracle: " << describe(Want);
    }
  }

  // A descriptor atom must not reuse a query that already has a per-rule,
  // boundary or address-space origin: the corpus would then offer the same
  // shape as evidence for two different claims.
  std::map<std::string, std::string> NonDescriptorKeys;
  for (const AtomKey &K : Keys)
    if (!K.Descriptor)
      NonDescriptorKeys.emplace(K.Key, K.Origin);
  unsigned Collisions = 0;
  for (const AtomKey &K : Keys)
    if (K.Descriptor) {
      auto It = NonDescriptorKeys.find(K.Key);
      if (It == NonDescriptorKeys.end())
        continue;
      ++Collisions;
      ADD_FAILURE() << "descriptor atom " << K.Origin << " duplicates "
                    << It->second;
    }

  EXPECT_EQ(Atoms, 1256u);
  EXPECT_EQ(RuleAtoms, 188u);
  EXPECT_EQ(BoundaryAtoms, 756u);
  EXPECT_EQ(ASAtoms, 220u);
  EXPECT_EQ(DescriptorAtoms, 92u);
  EXPECT_EQ(RulePairs.size(), 188u);
  EXPECT_EQ(RulePairs, ExpectedRulePairs)
      << "per-rule atoms must name exactly the manifest's 188 literal rules";
  EXPECT_EQ(DescriptorPairs.size(), 92u);
  EXPECT_EQ(DescriptorPairs, ExpectedEscapePairs)
      << "descriptor atoms must name every witnessable escaped rule; only "
         "approved unreachable exceptions may be absent";
  for (const LegalizerException &E : *Exceptions) {
    auto TableIt = TableForSet.find(E.RuleSet);
    ASSERT_NE(TableIt, TableForSet.end()) << E.RuleSet;
    EXPECT_EQ(DescriptorPairs.count({TableIt->second, E.RuleIndex}), 0u)
        << "approved exception " << E.RuleSet << "#" << E.RuleIndex
        << " unexpectedly has a descriptor atom";
  }
  EXPECT_EQ(DescriptorPairs.size() + ApprovedEscapePairs.size(), 93u)
      << "covered escapes plus approved unreachable exceptions must equal the "
         "escaped denominator";
  EXPECT_EQ(Collisions, 0u);
  EXPECT_EQ(ExpectedDiffs, 0u);
  EXPECT_EQ(Diffs, 0u);
  llvm::outs() << "witness T1: atoms=" << Atoms << " diffs=" << Diffs
               << " hit=" << Hits << " residual=" << Residuals
               << " escape=" << Escapes << " descriptor=" << DescriptorAtoms
               << " descriptor_exceptions=" << ApprovedEscapePairs.size()
               << " collisions=" << Collisions
               << " expected_diffs=" << ExpectedDiffs << "\n";
}
// F1: the table's key has a single pointer class.  Every pointer collapses to
// p0, and G_STORE's first rule is exactly `Types[0] != MemoryTy`: folding the
// address space erases the comparison and answers (p1, p0) as if it were
// (p1, p1).  A pointer whose address space is not zero must therefore leave
// the table and be answered by the evaluator, which sees the concrete LLTs.
TEST_F(X86LegalizerDifferentialTest, AddressSpaceGoesResidual) {
  const Config &C = Configs[0];
  Sigma S = makeSigma(C);
  ASSERT_TRUE(S.STI) << C.ID;
  X86LegalizerInfoReference Oracle(
      *S.STI, *static_cast<const X86TargetMachine *>(S.TM.get()));
  TableLegalizerInfoForTest Table(*S.STI);

  const LLT P0 = LLT::pointer(0, 64), P1 = LLT::pointer(1, 64);
  const LLT V2P1 = LLT::fixed_vector(2, P1);

  auto Ask = [&](LLT ValueTy, LLT PtrTy, LLT MemTy, const char *Name) {
    const StoreProbe Probe =
        probeStore(Table.table(), Oracle, ValueTy, PtrTy, MemTy);
    EXPECT_EQ(Probe.TableResult.Kind, X86LegalizerTable::Answer::Residual)
        << Name << ": a non-zero address space must not use a table cell";

    LegalizeActionStep Got(Probe.TableResult.Action, Probe.TableResult.TypeIdx,
                           Probe.TableResult.NewType);
    EXPECT_TRUE(stepsAgree(Got, Probe.OracleStep))
        << Name << ": table " << describe(Got) << ", oracle "
        << describe(Probe.OracleStep);
    return Probe.OracleStep;
  };

  // Value type and memory type agree.  The oracle's Type != MemoryTy guard
  // does not fire, so this is not the Custom answer below.
  const LegalizeActionStep SameAS = Ask(P1, P1, P1, "G_STORE (p1, p1) m(p1)");

  // Value type and memory type disagree only in address space.  With one
  // pointer class the table cannot see that: this is the F1 discriminator.
  const LegalizeActionStep DifferentAS =
      Ask(P1, P0, P0, "G_STORE (p1, p0) m(p0)");
  EXPECT_FALSE(stepsAgree(SameAS, DifferentAS))
      << "the oracle must distinguish (p1, p1) from (p1, p0): "
      << describe(SameAS) << " vs " << describe(DifferentAS);
  EXPECT_EQ(DifferentAS.Action, Custom)
      << "G_STORE's type != memory-type guard must fire";

  // The address space can also be only in the memory descriptor.
  Ask(P0, P0, P1, "G_STORE (p0, p0) m(p1)");

  // Pointer vectors carry their address space in the element type.
  Ask(V2P1, P0, V2P1, "G_STORE (v2p1, p0) m(v2p1)");
}

// F1's discriminator is exactly the pair the table's folded pointer class
// cannot see: (p1, p1) m(p1) and (p1, p0) m(p0) both go residual, but the
// oracle's Type != MemoryTy guard makes the second one Custom.
TEST_F(X86LegalizerDifferentialTest, F1Repro) {
  const Config &C = Configs[0];
  Sigma S = makeSigma(C);
  ASSERT_TRUE(S.STI) << C.ID;
  X86LegalizerInfoReference Oracle(
      *S.STI, *static_cast<const X86TargetMachine *>(S.TM.get()));
  TableLegalizerInfoForTest Table(*S.STI);

  const LLT P0 = LLT::pointer(0, 64), P1 = LLT::pointer(1, 64);

  const StoreProbe SameAS = probeStore(Table.table(), Oracle, P1, P1, P1);
  const StoreProbe DifferentAS = probeStore(Table.table(), Oracle, P1, P0, P0);

  EXPECT_EQ(SameAS.TableResult.Kind, X86LegalizerTable::Answer::Residual)
      << "G_STORE (p1, p1) m(p1)";
  EXPECT_EQ(DifferentAS.TableResult.Kind, X86LegalizerTable::Answer::Residual)
      << "G_STORE (p1, p0) m(p0)";

  EXPECT_FALSE(stepsAgree(SameAS.OracleStep, DifferentAS.OracleStep))
      << "the oracle must distinguish (p1, p1) from (p1, p0): "
      << describe(SameAS.OracleStep) << " vs "
      << describe(DifferentAS.OracleStep);
  EXPECT_EQ(DifferentAS.OracleStep.Action, Custom)
      << "G_STORE's type != memory-type guard must fire";
}

// TC-27: the report is a pure function of the corpus and the tables, so two
// independent constructions must produce the same bytes.  This is the cheap
// half of the determinism evidence; TC-26 (two CLI processes) needs the CLI.
TEST_F(X86LegalizerDifferentialTest, ReportIsDeterministic) {
  Report First = renderReport();
  Report Second = renderReport();
  EXPECT_EQ(First.Atoms, Second.Atoms);
  EXPECT_EQ(First.Text.size(), Second.Text.size());
  EXPECT_EQ(First.Text, Second.Text) << "two runs in one process disagree";
}

// Not a test.  Run it to regenerate x86-ctpop-report.json after a change to
// the table that is meant to be visible in the review:
//
//   X86Tests --gtest_also_run_disabled_tests --gtest_filter=*DumpReport \
//     | grep '^{' > llvm/unittests/Target/X86/x86-ctpop-report.json
TEST_F(X86LegalizerDifferentialTest, DISABLED_DumpReport) {
  std::fputs(renderReport().Text.c_str(), stdout);
}

// The committed table is the review artifact: 126 lines, one per atom, with
// the oracle's answer next to the generated table's.  A reviewer can read the
// whole quotient instead of trusting a count, and a later change that moves an
// atom has to show up as a diff in this file.  It also covers TC-26: the table
// must be reproduced byte for byte across processes and machines, which a
// checked-in golden proves more cheaply than a second binary would.
TEST_F(X86LegalizerDifferentialTest, ReportMatchesCommittedGolden) {
  std::string Got = renderReport().Text;
  std::string Want = X86CtpopGoldenReport;
  if (Got == Want) {
    EXPECT_EQ(Got.size(), Want.size());
    return;
  }
  // Report the first line that moved rather than two 20 KB strings.
  size_t Line = 0, Off = 0;
  while (Off < Got.size() && Off < Want.size() && Got[Off] == Want[Off]) {
    Line += Got[Off] == '\n';
    ++Off;
  }
  size_t GotEnd = Got.find('\n', Off), WantEnd = Want.find('\n', Off);
  EXPECT_EQ(Want.substr(Off, WantEnd - Off), Got.substr(Off, GotEnd - Off))
      << "first difference at line " << Line + 1
      << " (regenerate with the DISABLED_DumpReport command above)";
}

//===----------------------------------------------------------------------===//
// Pinned tuple (TC-29) and escape accounting (TC-31)
//===----------------------------------------------------------------------===//
//
// The generated header carries the tuple recorded in x86-ctpop-atoms.json next
// to the values observed in this build.  Comparing them here, rather than
// hashing a whole report, means a drift names the field that drifted.
//
// Only behaviour is pinned.  The emitter and the .td are covered through the
// .inc they generate; pinning their source text would turn a formatting change
// into a repin without saying anything about what the table answers.

TEST_F(X86LegalizerDifferentialTest, PinnedTuple) {
  // The oracle is a verbatim copy of the hand-written rules.  If it is edited,
  // the 0-diff result stops meaning "the generated table reproduces the code
  // that was deleted", so its blob is pinned too.
  EXPECT_STREQ(X86_PIN_ORACLE_RECORDED, X86_PIN_ORACLE_ACTUAL)
      << "X86LegalizerInfoReference.cpp (frozen oracle)";
  EXPECT_STREQ(X86_PIN_INC_RECORDED, X86_PIN_INC_ACTUAL)
      << "generated X86LegalizerInfo.inc";
  EXPECT_STREQ(X86_PIN_CORE_RECORDED, X86_PIN_CORE_ACTUAL)
      << "X86LegalizerInfo.core.json (checkout root folded)";
}

// TC-31: the escapes are the part of the table that is C++ in disguise.  Pin
// the count so that turning a rule into generated structure, or losing an
// escape to a parse error, is visible; and pin the one entry this feature
// depends on -- X86_CTPOP's ClampScalar takes X86_sMaxScalar, a derived type,
// so that rule is copied into the .td verbatim rather than generated.
TEST_F(X86LegalizerDifferentialTest, EscapeAccounting) {
  EXPECT_EQ(X86_PIN_ESCAPES_ACTUAL, X86_PIN_ESCAPES_RECORDED)
      << "escape count changed";
  EXPECT_GT(X86_PIN_ESCAPES_ACTUAL, 0) << "escapes[] is empty";
  EXPECT_EQ(X86_PIN_CTPOP_ESCAPES_ACTUAL, X86_PIN_CTPOP_ESCAPES_RECORDED)
      << "X86_CTPOP no longer has exactly one nonliteral_type escape whose "
         "reason names clampScalar";
  EXPECT_EQ(X86_PIN_CTPOP_ESCAPES_ACTUAL, 1);
}

//===----------------------------------------------------------------------===//
// Corpus health (TC-05, TC-07)
//===----------------------------------------------------------------------===//
//
// These do not check that the two tables agree; they check that the quotient
// can see anything at all.  A corpus that answered the same way everywhere
// would make the differential pass no matter what the table said.

// The oracle's answer for every atom, indexed [config][type].
static std::vector<std::vector<LegalizeActionStep>> oracleSteps() {
  std::vector<std::vector<LegalizeActionStep>> Out;
  for (const Config &C : Configs) {
    Sigma S = makeSigma(C);
    EXPECT_TRUE(S.STI) << "no subtarget for " << C.ID;
    Out.emplace_back();
    if (!S.STI)
      continue;
    X86LegalizerInfoReference Oracle(
        *S.STI, *static_cast<const X86TargetMachine *>(S.TM.get()));
    for (const TypeClass &T : typeClasses()) {
      LLT Types[] = {T.Dst, T.Src};
      Out.back().push_back(Oracle.getAction(LegalityQuery(G_CTPOP, Types)));
    }
  }
  return Out;
}

static unsigned countStepDiffs(const std::vector<LegalizeActionStep> &A,
                               const std::vector<LegalizeActionStep> &B) {
  if (A.size() != B.size())
    return 0;
  unsigned N = 0;
  for (size_t I = 0; I < A.size(); ++I)
    N += !stepsAgree(A[I], B[I]);
  return N;
}

// TC-05: every query is a two-slot, immediate-free, memory-free G_CTPOP.  The
// builder is three lines long, but the corpus's FNV anchor assumes exactly
// this shape ("...|none|unused"), so the shape is asserted rather than assumed.
TEST_F(X86LegalizerDifferentialTest, QueryShape) {
  unsigned NonDiagonal = 0;
  for (const Config &C : Configs) {
    Sigma S = makeSigma(C);
    ASSERT_TRUE(S.STI) << C.ID;
    X86LegalizerInfoReference Oracle(
        *S.STI, *static_cast<const X86TargetMachine *>(S.TM.get()));
    for (const TypeClass &T : typeClasses()) {
      LLT Types[] = {T.Dst, T.Src};
      LegalityQuery Q(G_CTPOP, Types);
      EXPECT_EQ(Q.Opcode, (unsigned)G_CTPOP) << C.ID << "/" << T.ID;
      ASSERT_EQ(Q.Types.size(), 2u) << C.ID << "/" << T.ID;
      EXPECT_EQ(Q.Types[0], T.Dst) << C.ID << "/" << T.ID;
      EXPECT_EQ(Q.Types[1], T.Src) << C.ID << "/" << T.ID;
      EXPECT_TRUE(Q.MMODescrs.empty()) << C.ID << "/" << T.ID;
      EXPECT_TRUE(Q.Immediates.empty()) << C.ID << "/" << T.ID;

      // Slot order is observable, otherwise the non-diagonal entries (N0, N1)
      // would be the same test twice.
      if (T.Dst == T.Src)
        continue;
      LLT Reversed[] = {T.Src, T.Dst};
      if (!stepsAgree(Oracle.getAction(Q),
                      Oracle.getAction(LegalityQuery(G_CTPOP, Reversed))))
        ++NonDiagonal;
    }
  }
  EXPECT_GT(NonDiagonal, 0u) << "no atom is sensitive to type slot order";
}

// TC-07: the corpus must discriminate.  The four claims below are counts over
// the whole quotient; none of them names an atom, so relaxing the corpus shows
// up as a failed count rather than as a silently smaller test.
TEST_F(X86LegalizerDifferentialTest, CorpusIsDiscriminating) {
  auto Steps = oracleSteps();
  ASSERT_EQ(Steps.size(), std::size(Configs));
  for (const auto &S : Steps)
    ASSERT_EQ(S.size(), typeClasses().size());

  // (a) C1 and C2 differ only by VLX, and the corpus must see that.
  EXPECT_GT(countStepDiffs(Steps[1], Steps[2]), 0u) << "C1 vs C2";
  // (b) C1 and C5 differ only by VPOPCNTDQ; the 512-bit atoms must see that.
  EXPECT_GT(countStepDiffs(Steps[1], Steps[5]), 0u) << "C1 vs C5";
  // (c) C2 and C6 differ only by VPOPCNTDQ; the 128/256-bit atoms must see it.
  EXPECT_GT(countStepDiffs(Steps[2], Steps[6]), 0u) << "C2 vs C6";
  // (d) C5 and C6 agree on every legal answer, because the two guards only
  // differ when VPOPCNTDQ is present and C5/C6 both lack it.  Asserting this
  // is the reason the mutant tables exist: within the quotient no *legitimate*
  // configuration can tell HasVLX from VPOPCNTAndVLX, so only a table whose
  // guard was corrupted can.
  EXPECT_EQ(countStepDiffs(Steps[5], Steps[6]), 0u)
      << "C5 and C6 became distinguishable; the mutant tests' premise is stale";
}

//===----------------------------------------------------------------------===//
// Report round trip (TC-06)
//===----------------------------------------------------------------------===//
//
// The report is written by hand, so "it is valid JSON" is a claim, not a
// definition.  Parse every line, re-serialise it, parse that again, and require
// the two values to be equal: the text is a fixed point of a real parser, and
// the schema is what the test says it is.  Byte equality after re-serialisation
// is deliberately not asserted: llvm::json::Object is DenseMap-backed, so the
// order a re-serialiser picks is an implementation detail.  The report's own
// byte stability is covered by ReportIsDeterministic (TC-27) and by the
// committed golden table.

TEST_F(X86LegalizerDifferentialTest, ReportRoundTripsThroughJson) {
  std::string Text = renderReport().Text;
  SmallVector<StringRef, 128> Lines;
  StringRef(Text).split(Lines, '\n', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  ASSERT_EQ(Lines.size(), (size_t)X86_CTPOP_ATOM_COUNT + 1);

  for (StringRef Line : Lines) {
    Expected<json::Value> Parsed = json::parse(Line);
    if (!Parsed) {
      ADD_FAILURE() << Line << "\n" << toString(Parsed.takeError());
      continue;
    }
    const json::Object *Obj = Parsed->getAsObject();
    ASSERT_NE(Obj, nullptr) << Line;

    if (Obj->get("atom")) {
      EXPECT_EQ(Obj->size(), 8u) << "atom record schema changed: " << Line;
      EXPECT_EQ(Obj->getString("opcode").value_or(""), "G_CTPOP");
      std::optional<StringRef> Status = Obj->getString("status");
      ASSERT_TRUE(Status) << Line;
      EXPECT_TRUE(*Status == "ok" || *Status == "diff") << Line;
      const json::Array *Types = Obj->getArray("types");
      ASSERT_NE(Types, nullptr) << Line;
      EXPECT_EQ(Types->size(), 2u) << Line;
      EXPECT_TRUE(Obj->getBoolean("custom")) << "custom must be a bool";
    } else {
      EXPECT_EQ(Obj->size(), 5u) << "summary schema changed: " << Line;
    }

    std::string ReSerialized;
    raw_string_ostream OS(ReSerialized);
    OS << *Parsed;
    Expected<json::Value> Reparsed = json::parse(ReSerialized);
    if (!Reparsed) {
      ADD_FAILURE() << ReSerialized << "\n" << toString(Reparsed.takeError());
      continue;
    }
    EXPECT_TRUE(*Parsed == *Reparsed) << "round trip changed the report";
  }
}

//===----------------------------------------------------------------------===//
// Mutation tests
//===----------------------------------------------------------------------===//
//
// A differential harness that never fails is indistinguishable from one whose
// two sides happen to be the same table.  These tests corrupt one rule of the
// .td at configure time and require the harness to report exactly the atoms the
// corruption can reach: no atom more (the comparator is not trigger-happy) and
// no atom less (the vocabulary is wide enough to see the defect at all).
//
// The expectation is written as a predicate over (config, type), derived from
// the rule that was corrupted, not as a list copied out of a run.

using AtomID = std::string;

struct DiffResult {
  std::set<AtomID> Atoms;
  std::string Report;
};

// Every atom where `Mutant` and the frozen oracle disagree on the first step.
template <typename Mutant> static DiffResult diffAgainstOracle() {
  DiffResult R;
  for (const Config &C : Configs) {
    Sigma S = makeSigma(C);
    EXPECT_TRUE(S.STI) << "no subtarget for " << C.ID;
    if (!S.STI)
      continue;
    X86LegalizerInfoReference Oracle(
        *S.STI, *static_cast<const X86TargetMachine *>(S.TM.get()));
    Mutant M(*S.STI);
    for (const TypeClass &T : typeClasses()) {
      LLT Types[] = {T.Dst, T.Src};
      LegalityQuery Q(G_CTPOP, Types);
      LegalizeActionStep A = Oracle.getAction(Q);
      LegalizeActionStep B = M.getAction(Q);
      if (stepsAgree(A, B))
        continue;
      R.Atoms.insert(std::string(C.ID) + "/" + T.ID);
      raw_string_ostream OS(R.Report);
      OS << "  " << C.ID << "/" << T.ID << " expected " << describe(A)
         << " got " << describe(B) << "\n";
    }
  }
  return R;
}

static std::set<AtomID> selectAtoms(bool (*Pred)(const Config &,
                                                 const TypeClass &)) {
  std::set<AtomID> S;
  for (const Config &C : Configs)
    for (const TypeClass &T : typeClasses())
      if (Pred(C, T))
        S.insert(std::string(C.ID) + "/" + T.ID);
  return S;
}

static std::string renderSetDifference(const std::set<AtomID> &Got,
                                       const std::set<AtomID> &Want) {
  std::string Out;
  raw_string_ostream OS(Out);
  for (const AtomID &A : Got)
    if (!Want.count(A))
      OS << "  unexpected diff: " << A << "\n";
  for (const AtomID &A : Want)
    if (!Got.count(A))
      OS << "  missed diff:     " << A << "\n";
  return OS.str();
}

static void expectDiffIs(const DiffResult &R, const std::set<AtomID> &Want,
                         const char *What) {
  EXPECT_EQ(R.Atoms.size(), Want.size()) << What;
  EXPECT_TRUE(R.Atoms == Want)
      << What << ": detected set != predicted set\n"
      << renderSetDifference(R.Atoms, Want) << "actual steps:\n"
      << R.Report;
}

// TC-23: the s32 pair is dropped from the HasPOPCNT rule.  s32/s32 (T2) is the
// only type whose sole legal path was that pair, so exactly the POPCNT targets
// move, and they move on that type alone.  C3 has no POPCNT and must not move.
TEST_F(X86LegalizerDifferentialTest, MutantDroppedS32PopcntPair) {
  std::set<AtomID> Want = selectAtoms([](const Config &C, const TypeClass &T) {
    return C.POPCNT && StringRef(T.ID) == "T2";
  });
  EXPECT_EQ(Want.size(), 6u) << "predicate no longer describes the corpus";
  expectDiffIs(diffAgainstOracle<mutant1::Mutant>(), Want,
               "dropped s32 popcnt pair");
}

// TC-24a: HasVPOPCNTDQ was mistyped as HasAVX512 on the 512-bit rule.  Only a
// target with AVX512 but without VPOPCNTDQ can tell the two apart, and only on
// the two types that rule legalizes: C5/C6 x {T9,T10}.
TEST_F(X86LegalizerDifferentialTest, MutantVpopcntdqWrittenAsAvx512) {
  std::set<AtomID> Want = selectAtoms([](const Config &C, const TypeClass &T) {
    StringRef Ty(T.ID);
    return C.AVX512 && !C.VPOPCNTDQ && (Ty == "T9" || Ty == "T10");
  });
  EXPECT_EQ(Want.size(), 4u) << "predicate no longer describes the corpus";
  expectDiffIs(diffAgainstOracle<mutant2::Mutant>(), Want,
               "vpopcntdq written as avx512");
}

// TC-24b: VPOPCNTAndVLX was mistyped as HasVLX on the 128/256-bit rule.  Only
// C6 (VLX without VPOPCNTDQ) can see it, on the four types that rule legalizes.
TEST_F(X86LegalizerDifferentialTest, MutantVlxGuardWrittenAsVlxOnly) {
  std::set<AtomID> Want = selectAtoms([](const Config &C, const TypeClass &T) {
    StringRef Ty(T.ID);
    return C.VLX && !C.VPOPCNTDQ &&
           (Ty == "T5" || Ty == "T6" || Ty == "T7" || Ty == "T8");
  });
  EXPECT_EQ(Want.size(), 4u) << "predicate no longer describes the corpus";
  expectDiffIs(diffAgainstOracle<mutant3::Mutant>(), Want,
               "vlx guard widened to vlx only");
}

} // namespace
