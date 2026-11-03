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
#include "X86LegalizerInfoReferenceMutants.h"
#include "X86Subtarget.h"
#include "X86TargetMachine.h"
#include "x86-ctpop-atoms.h"
#include "x86-ctpop-report.h"
#include "x86-legalizer-pins.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VCSRevision.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <array>
#include <chrono>
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

// A test-only decorator that records how often LegalizerInfo::getAction()
// consults the table and how often the table declines.  The reader's query()
// is not called through lookup(), so replaying a query directly does not move
// these counters.
class CountingLegalizerTable : public LegalizerInfo::LegalizeTable {
  std::unique_ptr<X86LegalizerTable> Inner;
  mutable uint64_t Lookups = 0;
  mutable uint64_t FallThroughs = 0;

public:
  explicit CountingLegalizerTable(std::unique_ptr<X86LegalizerTable> Table)
      : Inner(std::move(Table)) {}

  bool lookup(const LegalityQuery &Query,
              LegalizeActionStep &Step) const override {
    ++Lookups;
    if (Inner->lookup(Query, Step))
      return true;
    ++FallThroughs;
    return false;
  }

  const X86LegalizerTable &inner() const { return *Inner; }
  X86LegalizerTable &inner() { return *Inner; }
  uint64_t lookups() const { return Lookups; }
  uint64_t fallThroughs() const { return FallThroughs; }
};

// The table reader installed on the same generated fragments.  Its answers are
// compared through query() directly: getAction() falls back to the chain when
// the table declines, which would make a decline look like agreement.
class TableLegalizerInfoForTest : public LegalizerInfo {
  const X86Subtarget &Subtarget;
  CountingLegalizerTable *TablePtr = nullptr;

public:
  explicit TableLegalizerInfoForTest(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfo.inc"
    auto T = std::make_unique<X86LegalizerTable>(
        *this, Subtarget, Subtarget.isTarget64BitLP64() ? 64 : 32);
    auto Counting = std::make_unique<CountingLegalizerTable>(std::move(T));
    TablePtr = Counting.get();
    setTable(std::move(Counting));
  }

  const X86LegalizerTable &table() const { return TablePtr->inner(); }
  X86LegalizerTable &mutableTable() { return TablePtr->inner(); }
  uint64_t lookups() const { return TablePtr->lookups(); }
  uint64_t fallThroughs() const { return TablePtr->fallThroughs(); }

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

namespace mutant4 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant4.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant4

namespace mutant5 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant5.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant5

namespace mutant6 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant6.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant6

namespace mutant7 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant7.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant7

namespace mutant8 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant8.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant8

namespace mutant9 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant9.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant9

namespace mutant10 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant10.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant10

namespace mutant11 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant11.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant11

namespace mutant12 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant12.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant12

namespace mutant13 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant13.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant13

namespace mutant14 {
class Mutant : public LegalizerInfo {
  const X86Subtarget &Subtarget;

public:
  explicit Mutant(const X86Subtarget &STI) : Subtarget(STI) {
#include "X86LegalizerInfoMutant14.inc"
  }
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};
} // namespace mutant14

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

// G005 feature matrix.  Six subtargets vary one feature at a time so a mutant
// that corrupts a feature guard has a configuration that separates it from the
// reference.  These are not witness configurations: MutationKillMatrix replays
// every committed witness atom under each of them.
static const Config MatrixConfigs[] = {
    {"M0", "x86_64", "", true, false, false, false, false},
    {"M1", "x86_64", "+popcnt", true, false, false, false, true},
    {"M2", "x86_64", "+lzcnt", true, false, false, false, false},
    {"M3", "x86_64", "+avx512f,+avx512vpopcntdq", true, true, true, false,
     false},
    {"M4", "x86_64", "+avx512f,+avx512vl", true, true, false, true, false},
    {"M5", "x86_64", "+avx512f", true, true, false, false, false},
};
static const char *const MatrixConfigNames[] = {
    "baseline",       "POPCNT-only", "LZCNT-only",
    "VPOPCNTDQ-only", "VLX-only",    "AVX512-without-VPOPCNTDQ",
};
static_assert(std::size(MatrixConfigs) == std::size(MatrixConfigNames),
              "feature matrix labels must match the configurations");

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

class X86LegalizerTableSignatureTest : public testing::Test {
protected:
  static void SetUpTestCase() {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
  }
};

// The signature is the environment the table was built for: one bit per
// LegalizeFeature record, emitted from the same .td records the evaluators
// read.  Two properties matter here: it separates configs that differ in a
// rule-visible feature, and configs whose signatures agree answer identically.
static std::optional<uint32_t> signatureFor(const Config &C) {
  Sigma S = makeSigma(C);
  if (!S.STI)
    return std::nullopt;
  LegalizerInfo LI;
  X86LegalizerTable T(LI, *S.STI, S.STI->isTarget64BitLP64() ? 64 : 32);
  return T.signature();
}

TEST_F(X86LegalizerTableSignatureTest, SeparatesRuleVisibleFeatures) {
  const Config *C0 = &Configs[0]; // x86_64 +popcnt
  const Config *C1 = &Configs[1]; // +avx512f +avx512vpopcntdq
  const Config *C2 = &Configs[2]; // +avx512f +avx512vl +avx512vpopcntdq
  const Config *C3 = &Configs[3]; // x86_64 baseline
  const Config *C4 = &Configs[4]; // i386 +popcnt (Is64Bit clear)
  const Config *C5 = &Configs[5]; // +avx512f (no vpopcntdq)

  auto S0 = signatureFor(*C0), S1 = signatureFor(*C1), S2 = signatureFor(*C2),
       S3 = signatureFor(*C3), S4 = signatureFor(*C4), S5 = signatureFor(*C5);
  ASSERT_TRUE(S0 && S1 && S2 && S3 && S4 && S5);

  EXPECT_NE(*S0, *S3) << "POPCNT is rule-visible";
  EXPECT_NE(*S0, *S5) << "HasAVX512 is rule-visible";
  EXPECT_NE(*S1, *S2) << "HasVLX is rule-visible";
  EXPECT_NE(*S1, *S5) << "HasVPOPCNTDQ is rule-visible";
  EXPECT_NE(*S2, *S5) << "AVX512-with-VLX differs from AVX512-without-VLX";
  EXPECT_NE(*S3, *S4) << "Is64Bit is rule-visible";
}

TEST_F(X86LegalizerTableSignatureTest, EqualSignatureImpliesEqualAnswers) {
  // +cx16 is not read by any rule, so both configs must land in the same
  // equivalence class of the environment quotient.
  const Config A = Configs[0];
  const Config B = {"C0b", "x86_64", "+popcnt,+cx16", true, false, false,
                    false, true};

  auto SA = signatureFor(A);
  auto SB = signatureFor(B);
  ASSERT_TRUE(SA && SB);
  ASSERT_EQ(*SA, *SB) << "an unrelated feature changed the signature";

  Sigma SigmaA = makeSigma(A);
  Sigma SigmaB = makeSigma(B);
  ASSERT_NE(SigmaA.STI, nullptr);
  ASSERT_NE(SigmaB.STI, nullptr);

  LegalizerInfo LIA;
  LegalizerInfo LIB;
  X86LegalizerTable TA(LIA, *SigmaA.STI,
                       SigmaA.STI->isTarget64BitLP64() ? 64 : 32);
  X86LegalizerTable TB(LIB, *SigmaB.STI,
                       SigmaB.STI->isTarget64BitLP64() ? 64 : 32);

  SmallVector<X86TablePoint, 0> Points;
  x86CollectTablePoints(Points, 64);
  ASSERT_FALSE(Points.empty());

  unsigned Compared = 0;
  for (const X86TablePoint &P : Points) {
    ArrayRef<LLT> Types(P.Types, P.NumTypes);
    SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
    if (P.HasMem)
      MemDescs.emplace_back(P.MemTy, P.Align, AtomicOrdering::NotAtomic,
                            AtomicOrdering::NotAtomic);
    const LegalityQuery Q(P.Opcode, Types, MemDescs);
    const X86LegalizerTable::Result RA = TA.query(Q);
    const X86LegalizerTable::Result RB = TB.query(Q);
    ASSERT_EQ(RA.Kind, RB.Kind) << P.Name;
    if (RA.Kind == X86LegalizerTable::Answer::Hit ||
        RA.Kind == X86LegalizerTable::Answer::Residual) {
      ASSERT_EQ(RA.Action, RB.Action) << P.Name;
      ASSERT_EQ(RA.TypeIdx, RB.TypeIdx) << P.Name;
      if (actionCarriesType(RA.Action))
        ASSERT_EQ(RA.NewType, RB.NewType) << P.Name;
    }
    ++Compared;
  }
  EXPECT_EQ(Compared, Points.size());
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
static void enumerateBoundaryPanelPoints(const X86TestTable &T,
                                         const TableDomainAxis &A,
                                         ArrayRef<LLT> Boundary, Sink &&Emit) {
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

// The 12-atom witness/full path is unchanged; the deep panel reuses the same
// shape with a larger, reviewed panel.
template <typename Sink>
static void enumerateBoundaryPoints(const X86TestTable &T,
                                    const TableDomainAxis &A, Sink &&Emit) {
  enumerateBoundaryPanelPoints(T, A, boundaryPanel(), std::forward<Sink>(Emit));
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
  std::set<std::pair<std::string, int64_t>> MutationTargets;
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
      if (Rule->getObject("mutation"))
        MutationTargets.emplace(Def->str(), *RuleIndex);
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
  const int64_t MutationTargetCount =
      static_cast<int64_t>(MutationTargets.size());

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
  ExpectField("mutation_target_count",
              M->getInteger("mutation_target_count").value_or(-1),
              MutationTargetCount);

  EXPECT_EQ(TDRuleCount, LiteralRuleCount + EscapedRuleCount);
  EXPECT_EQ(EscapedRuleCount, EscapedInLiteralSets + EscapedInAllEscapedSets);
  EXPECT_EQ(TableCount, LiteralSetCount + OnlyEscapedSetCount);

  // The rule sets that only occur in the escape list, sorted.  Every rule set
  // now has at least one literal rule, so there are none.
  const std::vector<std::string> ExpectedOnlyEscaped = {};
  std::vector<std::string> ActualOnlyEscaped(OnlyEscapedSetNames.begin(),
                                             OnlyEscapedSetNames.end());
  ASSERT_EQ(ActualOnlyEscaped.size(), ExpectedOnlyEscaped.size());
  for (size_t I = 0; I < ExpectedOnlyEscaped.size(); ++I)
    EXPECT_EQ(ActualOnlyEscaped[I], ExpectedOnlyEscaped[I]) << "index " << I;
}

// A select type or lane count must stay a conditional node in the generated
// file.  Folding it would bake in one subtarget's answer and silently lose the
// others: the reader cannot tell a folded constant from a literal rule, so a
// regression here is invisible to the differential tests over one config.
static const json::Object *coreRulesetByName(const json::Object &Core,
                                             StringRef Def) {
  const json::Array *Rulesets = Core.getArray("rulesets");
  if (!Rulesets)
    return nullptr;
  for (const json::Value &V : *Rulesets) {
    const json::Object *O = V.getAsObject();
    if (!O)
      continue;
    std::optional<StringRef> D = O->getString("def");
    if (D && *D == Def)
      return O;
  }
  return nullptr;
}

static const json::Object *findCoreRule(const json::Object &Ruleset,
                                        int64_t Index, StringRef Action) {
  const json::Array *Rules = Ruleset.getArray("rules");
  if (!Rules)
    return nullptr;
  for (const json::Value &V : *Rules) {
    const json::Object *R = V.getAsObject();
    if (!R)
      continue;
    std::optional<int64_t> I = R->getInteger("rule_index");
    std::optional<StringRef> A = R->getString("action");
    if (I && *I == Index && A && *A == Action)
      return R;
  }
  return nullptr;
}

TEST_F(X86LegalizerDifferentialTest, SelectGuardsAreNotFolded) {
  auto Buffer = MemoryBuffer::getFile(X86_LEGALIZER_CORE_JSON);
  ASSERT_TRUE(!!Buffer) << X86_LEGALIZER_CORE_JSON;
  auto Value = json::parse((*Buffer)->getBuffer());
  ASSERT_TRUE(!!Value) << toString(Value.takeError());
  const json::Object *Core = Value->getAsObject();
  ASSERT_NE(Core, nullptr);

  // A type position: X86_INTTOPTR legalizes (p0, sMaxScalar), so slot 1 is the
  // Is64Bit select rather than one of its branches.
  const json::Object *IntToPtr = coreRulesetByName(*Core, "X86_INTTOPTR");
  ASSERT_NE(IntToPtr, nullptr);
  const json::Object *Pair = findCoreRule(*IntToPtr, 0, "Legal");
  ASSERT_NE(Pair, nullptr);
  const json::Object *PairPred = Pair->getObject("predicate");
  ASSERT_NE(PairPred, nullptr);
  const json::Array *PairArgs = PairPred->getArray("args");
  ASSERT_NE(PairArgs, nullptr);
  ASSERT_EQ(PairArgs->size(), 2u);
  const json::Object *Slot1 = (*PairArgs)[1].getAsObject();
  ASSERT_NE(Slot1, nullptr);
  EXPECT_EQ(Slot1->getString("name").value_or(""), "typeIs");
  const json::Array *Slot1Args = Slot1->getArray("args");
  ASSERT_NE(Slot1Args, nullptr);
  ASSERT_EQ(Slot1Args->size(), 2u);
  const json::Object *Slot1Value = (*Slot1Args)[1].getAsObject();
  ASSERT_NE(Slot1Value, nullptr);
  const json::Object *TypeSelect = Slot1Value->getObject("select");
  ASSERT_NE(TypeSelect, nullptr) << "slot 1 must keep the select node";
  EXPECT_EQ(TypeSelect->getString("then").value_or(""), "s64");
  EXPECT_EQ(TypeSelect->getString("else").value_or(""), "s32");

  // A bit-count position: ClampScalar's upper bound is the same select, and
  // the scalarWiderThan leaf reads the bit count, not a chosen number.
  const json::Object *Constant = coreRulesetByName(*Core, "X86_CONSTANT");
  ASSERT_NE(Constant, nullptr);
  const json::Object *Narrow = findCoreRule(*Constant, 3, "NarrowScalar");
  ASSERT_NE(Narrow, nullptr);
  const json::Object *NarrowPred = Narrow->getObject("predicate");
  ASSERT_NE(NarrowPred, nullptr);
  const json::Array *NarrowArgs = NarrowPred->getArray("args");
  ASSERT_NE(NarrowArgs, nullptr);
  ASSERT_EQ(NarrowArgs->size(), 2u);
  const json::Object *BitsValue = (*NarrowArgs)[1].getAsObject();
  ASSERT_NE(BitsValue, nullptr);
  const json::Object *BitsSelect = BitsValue->getObject("select");
  ASSERT_NE(BitsSelect, nullptr) << "the clamp bound must keep the select node";
  EXPECT_EQ(BitsSelect->getString("then").value_or(""), "64");
  EXPECT_EQ(BitsSelect->getString("else").value_or(""), "32");

  // A lane-count position: the max-vector clamp is a priority list, so its
  // false branch is another select rather than the fallback count.
  const json::Object *Add = coreRulesetByName(*Core, "X86_ADD");
  ASSERT_NE(Add, nullptr);
  const json::Object *ClampMax = findCoreRule(*Add, 10, "FewerElements");
  ASSERT_NE(ClampMax, nullptr);
  const json::Object *ClampPred = ClampMax->getObject("predicate");
  ASSERT_NE(ClampPred, nullptr);
  const json::Array *ClampArgs = ClampPred->getArray("args");
  ASSERT_NE(ClampArgs, nullptr);
  ASSERT_EQ(ClampArgs->size(), 2u);
  const json::Object *LanesLeaf = (*ClampArgs)[1].getAsObject();
  ASSERT_NE(LanesLeaf, nullptr);
  EXPECT_EQ(LanesLeaf->getString("name").value_or(""), "moreElementsThan");
  const json::Array *LanesArgs = LanesLeaf->getArray("args");
  ASSERT_NE(LanesArgs, nullptr);
  ASSERT_EQ(LanesArgs->size(), 2u);
  const json::Object *LanesValue = (*LanesArgs)[1].getAsObject();
  ASSERT_NE(LanesValue, nullptr);
  const json::Object *LanesSelect = LanesValue->getObject("select");
  ASSERT_NE(LanesSelect, nullptr);
  EXPECT_EQ(LanesSelect->getInteger("then").value_or(-1), 64);
  const json::Object *ElseValue = LanesSelect->getObject("else");
  ASSERT_NE(ElseValue, nullptr);
  const json::Object *ElseSelect = ElseValue->getObject("select");
  ASSERT_NE(ElseSelect, nullptr) << "the fallback must stay a nested select";
  EXPECT_EQ(ElseSelect->getInteger("then").value_or(-1), 32);
  EXPECT_EQ(ElseSelect->getInteger("else").value_or(-1), 16);
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

// The ten kinds the ledger accepts.  Only `unreachable` means "this rule has
// no atom": the other kinds record coverage, suppress a MIR mapping, or defer
// an obligation, and each consumer accepts a declared subset.  An unknown kind
// is a hard parse failure rather than a silently ignored entry.
static constexpr StringLiteral LegalizerExceptionKinds[] = {
    "unreachable",          "residual-only",
    "mir-exclusion",        "fuzz-budget",
    "reference-rebaseline", "ci-budget-waiver",
    "nonliteral-shape",     "judgment",
    "prerequisite-pending", "reference-retirement"};

static bool isLegalizerExceptionKind(StringRef Kind) {
  for (StringLiteral Known : LegalizerExceptionKinds)
    if (Kind == Known)
      return true;
  return false;
}

// `judgment` may link to the descriptor ledger or directly to one witness
// atom; `mir-exclusion` may cite an atom in its proof.  Witness atom ids use
// the `x86/<origin>/<rule set>#<index>` spelling the corpus commits.
static bool namesWitnessAtom(StringRef Text) { return Text.contains("x86/"); }

// Parse the in-memory ledger.  Field validation is shared by the committed
// file and by ExceptionLedgerKindContract, so a malformed entry fails the same
// way in both paths.
static std::optional<std::vector<LegalizerException>>
parseLegalizerExceptions(StringRef Text, std::string &Error) {
  auto Value = json::parse(Text);
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
  if (!Version || *Version != 2) {
    Error = "version must be 2";
    return std::nullopt;
  }
  const json::Array *Entries = Root->getArray("exceptions");
  if (!Entries) {
    Error = "missing field 'exceptions'";
    return std::nullopt;
  }

  std::vector<LegalizerException> Out;
  for (const json::Value &V : *Entries) {
    const json::Object *O = V.getAsObject();
    if (!O) {
      Error = "exception is not an object";
      return std::nullopt;
    }

    // Read the rule identity first so a later problem can name the rule.
    auto RuleSet = O->getString("rule_set");
    auto RuleIndex = O->getInteger("rule_index");
    std::string Name = RuleSet ? RuleSet->str() : std::string("<unknown>");
    if (RuleIndex)
      Name += "#" + std::to_string(*RuleIndex);

    LegalizerException E;
    auto RequireNonEmpty = [&](StringRef Key, std::string &Field) -> bool {
      auto S = O->getString(Key);
      if (!S) {
        Error = Name + ": missing field '" + Key.str() + "'";
        return false;
      }
      if (S->empty()) {
        Error = Name + ": empty '" + Key.str() + "'";
        return false;
      }
      Field = S->str();
      return true;
    };
    if (!RequireNonEmpty("kind", E.Kind) ||
        !RequireNonEmpty("rule_set", E.RuleSet) ||
        !RequireNonEmpty("reason", E.Reason) ||
        !RequireNonEmpty("proof", E.Proof) ||
        !RequireNonEmpty("linked_ledger", E.LinkedLedger) ||
        !RequireNonEmpty("approver", E.Approver) ||
        !RequireNonEmpty("PR", E.PR))
      return std::nullopt;
    if (!RuleIndex || *RuleIndex < 0) {
      Error = Name + ": missing or negative field 'rule_index'";
      return std::nullopt;
    }
    E.RuleIndex = static_cast<unsigned>(*RuleIndex);
    Name = E.RuleSet + "#" + std::to_string(E.RuleIndex);

    if (!isLegalizerExceptionKind(E.Kind)) {
      Error = Name + ": unknown kind '" + E.Kind + "'";
      return std::nullopt;
    }
    if (E.Kind == "prerequisite-pending" && E.PR != "PR-F1") {
      Error = Name + ": prerequisite-pending requires PR 'PR-F1', got '" +
              E.PR + "'";
      return std::nullopt;
    }
    if (E.Kind == "judgment" && E.LinkedLedger != "WitnessDescriptorAtoms" &&
        !namesWitnessAtom(E.LinkedLedger)) {
      Error = Name +
              ": judgment must link WitnessDescriptorAtoms or a witness "
              "atom, got '" +
              E.LinkedLedger + "'";
      return std::nullopt;
    }
    if (E.Kind == "nonliteral-shape" &&
        E.LinkedLedger != "WitnessDescriptorAtoms") {
      Error = Name +
              ": nonliteral-shape must link WitnessDescriptorAtoms, "
              "got '" +
              E.LinkedLedger + "'";
      return std::nullopt;
    }
    if (E.Kind == "mir-exclusion" &&
        E.LinkedLedger != "WitnessDecisionDifferential" &&
        !namesWitnessAtom(E.Proof)) {
      Error = Name + ": mir-exclusion must link WitnessDecisionDifferential "
                     "or cite a witness atom in its proof";
      return std::nullopt;
    }
    Out.push_back(E);
  }
  return Out;
}

static std::optional<std::vector<LegalizerException>>
readLegalizerExceptions(std::string &Error) {
  std::optional<std::string> Text =
      readTestSourceFile("x86-legalizer-exceptions.json");
  if (!Text) {
    Error = "cannot read x86-legalizer-exceptions.json from " +
            std::string(X86_LEGALIZER_TEST_SRC_DIR);
    return std::nullopt;
  }
  return parseLegalizerExceptions(*Text, Error);
}

// A minimal in-memory ledger entry.  A nullopt drops a field so the contract
// test can distinguish "missing" from "present but empty".
struct LedgerEntryFields {
  std::optional<std::string> Kind = "unreachable";
  std::optional<std::string> RuleSet = "X86_ADD";
  std::optional<std::string> RuleIndex = "9";
  std::optional<std::string> Reason = "reason";
  std::optional<std::string> Proof = "proof";
  std::optional<std::string> LinkedLedger = "WitnessDescriptorAtoms";
  std::optional<std::string> Approver = "nodecoda";
  std::optional<std::string> PR = "TBD";
};

static std::string ledgerEntryJson(const LedgerEntryFields &F) {
  std::string Body = "{";
  auto AddString = [&](StringRef Key, const std::optional<std::string> &V) {
    if (!V)
      return;
    Body += "\"" + Key.str() + "\":\"" + *V + "\",";
  };
  auto AddRaw = [&](StringRef Key, const std::optional<std::string> &V) {
    if (!V)
      return;
    Body += "\"" + Key.str() + "\":" + *V + ",";
  };
  AddString("kind", F.Kind);
  AddString("rule_set", F.RuleSet);
  AddRaw("rule_index", F.RuleIndex);
  AddString("reason", F.Reason);
  AddString("proof", F.Proof);
  AddString("linked_ledger", F.LinkedLedger);
  AddString("approver", F.Approver);
  AddString("PR", F.PR);
  if (!Body.empty() && Body.back() == ',')
    Body.pop_back();
  Body += "}";
  return Body;
}

static std::string ledgerDocument(StringRef EntryJson) {
  return "{\"schema\":\"llvm.gisel.legalizer-exceptions\","
         "\"version\":2,\"exceptions\":[" +
         EntryJson.str() + "]}";
}

// Every declared kind must be usable when its fields are valid, and each
// malformed field must fail instead of being treated as an absent exception.
TEST(LegalizerExceptionLedger, ExceptionLedgerKindContract) {
  auto Parses = [](const LedgerEntryFields &F, std::string &Error) {
    return parseLegalizerExceptions(ledgerDocument(ledgerEntryJson(F)), Error)
        .has_value();
  };

  struct KindCase {
    const char *Kind;
    const char *LinkedLedger;
    const char *PR;
  };
  static const KindCase Kinds[] = {
      {"unreachable", "WitnessDescriptorAtoms", "TBD"},
      {"residual-only", "WitnessDescriptorAtoms", "TBD"},
      {"mir-exclusion", "WitnessDecisionDifferential", "TBD"},
      {"fuzz-budget", "WitnessDescriptorAtoms", "TBD"},
      {"reference-rebaseline", "WitnessDescriptorAtoms", "TBD"},
      {"ci-budget-waiver", "WitnessDescriptorAtoms", "TBD"},
      {"nonliteral-shape", "WitnessDescriptorAtoms", "TBD"},
      {"judgment", "WitnessDescriptorAtoms", "TBD"},
      {"prerequisite-pending", "WitnessDescriptorAtoms", "PR-F1"},
      {"reference-retirement", "WitnessDescriptorAtoms", "TBD"},
  };
  for (const KindCase &C : Kinds) {
    LedgerEntryFields F;
    F.Kind = C.Kind;
    F.LinkedLedger = C.LinkedLedger;
    F.PR = C.PR;
    std::string Error;
    EXPECT_TRUE(Parses(F, Error)) << C.Kind << ": " << Error;
  }

  auto ExpectFailure = [&](const LedgerEntryFields &F, StringRef What) {
    std::string Error;
    EXPECT_FALSE(Parses(F, Error)) << What << " unexpectedly parsed";
    EXPECT_FALSE(Error.empty()) << What << " failed without a message";
  };

  {
    LedgerEntryFields F;
    F.Kind = "not-a-kind";
    ExpectFailure(F, "unknown kind");
  }
  {
    LedgerEntryFields F;
    F.Approver = "";
    ExpectFailure(F, "empty approver");
  }
  {
    LedgerEntryFields F;
    F.Proof = "";
    ExpectFailure(F, "empty proof");
  }
  {
    LedgerEntryFields F;
    F.LinkedLedger = std::nullopt;
    ExpectFailure(F, "missing linked_ledger");
  }
  {
    LedgerEntryFields F;
    F.PR = std::nullopt;
    ExpectFailure(F, "missing PR");
  }
  {
    LedgerEntryFields F;
    F.RuleIndex = "-1";
    ExpectFailure(F, "negative rule_index");
  }
  {
    LedgerEntryFields F;
    F.Kind = "prerequisite-pending";
    F.PR = "TBD";
    ExpectFailure(F, "prerequisite-pending with wrong PR");
  }

  // The committed ledger must parse and only use declared kinds.
  std::string Error;
  std::optional<std::vector<LegalizerException>> Committed =
      readLegalizerExceptions(Error);
  ASSERT_TRUE(Committed.has_value()) << Error;
  for (const LegalizerException &E : *Committed)
    EXPECT_TRUE(isLegalizerExceptionKind(E.Kind))
        << E.RuleSet << "#" << E.RuleIndex << ": " << E.Kind;
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
        // Only the element-size mutations rebuild an LLT whose element may
        // not be a pointer; element-count mutations build a fresh vector and
        // are safe on pointer vectors (see actionChangesElementSize).
        if (actionChangesElementSize(Hit->Step.Action) &&
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

// The committed manifest is the single source of the rule denominators.  A
// Core IR change updates one file; these tests must not need a second edit.
static int64_t manifestCount(StringRef Field) {
  std::optional<std::string> Text =
      readTestSourceFile("x86-legalizer-rule-counts.json");
  if (!Text)
    return -1;
  llvm::Expected<json::Value> Value = json::parse(*Text);
  if (!Value) {
    consumeError(Value.takeError());
    return -1;
  }
  const json::Object *O = Value->getAsObject();
  if (!O)
    return -1;
  return O->getInteger(Field).value_or(-1);
}

// The denominator is the manifest's literal `.td` rules, keyed by the same
// (ruleSet, ruleIndex) the Core IR records.  The ledger records the first-match
// witness of every rule the domain can reach.  A rule that cannot first-match
// must be approved in x86-legalizer-exceptions.json, so a new miss is a hard
// failure rather than a silently reduced denominator.
TEST_F(X86LegalizerDifferentialTest, LiteralRuleFirstMatchLedger) {
  std::string Error;
  std::optional<std::map<RuleKey, LiteralTarget>> Targets =
      readLiteralTargets(Error);
  ASSERT_TRUE(Targets.has_value()) << Error;
  const int64_t LiteralDenominator = manifestCount("literal_td_rule_count");
  ASSERT_GT(LiteralDenominator, 0) << "cannot read the rule-count manifest";
  ASSERT_EQ(static_cast<int64_t>(Targets->size()), LiteralDenominator)
      << "literal rule denominator must be the manifest's distinct "
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
    // Only `unreachable` means this literal rule has no atom.  A kind this
    // ledger does not accept is a hard failure, not a silent skip.
    if (E.Kind != "unreachable") {
      if (E.LinkedLedger == "LiteralRuleFirstMatchLedger")
        ADD_FAILURE() << "LiteralRuleFirstMatchLedger does not accept kind '"
                      << E.Kind << "' for " << E.RuleSet << "#" << E.RuleIndex;
      continue;
    }
    ASSERT_TRUE(Approved.emplace(Key, &E).second)
        << "duplicate exception for " << E.RuleSet << "#" << E.RuleIndex;
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

// Reconstruct the committed witness corpus as table points.  The T2 property
// tests walk the same atoms T1 walks, but ask a different question of each
// one, so they share this reader instead of growing a second JSON parser.
static std::optional<std::vector<WitnessAtom>>
readWitnessAtoms(std::string &Error) {
  std::optional<std::string> Text =
      readTestSourceFile("x86-legalizer-witness.json");
  if (!Text) {
    Error = "cannot read x86-legalizer-witness.json from " +
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
    Error = "witness root is not an object";
    return std::nullopt;
  }
  const json::Array *AtomArray = Root->getArray("atoms");
  if (!AtomArray) {
    Error = "witness has no atoms array";
    return std::nullopt;
  }

  std::map<std::string, unsigned> TableForSet;
  for (unsigned I = 0; I != X86TestNumTables; ++I)
    TableForSet.emplace(X86RuleIndexTableNames[I], I);

  std::vector<WitnessAtom> Atoms;
  Atoms.reserve(AtomArray->size());
  for (const json::Value &AtomValue : *AtomArray) {
    const json::Object *O = AtomValue.getAsObject();
    if (!O) {
      Error = "witness atom is not an object";
      return std::nullopt;
    }
    auto Id = O->getString("id");
    auto Origin = O->getString("origin");
    auto RuleSet = O->getString("rule_set");
    auto ConfigID = O->getString("config");
    if (!Id || !Origin || !RuleSet || !ConfigID) {
      Error = "witness atom is missing id/origin/rule_set/config";
      return std::nullopt;
    }
    auto TableIt = TableForSet.find(RuleSet->str());
    if (TableIt == TableForSet.end()) {
      Error = "unknown rule set " + RuleSet->str();
      return std::nullopt;
    }
    const X86TestTable &T = X86TestTables[TableIt->second];
    std::optional<unsigned> Opcode = opcodeForName(T.Opcode);
    if (!Opcode) {
      Error = "no TargetOpcode for " + std::string(T.Opcode);
      return std::nullopt;
    }

    const json::Object *Query = O->getObject("query");
    if (!Query) {
      Error = Id->str() + " has no query";
      return std::nullopt;
    }
    const json::Array *TypeArray = Query->getArray("types");
    if (!TypeArray) {
      Error = Id->str() + " has no types";
      return std::nullopt;
    }
    if (TypeArray->size() > 3) {
      Error = Id->str() + " has more than three types";
      return std::nullopt;
    }

    WitnessAtom A;
    A.Point.Opcode = 0;
    A.Point.Name = nullptr;
    A.Point.NumTypes = 0;
    A.Point.Align = 0;
    A.Point.HasMem = false;
    A.Id = Id->str();
    A.Origin = Origin->str();
    A.RuleSet = RuleSet->str();
    if (auto R = O->getInteger("rule_index"))
      A.RuleIndex = *R;
    A.ConfigID = ConfigID->str();
    A.Point.Opcode = *Opcode;
    A.Point.Name = T.Opcode;
    A.Point.NumTypes = static_cast<uint8_t>(TypeArray->size());
    for (unsigned I = 0; I != TypeArray->size(); ++I) {
      auto Name = (*TypeArray)[I].getAsString();
      if (!Name) {
        Error = Id->str() + " has a non-string type";
        return std::nullopt;
      }
      auto Ty = parseWitnessLLT(*Name);
      if (!Ty) {
        Error = Id->str() + ": bad type " + Name->str();
        return std::nullopt;
      }
      A.Point.Types[I] = *Ty;
    }
    if (const json::Object *Mem = Query->getObject("mem")) {
      auto MemName = Mem->getString("type");
      auto Align = Mem->getInteger("align");
      if (!MemName || !Align) {
        Error = Id->str() + ": bad memdesc";
        return std::nullopt;
      }
      auto MemTy = parseWitnessLLT(*MemName);
      if (!MemTy) {
        Error = Id->str() + ": bad mem type " + MemName->str();
        return std::nullopt;
      }
      A.Point.MemTy = *MemTy;
      A.Point.Align = static_cast<uint64_t>(*Align);
      A.Point.HasMem = true;
    }

    const json::Object *ExpectedObj = O->getObject("expected");
    if (!ExpectedObj) {
      Error = Id->str() + " has no expected";
      return std::nullopt;
    }
    auto ExpectedAction = ExpectedObj->getString("action");
    if (!ExpectedAction) {
      Error = Id->str() + ": missing expected action";
      return std::nullopt;
    }
    auto Action = parseLegalizeAction(*ExpectedAction);
    if (!Action) {
      Error = Id->str() + ": unknown expected action " + ExpectedAction->str();
      return std::nullopt;
    }
    LLT NewType = LLT{};
    if (auto To = ExpectedObj->getString("to")) {
      auto Parsed = parseWitnessLLT(*To);
      if (!Parsed) {
        Error = Id->str() + ": bad expected type " + To->str();
        return std::nullopt;
      }
      NewType = *Parsed;
    }
    A.Expected = LegalizeActionStep(
        *Action,
        static_cast<unsigned>(ExpectedObj->getInteger("idx").value_or(0)),
        NewType);
    A.QueryText = witnessQueryText(A.Point);
    Atoms.push_back(std::move(A));
  }
  return Atoms;
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
  ASSERT_EQ(static_cast<int64_t>(Targets->size()),
            manifestCount("literal_td_rule_count"));

  std::optional<std::vector<LegalizerException>> Exceptions =
      readLegalizerExceptions(Error);
  ASSERT_TRUE(Exceptions.has_value()) << Error;

  std::optional<std::vector<EscapeTarget>> Escapes = readEscapeTargets(Error);
  ASSERT_TRUE(Escapes.has_value()) << Error;
  ASSERT_EQ(static_cast<int64_t>(Escapes->size()),
            manifestCount("escaped_td_rule_count"));

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

  std::set<RuleKey> ApprovedLiteral, ApprovedEscaped;
  for (const LegalizerException &E : *Exceptions) {
    // Only `unreachable` removes a rule from an atom denominator.  Coverage
    // kinds such as judgment/nonliteral-shape must not shrink the corpus.
    if (E.Kind != "unreachable")
      continue;
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
  const int64_t LiteralRules = manifestCount("literal_td_rule_count");
  ASSERT_GT(LiteralRules, 0);
  EXPECT_EQ(static_cast<int64_t>(RuleAtoms) +
                static_cast<int64_t>(ApprovedLiteral.size()),
            LiteralRules)
      << "every literal rule is a per-rule atom or an approved exception";
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
  const int64_t AtomCount = Root->getInteger("atom_count").value_or(-1);
  const int64_t PerRuleHits = Root->getInteger("per_rule_hits").value_or(-1);
  const int64_t BoundaryHits =
      Root->getInteger("boundary_atoms").value_or(-1);
  const int64_t ASHits = Root->getInteger("as_atoms").value_or(-1);
  const int64_t DescriptorHits =
      Root->getInteger("descriptor_atoms").value_or(-1);
  const int64_t DescriptorExceptions =
      Root->getInteger("descriptor_exceptions").value_or(-1);
  EXPECT_EQ(AtomCount, 1256);
  EXPECT_EQ(BoundaryHits, 756);
  EXPECT_EQ(ASHits, 220);
  EXPECT_EQ(PerRuleHits + DescriptorHits + BoundaryHits + ASHits, AtomCount)
      << "the witness header must account for every atom";
  EXPECT_EQ(DescriptorHits + DescriptorExceptions,
            manifestCount("escaped_td_rule_count"));
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
  const int64_t EscapeDenominator = manifestCount("escaped_td_rule_count");
  ASSERT_GT(EscapeDenominator, 0) << "cannot read the rule-count manifest";
  ASSERT_EQ(static_cast<int64_t>(ExpectedEscapePairs.size()),
            EscapeDenominator);

  // The manifest's literal rules minus every approved literal exception is the
  // set of rules a per-rule atom must name; approved escaped exceptions leave
  // the descriptor denominator instead.
  std::set<RuleKey> ExpectedRulePairs;
  for (const auto &Entry : *Targets)
    ExpectedRulePairs.insert(Entry.first);
  std::set<RuleKey> ApprovedEscapePairs;
  for (const LegalizerException &E : *Exceptions) {
    // Only `unreachable` means the rule has no atom.  A judgment or a
    // nonliteral-shape entry records coverage and must leave the atom in the
    // denominator it belongs to.
    if (E.Kind != "unreachable")
      continue;
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

  // Per-consumer acceptance: WitnessDescriptorAtoms accepts unreachable,
  // nonliteral-shape and judgment only.  An entry linked to another ledger is
  // that consumer's responsibility.
  for (const LegalizerException &E : *Exceptions) {
    const bool InDescriptorScope =
        E.LinkedLedger == "WitnessDescriptorAtoms" ||
        (E.Kind == "judgment" && namesWitnessAtom(E.LinkedLedger));
    if (!InDescriptorScope)
      continue;
    if (E.Kind != "unreachable" && E.Kind != "nonliteral-shape" &&
        E.Kind != "judgment") {
      ADD_FAILURE() << "WitnessDescriptorAtoms does not accept kind '" << E.Kind
                    << "' for " << E.RuleSet << "#" << E.RuleIndex;
      continue;
    }
    if (E.Kind == "nonliteral-shape" &&
        !isEscapedTarget(*EscapeTargets, E.RuleSet, E.RuleIndex))
      ADD_FAILURE() << "nonliteral-shape exception " << E.RuleSet << "#"
                    << E.RuleIndex << " must name an escaped rule";
  }
  ASSERT_EQ(static_cast<int64_t>(ExpectedEscapePairs.size() +
                                ApprovedEscapePairs.size()),
            manifestCount("escaped_td_rule_count"));

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
  EXPECT_EQ(BoundaryAtoms, 756u);
  EXPECT_EQ(ASAtoms, 220u);
  EXPECT_EQ(static_cast<int64_t>(DescriptorAtoms), EscapeDenominator);
  EXPECT_EQ(RulePairs, ExpectedRulePairs)
      << "per-rule atoms must name exactly the manifest's literal rules";
  EXPECT_EQ(static_cast<int64_t>(DescriptorPairs.size()), EscapeDenominator);
  EXPECT_EQ(DescriptorPairs, ExpectedEscapePairs)
      << "descriptor atoms must name every witnessable escaped rule; only "
         "approved unreachable exceptions may be absent";
  for (const LegalizerException &E : *Exceptions) {
    // Judgment and nonliteral-shape entries must never suppress a descriptor
    // atom or its decision comparison; only `unreachable` may be absent.
    if (E.Kind != "unreachable")
      continue;
    auto TableIt = TableForSet.find(E.RuleSet);
    ASSERT_NE(TableIt, TableForSet.end()) << E.RuleSet;
    EXPECT_EQ(DescriptorPairs.count({TableIt->second, E.RuleIndex}), 0u)
        << "unreachable exception " << E.RuleSet << "#" << E.RuleIndex
        << " unexpectedly has a descriptor atom";
  }
  EXPECT_EQ(static_cast<int64_t>(DescriptorPairs.size() +
                                ApprovedEscapePairs.size()),
            manifestCount("escaped_td_rule_count"))
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

//===----------------------------------------------------------------------===//
// T2 properties
//===----------------------------------------------------------------------===//
//
// T1 asks "is this witness's whole step right?".  T2 asks narrower questions
// that a table can get wrong without failing any single witness: an address
// space must never be folded back into a table cell, a memory descriptor's
// address space must be routed like the value's, and a named-type predicate
// must still agree after its slot-0 representative is replaced by another
// member of the same 24-class vocabulary.

class X86LegalizerProperties : public X86LegalizerDifferentialTest {};

struct PropertyFixture {
  Sigma S;
  std::unique_ptr<TableLegalizerInfoForTest> Table;
  std::unique_ptr<X86LegalizerInfoReference> Oracle;
};

static std::optional<PropertyFixture> makePropertyFixture(const Config &C) {
  PropertyFixture F;
  F.S = makeSigma(C);
  if (!F.S.STI)
    return std::nullopt;
  F.Table = std::make_unique<TableLegalizerInfoForTest>(*F.S.STI);
  F.Oracle = std::make_unique<X86LegalizerInfoReference>(
      *F.S.STI, *static_cast<const X86TargetMachine *>(F.S.TM.get()));
  return std::move(F);
}

static std::map<std::string, PropertyFixture> makePropertyFixtures() {
  std::map<std::string, PropertyFixture> Fixtures;
  for (const Config &C : FullConfigs) {
    std::optional<PropertyFixture> F = makePropertyFixture(C);
    if (!F)
      continue;
    Fixtures.emplace(C.ID, std::move(*F));
  }
  return Fixtures;
}

static const Config *findFullConfig(StringRef ID) {
  for (const Config &C : FullConfigs)
    if (ID == C.ID)
      return &C;
  return nullptr;
}

static const char *answerName(X86LegalizerTable::Answer A) {
  switch (A) {
  case X86LegalizerTable::Answer::Hit:
    return "Hit";
  case X86LegalizerTable::Answer::Residual:
    return "Residual";
  case X86LegalizerTable::Answer::Escape:
    return "Escape";
  case X86LegalizerTable::Answer::Outside:
    return "Outside";
  case X86LegalizerTable::Answer::Untabulated:
    return "Untabulated";
  }
  return "?";
}

// One property point.  `RequireResidual` is the AS contract: a query whose
// keyed types, memory descriptor or pointer-vector element carries a non-zero
// address space must leave the table cell and be answered on the original
// query.  Every point also requires the table's step to equal the reference,
// so a residual fallback cannot hide a wrong decision.
static LegalizeActionStep
checkPropertyPoint(const char *TestName, StringRef RuleSet, int64_t RuleIndex,
                   StringRef Origin, StringRef ConfigID, const X86TablePoint &P,
                   bool RequireResidual, const PropertyFixture &F,
                   unsigned &Checks, unsigned &Violations) {
  ++Checks;
  X86LegalizerTable::Result R;
  LegalizeActionStep Want(Unsupported, 0, LLT{});
  withAtomQuery(P, [&](const LegalityQuery &Q) {
    R = F.Table->table().query(Q);
    Want = F.Oracle->getAction(Q);
  });
  const LegalizeActionStep Got(R.Action, R.TypeIdx, R.NewType);
  const bool KindOk =
      !RequireResidual || R.Kind == X86LegalizerTable::Answer::Residual;
  if (KindOk && stepsAgree(Got, Want))
    return Want;
  ++Violations;
  ADD_FAILURE() << TestName << " rule-set=" << RuleSet << " rule=#"
                << (RuleIndex >= 0 ? std::to_string(RuleIndex) : "-")
                << " origin=" << Origin << " config=" << ConfigID
                << "\n  query:  " << witnessQueryText(P)
                << "\n  table:  " << describe(Got) << " [" << answerName(R.Kind)
                << "]"
                << "\n  reference: " << describe(Want);
  return Want;
}

// Every origin=as witness atom must be residual and agree with the reference,
// and every descriptor's p0 representative must still be a table Hit.  The
// two halves are complementary: the first proves the router fires, the second
// proves it does not over-fire on address space 0.
TEST_F(X86LegalizerProperties, AddressSpaceSoundness) {
  std::string Error;
  std::optional<std::vector<WitnessAtom>> Atoms = readWitnessAtoms(Error);
  ASSERT_TRUE(Atoms.has_value()) << Error;

  std::map<std::string, PropertyFixture> Fixtures = makePropertyFixtures();
  ASSERT_EQ(Fixtures.size(), 4u);

  unsigned Checks = 0, Violations = 0, ASAtoms = 0;
  for (const WitnessAtom &A : *Atoms) {
    if (A.Origin != "as")
      continue;
    ++ASAtoms;
    auto It = Fixtures.find(A.ConfigID);
    ASSERT_NE(It, Fixtures.end()) << A.Id << ": unknown config " << A.ConfigID;
    checkPropertyPoint("AddressSpaceSoundness", A.RuleSet, A.RuleIndex,
                       A.Origin, A.ConfigID, A.Point, /*RequireResidual=*/true,
                       It->second, Checks, Violations);
  }
  EXPECT_EQ(ASAtoms, 220u);

  // The 63 descriptors' p0 representatives are the `kind=hit` behavior
  // queries: classifiable types, address space 0, no pointer vector.
  auto QueriesText = readTestSourceFile("x86-legalizer-behavior-queries.json");
  ASSERT_TRUE(QueriesText.has_value());
  auto QueriesValue = json::parse(*QueriesText);
  ASSERT_TRUE(!!QueriesValue) << toString(QueriesValue.takeError());
  const json::Object *Root = QueriesValue->getAsObject();
  ASSERT_NE(Root, nullptr);
  const json::Array *QueryArray = Root->getArray("queries");
  ASSERT_NE(QueryArray, nullptr);

  unsigned Descriptors = 0;
  for (const json::Value &V : *QueryArray) {
    const json::Object *O = V.getAsObject();
    ASSERT_NE(O, nullptr);
    std::optional<BehaviorQuery> BQ = parseBehaviorQuery(*O);
    ASSERT_TRUE(BQ.has_value());
    if (BQ->Kind != "hit")
      continue;
    ++Descriptors;
    const Config *C = findFullConfig(BQ->Config);
    ASSERT_NE(C, nullptr) << BQ->Config;
    auto FixtureIt = Fixtures.find(BQ->Config);
    ASSERT_NE(FixtureIt, Fixtures.end()) << BQ->Config;
    SmallVector<LLT, 3> Types;
    SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
    std::optional<unsigned> Opcode =
        buildBehaviorQuery(*BQ, C->Is64 ? 64 : 32, Types, MemDescs);
    ASSERT_TRUE(Opcode.has_value()) << BQ->Opcode;
    const LegalityQuery Q(*Opcode, Types, MemDescs);
    const X86LegalizerTable::Result R =
        FixtureIt->second.Table->table().query(Q);
    ++Checks;
    if (R.Kind == X86LegalizerTable::Answer::Hit)
      continue;
    ++Violations;
    ADD_FAILURE() << "AddressSpaceSoundness rule-set=" << BQ->Descriptor
                  << " rule=#- origin=p0-representative config=" << BQ->Config
                  << "\n  query:  " << BQ->Id
                  << "\n  table:  " << answerName(R.Kind)
                  << "\n  reference: Answer::Hit expected for p0";
  }
  EXPECT_EQ(Descriptors, 63u);

  llvm::outs() << "AddressSpaceSoundness: checks=" << Checks
               << " violations=" << Violations << " as-atoms=" << ASAtoms
               << " descriptors=" << Descriptors << "\n";
  EXPECT_EQ(Violations, 0u);
}

// A memory rule set's descriptor is a second address-space carrier.  The four
// points below pin both carriers: value AS, memory AS, and their combination.
// The reference's G_STORE guard `Types[0] != MemoryTy` makes (p1)[mem p0] and
// (p1)[mem p1] distinguishable decisions; a router that only looks at
// Q.Types would answer both from the same (p1, p0) cell.
TEST_F(X86LegalizerProperties, MemSymmetricAddressSpace) {
  std::optional<PropertyFixture> F = makePropertyFixture(FullConfigs[2]);
  ASSERT_TRUE(F.has_value()) << FullConfigs[2].ID;
  const Config &C = FullConfigs[2];
  const TableDomainAxis A = tableDomainAxis(64);
  const uint64_t Align = alignForClass(1);

  unsigned Checks = 0, Violations = 0, MemSets = 0, DistinguishingMemSets = 0;
  for (unsigned TI = 0; TI != X86TestNumTables; ++TI) {
    const X86TestTable &T = X86TestTables[TI];
    if (!T.HasMem)
      continue;
    ++MemSets;
    const std::optional<unsigned> Opcode = opcodeForName(T.Opcode);
    ASSERT_TRUE(Opcode.has_value()) << T.Opcode;
    auto MakePoint = [&](LLT Value, LLT Ptr, LLT Mem) {
      X86TablePoint P;
      P.Opcode = *Opcode;
      P.Name = T.Opcode;
      P.NumTypes = 2;
      P.Types[0] = Value;
      P.Types[1] = Ptr;
      P.MemTy = Mem;
      P.Align = Align;
      P.HasMem = true;
      return P;
    };

    const X86TablePoint P0M1 = MakePoint(A.P0, A.P0, A.P1);
    const X86TablePoint P1M0 = MakePoint(A.P1, A.P0, A.P0);
    const X86TablePoint P1M1 = MakePoint(A.P1, A.P0, A.P1);
    const X86TablePoint P1M2 = MakePoint(A.P1, A.P0, A.P2);

    checkPropertyPoint("MemSymmetricAddressSpace", T.Name, -1,
                       "value=p0 mem=p1", C.ID, P0M1, /*RequireResidual=*/true,
                       *F, Checks, Violations);
    const LegalizeActionStep W1M0 = checkPropertyPoint(
        "MemSymmetricAddressSpace", T.Name, -1, "value=p1 mem=p0", C.ID, P1M0,
        /*RequireResidual=*/true, *F, Checks, Violations);
    const LegalizeActionStep W1M1 = checkPropertyPoint(
        "MemSymmetricAddressSpace", T.Name, -1, "value=p1 mem=p1", C.ID, P1M1,
        /*RequireResidual=*/true, *F, Checks, Violations);
    checkPropertyPoint("MemSymmetricAddressSpace", T.Name, -1,
                       "value=p1 mem=p2", C.ID, P1M2, /*RequireResidual=*/true,
                       *F, Checks, Violations);

    if (!stepsAgree(W1M0, W1M1)) {
      ++DistinguishingMemSets;
      continue;
    }
    // The `Types[0] != MemoryTy` guard lives in the store rules; the other
    // five memory sets have no such decision to distinguish, so the
    // discriminator is asserted where the reference actually encodes it.
    if (StringRef(T.Name) != "X86_STORE")
      continue;
    ++Violations;
    ADD_FAILURE() << "MemSymmetricAddressSpace rule-set=" << T.Name
                  << " rule=#- origin=mem-symmetric config=" << C.ID
                  << "\n  reference decisions must differ: " << describe(W1M0)
                  << " vs " << describe(W1M1);
  }
  EXPECT_EQ(MemSets, 6u);
  EXPECT_GE(DistinguishingMemSets, 1u);

  llvm::outs() << "MemSymmetricAddressSpace: checks=" << Checks
               << " violations=" << Violations << " mem-sets=" << MemSets
               << " distinguishing-mem-sets=" << DistinguishingMemSets << "\n";
  EXPECT_EQ(Violations, 0u);
}

// Pointer vectors carry the address space in their element type, not in the
// pointer class.  Every rule set gets a v2p1 and v4p1 slot-0 point; the 34
// rule sets that read slot 1 get the same two types in slot 1.
TEST_F(X86LegalizerProperties, PointerVectorAddressSpace) {
  std::optional<PropertyFixture> F = makePropertyFixture(FullConfigs[2]);
  ASSERT_TRUE(F.has_value()) << FullConfigs[2].ID;
  const Config &C = FullConfigs[2];
  const TableDomainAxis A = tableDomainAxis(64);
  const uint64_t Align = alignForClass(1);

  unsigned Checks = 0, Violations = 0, Slot0Sets = 0, Slot1Sets = 0;
  for (unsigned TI = 0; TI != X86TestNumTables; ++TI) {
    const X86TestTable &T = X86TestTables[TI];
    const std::optional<unsigned> Opcode = opcodeForName(T.Opcode);
    ASSERT_TRUE(Opcode.has_value()) << T.Opcode;
    auto MakePoint = [&](ArrayRef<LLT> Types, bool HasMem, LLT MemTy) {
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
    const bool HasMem = T.HasMem;
    const LLT MemTy = HasMem ? A.P0 : LLT{};

    ++Slot0Sets;
    if (T.NumSlots == 1) {
      checkPropertyPoint("PointerVectorAddressSpace", T.Name, -1,
                         "ptr-vector-slot0", C.ID,
                         MakePoint({A.V2P1}, false, LLT{}),
                         /*RequireResidual=*/true, *F, Checks, Violations);
      checkPropertyPoint("PointerVectorAddressSpace", T.Name, -1,
                         "ptr-vector-slot0", C.ID,
                         MakePoint({A.V4P1}, false, LLT{}),
                         /*RequireResidual=*/true, *F, Checks, Violations);
    } else {
      checkPropertyPoint("PointerVectorAddressSpace", T.Name, -1,
                         "ptr-vector-slot0", C.ID,
                         MakePoint({A.V2P1, A.P0}, HasMem, MemTy),
                         /*RequireResidual=*/true, *F, Checks, Violations);
      checkPropertyPoint("PointerVectorAddressSpace", T.Name, -1,
                         "ptr-vector-slot0", C.ID,
                         MakePoint({A.V4P1, A.P0}, HasMem, MemTy),
                         /*RequireResidual=*/true, *F, Checks, Violations);
    }

    if (T.NumSlots < 2)
      continue;
    ++Slot1Sets;
    checkPropertyPoint("PointerVectorAddressSpace", T.Name, -1,
                       "ptr-vector-slot1", C.ID,
                       MakePoint({A.P0, A.V2P1}, HasMem, MemTy),
                       /*RequireResidual=*/true, *F, Checks, Violations);
    checkPropertyPoint("PointerVectorAddressSpace", T.Name, -1,
                       "ptr-vector-slot1", C.ID,
                       MakePoint({A.P0, A.V4P1}, HasMem, MemTy),
                       /*RequireResidual=*/true, *F, Checks, Violations);
  }
  EXPECT_EQ(Slot0Sets, 63u);
  EXPECT_EQ(Slot1Sets, 34u);
  EXPECT_EQ(Checks, 63u * 2 + 34u * 2);

  llvm::outs() << "PointerVectorAddressSpace: checks=" << Checks
               << " violations=" << Violations << " slot0-sets=" << Slot0Sets
               << " slot1-sets=" << Slot1Sets << "\n";
  EXPECT_EQ(Violations, 0u);
}

// A named type (`sMaxScalar`, `s32MaxVector`, ...) is expanded against the
// configuration before any rule compares it.  Relativizing a per-rule witness
// by replacing slot 0 with another member of the same 24-class vocabulary
// keeps the query shape but changes the named-type expansion; the table and
// the reference must still agree.  This does not require the same action as
// the original witness -- only that both sides answer the new query alike.
//
// The generated test metadata exposes the class kind (Pointer/Scalar/Vector/
// PtrVector) but not the .td named-type list membership, so the replacement
// stays inside the same kind.  A pointer has no same-kind peer and falls back
// to s64; a scalar or vector outside the class list falls back to s32 or
// v4s32, which are 24-class members of the same kind.
TEST_F(X86LegalizerProperties, NamedTypeRelativization) {
  std::string Error;
  std::optional<std::vector<WitnessAtom>> Atoms = readWitnessAtoms(Error);
  ASSERT_TRUE(Atoms.has_value()) << Error;

  std::map<std::string, PropertyFixture> Fixtures = makePropertyFixtures();
  ASSERT_EQ(Fixtures.size(), 4u);

  const std::vector<LLT> Reps = typeClassReps(64);
  ASSERT_EQ(Reps.size(), 24u);
  auto Replacement = [&](LLT T) {
    int Found = -1;
    for (unsigned I = 0; I != Reps.size(); ++I)
      if (Reps[I] == T) {
        Found = static_cast<int>(I);
        break;
      }
    if (Found >= 0) {
      const uint8_t Kind = X86TestTypeClasses[Found].Kind;
      for (unsigned Step = 1; Step != Reps.size(); ++Step) {
        const unsigned J = (static_cast<unsigned>(Found) + Step) % Reps.size();
        if (X86TestTypeClasses[J].Kind == Kind)
          return Reps[J];
      }
    }
    // A pointer has no other member of its kind; s64 is the pointer-sized
    // scalar the named-type lists also contain.
    if (T.isPointer())
      return LLT::scalar(64);
    if (T.isPointerVector())
      return LLT::fixed_vector(2, LLT::pointer(0, 64));
    if (T.isVector())
      return LLT::fixed_vector(4, 32);
    return LLT::scalar(32);
  };

  unsigned Checks = 0, Violations = 0, RuleAtoms = 0;
  for (const WitnessAtom &A : *Atoms) {
    if (A.Origin != "rule")
      continue;
    ++RuleAtoms;
    auto It = Fixtures.find(A.ConfigID);
    ASSERT_NE(It, Fixtures.end()) << A.Id << ": unknown config " << A.ConfigID;
    X86TablePoint P = A.Point;
    const LLT Before = P.Types[0];
    const LLT After = Replacement(Before);
    EXPECT_NE(After, Before) << A.Id << ": slot-0 substitution was a no-op";
    P.Types[0] = After;
    checkPropertyPoint("NamedTypeRelativization", A.RuleSet, A.RuleIndex,
                       "named-type", A.ConfigID, P, /*RequireResidual=*/false,
                       It->second, Checks, Violations);
  }
  // The per-rule atoms are the literal rules minus the approved unreachable
  // ones.  Derive that denominator from the generated manifest instead of
  // pinning the current count.
  std::string TargetsError;
  std::optional<std::map<RuleKey, LiteralTarget>> LiteralTargets =
      readLiteralTargets(TargetsError);
  ASSERT_TRUE(LiteralTargets.has_value()) << TargetsError;
  std::string ExceptionsError;
  std::optional<std::vector<LegalizerException>> Exceptions =
      readLegalizerExceptions(ExceptionsError);
  ASSERT_TRUE(Exceptions.has_value()) << ExceptionsError;
  std::map<std::string, unsigned> TableForSet;
  for (unsigned I = 0; I != X86TestNumTables; ++I)
    TableForSet.emplace(X86RuleIndexTableNames[I], I);
  int64_t UnreachableLiterals = 0;
  for (const LegalizerException &E : *Exceptions) {
    if (E.Kind != "unreachable")
      continue;
    auto It = TableForSet.find(E.RuleSet);
    if (It != TableForSet.end() &&
        LiteralTargets->count({It->second, E.RuleIndex}))
      ++UnreachableLiterals;
  }
  EXPECT_EQ(static_cast<int64_t>(RuleAtoms),
            manifestCount("literal_td_rule_count") - UnreachableLiterals)
      << "per-rule atoms must name every reachable literal rule";

  llvm::outs() << "NamedTypeRelativization: checks=" << Checks
               << " violations=" << Violations << " rule-atoms=" << RuleAtoms
               << "\n";
  EXPECT_EQ(Violations, 0u);
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
// escape to a parse error, is visible.  X86_CTPOP's ClampScalar used to be the
// model case -- it took X86_sMaxScalar, a derived type, so the rule was copied
// into the .td verbatim; that type is now a structured select, so the rule is
// generated like any other.
TEST_F(X86LegalizerDifferentialTest, EscapeAccounting) {
  EXPECT_EQ(X86_PIN_ESCAPES_ACTUAL, X86_PIN_ESCAPES_RECORDED)
      << "escape count changed";
  EXPECT_EQ((int64_t)X86_PIN_ESCAPES_ACTUAL,
            manifestCount("escaped_td_rule_count"))
      << "the pinned escape count must be the manifest's denominator";
  EXPECT_EQ(X86_PIN_CTPOP_ESCAPES_ACTUAL, X86_PIN_CTPOP_ESCAPES_RECORDED)
      << "X86_CTPOP's clampScalar escape count changed";
  EXPECT_EQ(X86_PIN_CTPOP_ESCAPES_ACTUAL, 0);
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
// Mutation tests (G005)
//===----------------------------------------------------------------------===//
//
// The differential harness is falsified by fixing a set of mutants and proving
// every one is caught.  Fourteen mutants corrupt one rule of
// X86LegalizerInfo.td, in fourteen different rule sets; two more corrupt the
// frozen reference.  A mutant is killed when its first legalization step
// differs from the frozen reference for one committed witness atom under one
// configuration.
//
// All 1,256 committed atoms are replayed under their own witness configuration
// and under the six feature-matrix configurations.  The first kill in
// (configuration, atom) order is committed to x86-legalizer-mutants.json, so
// the evidence is reviewable, and a rerun that moves a witness fails instead
// of silently passing with a different atom.

struct MutantSpec {
  const char *Id;
  const char *Kind;
  const char *RuleSet;
  std::unique_ptr<LegalizerInfo> (*Make)(const X86Subtarget &,
                                         const X86TargetMachine &);
};

static const MutantSpec Mutants[] = {
    {"td-01/X86LegalizerInfoMutant1", "td", "X86_CTPOP",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant1::Mutant>(STI);
     }},
    {"td-02/X86LegalizerInfoMutant2", "td", "X86_CTLZ",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant2::Mutant>(STI);
     }},
    {"td-03/X86LegalizerInfoMutant3", "td", "X86_CTTZ",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant3::Mutant>(STI);
     }},
    {"td-04/X86LegalizerInfoMutant4", "td", "X86_MUL",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant4::Mutant>(STI);
     }},
    {"td-05/X86LegalizerInfoMutant5", "td", "X86_ADD",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant5::Mutant>(STI);
     }},
    {"td-06/X86LegalizerInfoMutant6", "td", "X86_IMPLICIT_DEF",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant6::Mutant>(STI);
     }},
    {"td-07/X86LegalizerInfoMutant7", "td", "X86_SELECT",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant7::Mutant>(STI);
     }},
    {"td-08/X86LegalizerInfoMutant8", "td", "X86_FNEG",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant8::Mutant>(STI);
     }},
    {"td-09/X86LegalizerInfoMutant9", "td", "X86_CONCAT_VECTORS",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant9::Mutant>(STI);
     }},
    {"td-10/X86LegalizerInfoMutant10", "td", "X86_INSERT",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant10::Mutant>(STI);
     }},
    {"td-11/X86LegalizerInfoMutant11", "td", "X86_CTTZ_ZERO_POISON",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant11::Mutant>(STI);
     }},
    {"td-12/X86LegalizerInfoMutant12", "td", "X86_BSWAP",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant12::Mutant>(STI);
     }},
    {"td-13/X86LegalizerInfoMutant13", "td", "X86_IS_FPCLASS",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant13::Mutant>(STI);
     }},
    {"td-14/X86LegalizerInfoMutant14", "td", "X86_ANYEXT",
     [](const X86Subtarget &STI,
        const X86TargetMachine &) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<mutant14::Mutant>(STI);
     }},
    {"ref-01/X86LegalizerInfoReferenceMutant1", "reference", "X86_CTLZ",
     [](const X86Subtarget &STI,
        const X86TargetMachine &TM) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<X86LegalizerInfoReferenceMutant1>(STI, TM);
     }},
    {"ref-02/X86LegalizerInfoReferenceMutant2", "reference", "X86_CTPOP",
     [](const X86Subtarget &STI,
        const X86TargetMachine &TM) -> std::unique_ptr<LegalizerInfo> {
       return std::make_unique<X86LegalizerInfoReferenceMutant2>(STI, TM);
     }},
};
static_assert(std::size(Mutants) == 16, "the manifest is fixed at 16 mutants");

// A witness configuration and the six feature-matrix configurations, in the
// order the kill search visits them.  Matrix first: when a mutant is sensitive
// to a feature, that is the more interesting witness than the atom's own
// configuration.
struct KillConfig {
  std::string Label;
  const Config *C;
};

static std::vector<KillConfig> makeKillConfigs() {
  std::vector<KillConfig> ConfigsForKills;
  for (unsigned I = 0; I != std::size(MatrixConfigs); ++I)
    ConfigsForKills.push_back({MatrixConfigNames[I], &MatrixConfigs[I]});
  for (const Config &C : FullConfigs)
    ConfigsForKills.push_back({std::string("witness-") + C.ID, &C});
  return ConfigsForKills;
}

static bool sameSubtargetConfig(const Config &A, const Config &B) {
  return StringRef(A.Triple) == StringRef(B.Triple) &&
         StringRef(A.FS) == StringRef(B.FS);
}

struct MutantKillRecord {
  std::string Config;
  std::string AtomId;
};

struct KillMatrixResult {
  // Indexed like Mutants; engaged when at least one atom killed the mutant.
  std::vector<std::optional<MutantKillRecord>> FirstKill;
  // Configuration label -> the mutants that configuration kills.
  std::map<std::string, std::set<std::string>> KilledByConfig;
  unsigned Atoms = 0;
};

// A 64-bit pointer query against an i386 subtarget can reach clampScalarOrElt
// with a pointer and assert inside LLT::changeElementSize.  The witness atoms
// are all 64-bit pointers, so the i386 witness configurations are skipped for
// them; the six feature-matrix configurations are x86_64 and still see every
// atom.  This is a property of replaying a query outside its own subtarget,
// not a mutant exemption.
static bool tablePointHasPointer(const X86TablePoint &P) {
  for (unsigned I = 0; I != P.NumTypes; ++I)
    if (P.Types[I].isPointer() || P.Types[I].isPointerVector())
      return true;
  return P.HasMem && (P.MemTy.isPointer() || P.MemTy.isPointerVector());
}

static KillMatrixResult computeKillMatrix() {
  KillMatrixResult Result;
  Result.FirstKill.resize(std::size(Mutants));

  std::string Error;
  std::optional<std::vector<WitnessAtom>> Atoms = readWitnessAtoms(Error);
  if (!Atoms) {
    ADD_FAILURE() << Error;
    return Result;
  }
  Result.Atoms = Atoms->size();

  const std::vector<KillConfig> ConfigsForKills = makeKillConfigs();
  std::vector<Sigma> Sigmas;
  Sigmas.reserve(ConfigsForKills.size());
  for (const KillConfig &KC : ConfigsForKills) {
    Sigmas.push_back(makeSigma(*KC.C));
    if (!Sigmas.back().STI) {
      ADD_FAILURE() << "no subtarget for " << KC.Label;
      return Result;
    }
  }

  for (unsigned CI = 0; CI != ConfigsForKills.size(); ++CI) {
    const X86TargetMachine &TM =
        *static_cast<const X86TargetMachine *>(Sigmas[CI].TM.get());
    X86LegalizerInfoReference Oracle(*Sigmas[CI].STI, TM);
    std::vector<std::unique_ptr<LegalizerInfo>> Instances;
    Instances.reserve(std::size(Mutants));
    for (const MutantSpec &M : Mutants)
      Instances.push_back(M.Make(*Sigmas[CI].STI, TM));

    for (const WitnessAtom &A : *Atoms) {
      if (!ConfigsForKills[CI].C->Is64 && tablePointHasPointer(A.Point))
        continue;
      LegalizeActionStep Want{Unsupported, 0, LLT{}};
      withAtomQuery(A.Point, [&](const LegalityQuery &Q) {
        Want = Oracle.getAction(Q);
        for (unsigned MI = 0; MI != Instances.size(); ++MI) {
          if (stepsAgree(Want, Instances[MI]->getAction(Q)))
            continue;
          Result.KilledByConfig[ConfigsForKills[CI].Label].insert(
              Mutants[MI].Id);
          if (!Result.FirstKill[MI])
            Result.FirstKill[MI] =
                MutantKillRecord{ConfigsForKills[CI].Label, A.Id};
        }
      });
    }
  }
  return Result;
}

// Replay one committed (mutant, config, atom) triple.  The manifest test uses
// this instead of trusting the computed matrix: a stale entry must fail even
// when some other atom still kills the mutant.
static bool reproducesKill(const MutantSpec &M, StringRef ConfigLabel,
                           StringRef AtomId) {
  std::string Error;
  std::optional<std::vector<WitnessAtom>> Atoms = readWitnessAtoms(Error);
  if (!Atoms) {
    ADD_FAILURE() << Error;
    return false;
  }
  const Config *C = nullptr;
  for (unsigned I = 0; I != std::size(MatrixConfigs); ++I)
    if (ConfigLabel == MatrixConfigNames[I])
      C = &MatrixConfigs[I];
  for (const Config &FC : FullConfigs) {
    const std::string Label = "witness-" + std::string(FC.ID);
    if (ConfigLabel == StringRef(Label))
      C = &FC;
  }
  if (!C) {
    ADD_FAILURE() << M.Id << ": manifest names unknown config " << ConfigLabel;
    return false;
  }
  Sigma S = makeSigma(*C);
  if (!S.STI)
    return false;
  const X86TargetMachine &TM =
      *static_cast<const X86TargetMachine *>(S.TM.get());
  X86LegalizerInfoReference Oracle(*S.STI, TM);
  std::unique_ptr<LegalizerInfo> Mutant = M.Make(*S.STI, TM);
  for (const WitnessAtom &A : *Atoms) {
    if (A.Id != AtomId)
      continue;
    if (!C->Is64 && tablePointHasPointer(A.Point)) {
      ADD_FAILURE() << M.Id << ": manifest pairs a 64-bit pointer atom with "
                    << "an i386 config";
      return false;
    }
    LegalizeActionStep Want{Unsupported, 0, LLT{}};
    LegalizeActionStep Got{Unsupported, 0, LLT{}};
    withAtomQuery(A.Point, [&](const LegalityQuery &Q) {
      Want = Oracle.getAction(Q);
      Got = Mutant->getAction(Q);
    });
    return !stepsAgree(Want, Got);
  }
  ADD_FAILURE() << M.Id << ": manifest names unknown atom " << AtomId;
  return false;
}

struct ManifestEntry {
  std::string MutantId;
  std::string Kind;
  std::string RuleSet;
  std::string Config;
  std::string AtomId;
};

static std::string renderMutantManifest(const KillMatrixResult &KM) {
  std::vector<const MutantSpec *> Sorted;
  Sorted.reserve(std::size(Mutants));
  for (const MutantSpec &M : Mutants)
    Sorted.push_back(&M);
  std::sort(Sorted.begin(), Sorted.end(),
            [](const MutantSpec *A, const MutantSpec *B) {
              return StringRef(A->Id).compare(StringRef(B->Id)) < 0;
            });

  std::string Out;
  raw_string_ostream OS(Out);
  OS << "{\n";
  OS << "  \"schema\": \"llvm.gisel.x86-legalizer-mutants\",\n";
  OS << "  \"version\": 1,\n";
  OS << "  \"comment\": \"One killing (config, atom) pair per mutant, "
        "replayed from x86-legalizer-witness.json by MutationKillMatrix.  "
        "Entries are sorted by mutant_id; the first kill in (configuration, "
        "atom) order is recorded.\",\n";
  OS << "  \"count\": " << Sorted.size() << ",\n";
  OS << "  \"mutants\": [\n";
  for (size_t I = 0; I != Sorted.size(); ++I) {
    const MutantSpec &M = *Sorted[I];
    const unsigned Index = static_cast<unsigned>(&M - Mutants);
    const std::optional<MutantKillRecord> &K = KM.FirstKill[Index];
    OS << "    {\"mutant_id\": \"" << M.Id << "\", \"kind\": \"" << M.Kind
       << "\", \"rule_set\": \"" << M.RuleSet << "\", \"config\": \""
       << (K ? K->Config : "") << "\", \"killing_atom_id\": \""
       << (K ? K->AtomId : "") << "\"}";
    OS << (I + 1 == Sorted.size() ? "\n" : ",\n");
  }
  OS << "  ]\n}\n";
  return OS.str();
}

// Not a test.  Regenerate the committed manifest after an intentional mutant
// change:
//
//   X86_MUTANTS_OUT=<abs path> X86Tests --gtest_also_run_disabled_tests \
//     --gtest_filter='*GenerateMutantManifest*'
TEST_F(X86LegalizerDifferentialTest, DISABLED_GenerateMutantManifest) {
  const char *OutPath = std::getenv("X86_MUTANTS_OUT");
  if (!OutPath)
    GTEST_SKIP() << "set X86_MUTANTS_OUT to the absolute output path";

  const KillMatrixResult KM = computeKillMatrix();
  for (unsigned I = 0; I != std::size(Mutants); ++I)
    ASSERT_TRUE(KM.FirstKill[I]) << Mutants[I].Id << " was not killed";

  std::error_code EC;
  raw_fd_ostream OS(OutPath, EC);
  ASSERT_FALSE(EC) << OutPath << ": " << EC.message();
  OS << renderMutantManifest(KM);
  OS.flush();
  ASSERT_FALSE(OS.has_error()) << OutPath;
}

TEST_F(X86LegalizerDifferentialTest, MutationKillMatrix) {
  std::optional<std::string> Text =
      readTestSourceFile("x86-legalizer-mutants.json");
  ASSERT_TRUE(Text.has_value())
      << "cannot read x86-legalizer-mutants.json from "
      << X86_LEGALIZER_TEST_SRC_DIR;
  auto Value = json::parse(*Text);
  ASSERT_TRUE(!!Value) << toString(Value.takeError());
  const json::Object *Root = Value->getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getString("schema").value_or(""),
            "llvm.gisel.x86-legalizer-mutants");
  EXPECT_EQ(Root->getInteger("version").value_or(-1), 1);
  EXPECT_EQ(Root->getInteger("count").value_or(-1), 16);

  const json::Array *Entries = Root->getArray("mutants");
  ASSERT_NE(Entries, nullptr);
  ASSERT_EQ(Entries->size(), 16u) << "the manifest must have exactly 16 rows";

  std::map<std::string, ManifestEntry> Manifest;
  for (const json::Value &V : *Entries) {
    const json::Object *O = V.getAsObject();
    ASSERT_NE(O, nullptr);
    ManifestEntry E;
    E.MutantId = O->getString("mutant_id").value_or("").str();
    E.Kind = O->getString("kind").value_or("").str();
    E.RuleSet = O->getString("rule_set").value_or("").str();
    E.Config = O->getString("config").value_or("").str();
    E.AtomId = O->getString("killing_atom_id").value_or("").str();
    EXPECT_FALSE(E.MutantId.empty());
    EXPECT_FALSE(E.Kind.empty());
    EXPECT_FALSE(E.RuleSet.empty());
    EXPECT_FALSE(E.Config.empty());
    EXPECT_FALSE(E.AtomId.empty());
    EXPECT_TRUE(E.Kind == "td" || E.Kind == "reference") << E.MutantId;
    EXPECT_TRUE(Manifest.emplace(E.MutantId, E).second)
        << "duplicate mutant_id " << E.MutantId;
  }
  ASSERT_EQ(Manifest.size(), 16u) << "manifest mutant ids are not unique";

  unsigned TDCount = 0, ReferenceCount = 0;
  std::set<std::string> TDRuleSets;
  for (const auto &Entry : Manifest) {
    if (Entry.second.Kind == "td") {
      ++TDCount;
      TDRuleSets.insert(Entry.second.RuleSet);
    } else if (Entry.second.Kind == "reference") {
      ++ReferenceCount;
    }
  }
  EXPECT_EQ(TDCount, 14u);
  EXPECT_EQ(ReferenceCount, 2u);
  EXPECT_EQ(TDRuleSets.size(), 14u)
      << "the 14 .td mutants must cover 14 different rule sets";

  const KillMatrixResult KM = computeKillMatrix();
  ASSERT_EQ(KM.Atoms, 1256u)
      << "the kill matrix must replay every witness atom";
  ASSERT_EQ(std::size(Mutants), 16u);

  unsigned Killed = 0;
  for (unsigned MI = 0; MI != std::size(Mutants); ++MI) {
    const MutantSpec &M = Mutants[MI];
    const auto It = Manifest.find(M.Id);
    ASSERT_TRUE(It != Manifest.end()) << "manifest is missing " << M.Id;
    const ManifestEntry &E = It->second;
    EXPECT_EQ(E.Kind, M.Kind) << M.Id;
    EXPECT_EQ(E.RuleSet, M.RuleSet) << M.Id;

    if (!KM.FirstKill[MI]) {
      ADD_FAILURE() << "mutant " << M.Id << " (" << M.Kind << ", " << M.RuleSet
                    << ") was not killed by any of " << KM.Atoms
                    << " witness atoms under any configuration";
      continue;
    }
    ++Killed;
    const MutantKillRecord &K = *KM.FirstKill[MI];
    EXPECT_EQ(E.Config, K.Config)
        << M.Id << ": manifest config is stale; first kill is now at "
        << K.Config;
    EXPECT_EQ(E.AtomId, K.AtomId)
        << M.Id << ": manifest killing atom moved; first kill is now "
        << K.AtomId;
    EXPECT_TRUE(reproducesKill(M, E.Config, E.AtomId))
        << M.Id << ": recorded (" << E.Config << ", " << E.AtomId
        << ") does not reproduce a kill";
  }
  EXPECT_EQ(Killed, 16u) << "every mutant must be killed";

  // The feature matrix is only useful if each configuration separates at least
  // one mutant from the reference.  A configuration that does not is accepted
  // only when its feature vector is exactly a witness configuration, i.e. the
  // witness corpus itself covers it.
  unsigned MatrixKills = 0;
  llvm::outs() << "feature matrix coverage:\n";
  for (unsigned I = 0; I != std::size(MatrixConfigs); ++I) {
    const std::string Label = MatrixConfigNames[I];
    const auto It = KM.KilledByConfig.find(Label);
    const unsigned KillCount =
        It == KM.KilledByConfig.end() ? 0 : It->second.size();
    MatrixKills += KillCount;
    bool WitnessCovered = false;
    for (const Config &C : FullConfigs)
      if (sameSubtargetConfig(C, MatrixConfigs[I]))
        WitnessCovered = true;
    EXPECT_TRUE(KillCount > 0 || WitnessCovered)
        << "feature-matrix config " << Label
        << " kills no mutant and is not covered by a witness config";
    llvm::outs() << "  " << Label << ": killed=" << KillCount
                 << " witness_covered=" << (WitnessCovered ? "yes" : "no")
                 << "\n";
  }
  EXPECT_GT(MatrixKills, 0u);

  llvm::outs() << "mutant kill matrix:\n";
  std::vector<const MutantSpec *> Sorted;
  for (const MutantSpec &M : Mutants)
    Sorted.push_back(&M);
  std::sort(Sorted.begin(), Sorted.end(),
            [](const MutantSpec *A, const MutantSpec *B) {
              return StringRef(A->Id).compare(StringRef(B->Id)) < 0;
            });
  for (const MutantSpec *M : Sorted) {
    const ManifestEntry &E = Manifest.at(M->Id);
    llvm::outs() << "  " << M->Id << " kind=" << M->Kind
                 << " rule_set=" << M->RuleSet << " config=" << E.Config
                 << " atom=" << E.AtomId << "\n";
  }
  llvm::outs() << "mutants killed: " << Killed << "/16\n";
}

//===----------------------------------------------------------------------===//
// T4: .td rule set to MIR map
//===----------------------------------------------------------------------===//
//
// The two fragments are authored by separate lanes and joined here: every
// table name is either exercised by a MIR file that runs the legalizer, or it
// carries a linked, approved exclusion.  A missing mapping, an atom that is
// not in the witness corpus, or an exclusion without a matching ledger entry
// is a hard failure that names the rule set.

struct MirMapEntry {
  std::string RuleSet;
  std::string Kind;
  std::string MirFile;
  std::vector<std::string> Functions;
  std::string Atom;
  std::vector<std::string> Opcodes;
  std::string ExceptionKind;
  std::string Reason;
  std::string LinkedLedger;
};

static bool readStringArray(const json::Object &O, StringRef Key,
                            std::vector<std::string> &Out, std::string &Error,
                            StringRef Name) {
  const json::Array *A = O.getArray(Key);
  if (!A) {
    Error = Name.str() + ": missing array '" + Key.str() + "'";
    return false;
  }
  for (const json::Value &V : *A) {
    auto S = V.getAsString();
    if (!S) {
      Error = Name.str() + ": '" + Key.str() + "' is not an array of strings";
      return false;
    }
    Out.push_back(S->str());
  }
  return true;
}

static std::optional<std::vector<MirMapEntry>>
readMirMapFragment(StringRef Name, StringRef ExpectedFamily,
                   std::string &Error) {
  std::optional<std::string> Text = readTestSourceFile(Name);
  if (!Text) {
    Error = "cannot read " + Name.str() + " from " +
            std::string(X86_LEGALIZER_TEST_SRC_DIR) +
            " (the fragment-producing lane has not delivered it yet)";
    return std::nullopt;
  }
  auto Value = json::parse(*Text);
  if (!Value) {
    Error = Name.str() + ": " + toString(Value.takeError());
    return std::nullopt;
  }
  const json::Object *Root = Value->getAsObject();
  if (!Root) {
    Error = Name.str() + ": top level is not an object";
    return std::nullopt;
  }
  auto Schema = Root->getString("schema");
  if (!Schema || *Schema != "llvm.gisel.x86-legalizer-mir-map") {
    Error = Name.str() + ": schema must be llvm.gisel.x86-legalizer-mir-map";
    return std::nullopt;
  }
  auto Version = Root->getInteger("version");
  if (!Version || *Version != 1) {
    Error = Name.str() + ": version must be 1";
    return std::nullopt;
  }
  auto Family = Root->getString("family");
  if (!Family || *Family != ExpectedFamily) {
    Error = Name.str() + ": family must be " + ExpectedFamily.str();
    return std::nullopt;
  }
  const json::Array *RuleSets = Root->getArray("rule_sets");
  if (!RuleSets) {
    Error = Name.str() + ": missing field 'rule_sets'";
    return std::nullopt;
  }

  std::vector<MirMapEntry> Out;
  for (const json::Value &V : *RuleSets) {
    const json::Object *O = V.getAsObject();
    if (!O) {
      Error = Name.str() + ": rule_sets entry is not an object";
      return std::nullopt;
    }
    auto RuleSet = O->getString("rule_set");
    const std::string EntryName =
        RuleSet ? RuleSet->str() : std::string("<unknown rule set>");
    if (!RuleSet || RuleSet->empty()) {
      Error = Name.str() + ": rule_sets entry has no 'rule_set'";
      return std::nullopt;
    }
    auto Kind = O->getString("kind");
    if (!Kind || Kind->empty()) {
      Error = EntryName + ": missing field 'kind'";
      return std::nullopt;
    }
    auto Atom = O->getString("atom");
    if (!Atom || Atom->empty()) {
      Error = EntryName + ": missing field 'atom'";
      return std::nullopt;
    }
    MirMapEntry E;
    E.RuleSet = RuleSet->str();
    E.Kind = Kind->str();
    E.Atom = Atom->str();
    if (E.Kind == "mir") {
      auto MirFile = O->getString("mir_file");
      if (!MirFile || MirFile->empty()) {
        Error = EntryName + ": mir entry has no 'mir_file'";
        return std::nullopt;
      }
      E.MirFile = MirFile->str();
      if (!readStringArray(*O, "functions", E.Functions, Error, EntryName) ||
          !readStringArray(*O, "opcodes", E.Opcodes, Error, EntryName))
        return std::nullopt;
    } else if (E.Kind == "exclusion") {
      auto ExceptionKind = O->getString("exception_kind");
      if (!ExceptionKind || ExceptionKind->empty()) {
        Error = EntryName + ": exclusion entry has no 'exception_kind'";
        return std::nullopt;
      }
      auto Reason = O->getString("reason");
      auto LinkedLedger = O->getString("linked_ledger");
      if (!Reason || Reason->empty()) {
        Error = EntryName + ": exclusion entry has no 'reason'";
        return std::nullopt;
      }
      if (!LinkedLedger || LinkedLedger->empty()) {
        Error = EntryName + ": exclusion entry has no 'linked_ledger'";
        return std::nullopt;
      }
      E.ExceptionKind = ExceptionKind->str();
      E.Reason = Reason->str();
      E.LinkedLedger = LinkedLedger->str();
    } else {
      Error = EntryName + ": unknown kind '" + E.Kind + "'";
      return std::nullopt;
    }
    Out.push_back(std::move(E));
  }
  return Out;
}

struct WitnessAtomRef {
  std::string RuleSet;
  std::optional<int64_t> RuleIndex;
};

static std::optional<std::map<std::string, WitnessAtomRef>>
readWitnessAtomIndex(std::string &Error) {
  std::optional<std::string> Text =
      readTestSourceFile("x86-legalizer-witness.json");
  if (!Text) {
    Error = "cannot read x86-legalizer-witness.json";
    return std::nullopt;
  }
  auto Value = json::parse(*Text);
  if (!Value) {
    Error = toString(Value.takeError());
    return std::nullopt;
  }
  const json::Object *Root = Value->getAsObject();
  if (!Root) {
    Error = "witness top level is not an object";
    return std::nullopt;
  }
  const json::Array *Atoms = Root->getArray("atoms");
  if (!Atoms) {
    Error = "witness has no 'atoms'";
    return std::nullopt;
  }
  std::map<std::string, WitnessAtomRef> Out;
  for (const json::Value &V : *Atoms) {
    const json::Object *O = V.getAsObject();
    if (!O) {
      Error = "witness atom is not an object";
      return std::nullopt;
    }
    auto Id = O->getString("id");
    auto RuleSet = O->getString("rule_set");
    if (!Id || !RuleSet) {
      Error = "witness atom has no 'id' or 'rule_set'";
      return std::nullopt;
    }
    WitnessAtomRef Ref;
    Ref.RuleSet = RuleSet->str();
    Ref.RuleIndex = O->getInteger("rule_index");
    if (!Out.emplace(Id->str(), Ref).second) {
      Error = "duplicate witness atom id " + Id->str();
      return std::nullopt;
    }
  }
  return Out;
}

static bool mirFileHasFunction(StringRef Text, StringRef Function) {
  SmallVector<StringRef, 0> Lines;
  Text.split(Lines, '\n');
  for (StringRef Line : Lines) {
    Line = Line.ltrim();
    if (!Line.consume_front("name:"))
      continue;
    Line = Line.ltrim();
    StringRef Name = Line.take_until([](char C) { return isSpace(C); });
    if (Name == Function)
      return true;
  }
  return false;
}

static bool mirFileRunsLegalizer(StringRef Text) {
  SmallVector<StringRef, 0> Lines;
  Text.split(Lines, '\n');
  for (StringRef Line : Lines)
    if (Line.contains("RUN:") && (Line.contains("-run-pass=legalizer") ||
                                  Line.contains("-stop-after=legalizer")))
      return true;
  return false;
}

TEST(X86LegalizerMirMap, MirRuleSetMapIsCompleteAndLinked) {
  std::string Error;
  std::optional<std::vector<MirMapEntry>> IntMap =
      readMirMapFragment("x86-legalizer-mir-map-01-int.json", "int", Error);
  ASSERT_TRUE(IntMap.has_value()) << Error;
  Error.clear();
  std::optional<std::vector<MirMapEntry>> MemMap =
      readMirMapFragment("x86-legalizer-mir-map-02-mem.json", "mem", Error);
  ASSERT_TRUE(MemMap.has_value()) << Error;

  // The union is exactly the 63 table names, each mapped once.
  std::set<std::string> Tables;
  for (unsigned I = 0; I != X86TestNumTables; ++I)
    Tables.insert(X86RuleIndexTableNames[I]);
  ASSERT_EQ(Tables.size(), 63u);

  std::map<std::string, const MirMapEntry *> BySet;
  for (const MirMapEntry &E : *IntMap)
    if (!BySet.emplace(E.RuleSet, &E).second)
      ADD_FAILURE() << "duplicate rule set in the int map: " << E.RuleSet;
  for (const MirMapEntry &E : *MemMap)
    if (!BySet.emplace(E.RuleSet, &E).second)
      ADD_FAILURE() << "duplicate rule set in the mem map: " << E.RuleSet;
  for (const auto &Entry : BySet)
    if (!Tables.count(Entry.first))
      ADD_FAILURE() << "map names an unknown table: " << Entry.first;
  for (const std::string &T : Tables)
    if (!BySet.count(T))
      ADD_FAILURE() << "map is missing rule set " << T;

  // Literal-bearing rulesets versus all-escaped ones come from the Core IR.
  std::optional<std::map<RuleKey, LiteralTarget>> LiteralTargets =
      readLiteralTargets(Error);
  ASSERT_TRUE(LiteralTargets.has_value()) << Error;
  std::set<std::string> LiteralSets;
  for (const auto &Entry : *LiteralTargets)
    LiteralSets.insert(Entry.second.Set);
  EXPECT_EQ(static_cast<int64_t>(LiteralSets.size()),
            manifestCount("literal_bearing_rule_set_count"));
  std::optional<std::vector<EscapeTarget>> EscapeTargets =
      readEscapeTargets(Error);
  ASSERT_TRUE(EscapeTargets.has_value()) << Error;
  std::set<std::string> EscapeSets;
  for (const EscapeTarget &E : *EscapeTargets)
    EscapeSets.insert(E.RuleSet);
  std::set<std::string> AllEscaped;
  for (const std::string &T : Tables)
    if (!LiteralSets.count(T))
      AllEscaped.insert(T);
  EXPECT_EQ(static_cast<int64_t>(AllEscaped.size()),
            manifestCount("all_escaped_rule_set_count"));
  std::set<std::string> EscapeOnly;
  for (const std::string &S : EscapeSets)
    if (!LiteralSets.count(S))
      EscapeOnly.insert(S);
  EXPECT_EQ(EscapeOnly, AllEscaped)
      << "all-escaped rule sets must be exactly the escape-only sets";
  std::set<std::string> Classified = LiteralSets;
  Classified.insert(EscapeSets.begin(), EscapeSets.end());
  EXPECT_EQ(Classified, Tables)
      << "every table must have a literal rule or an escaped rule";

  std::optional<std::map<std::string, WitnessAtomRef>> WitnessAtoms =
      readWitnessAtomIndex(Error);
  ASSERT_TRUE(WitnessAtoms.has_value()) << Error;
  std::optional<std::vector<LegalizerException>> Exceptions =
      readLegalizerExceptions(Error);
  ASSERT_TRUE(Exceptions.has_value()) << Error;
  std::map<std::tuple<std::string, std::string, unsigned>,
           const LegalizerException *>
      Approved;
  for (const LegalizerException &E : *Exceptions)
    Approved.emplace(std::make_tuple(E.RuleSet, E.Kind, E.RuleIndex), &E);

  std::set<std::string> Valid;
  for (const auto &E : BySet) {
    const MirMapEntry &M = *E.second;
    bool EntryOK = true;
    auto WIt = WitnessAtoms->find(M.Atom);
    if (WIt == WitnessAtoms->end()) {
      ADD_FAILURE() << M.RuleSet << ": atom " << M.Atom
                    << " is not in x86-legalizer-witness.json";
      EntryOK = false;
    } else if (WIt->second.RuleSet != M.RuleSet) {
      ADD_FAILURE() << M.RuleSet << ": atom " << M.Atom << " belongs to "
                    << WIt->second.RuleSet;
      EntryOK = false;
    }

    if (M.Kind == "mir") {
      const std::string Path =
          std::string(X86_LEGALIZER_TEST_TREE_DIR) + "/" + M.MirFile;
      auto Buffer = MemoryBuffer::getFile(Path);
      if (!Buffer) {
        ADD_FAILURE() << M.RuleSet << ": MIR file " << M.MirFile
                      << " does not exist under "
                      << X86_LEGALIZER_TEST_TREE_DIR;
        EntryOK = false;
      } else {
        const StringRef Text = (*Buffer)->getBuffer();
        if (M.Functions.empty()) {
          ADD_FAILURE() << M.RuleSet << ": mir entry lists no functions";
          EntryOK = false;
        }
        for (const std::string &Fn : M.Functions)
          if (!mirFileHasFunction(Text, Fn)) {
            ADD_FAILURE() << M.RuleSet << ": " << M.MirFile
                          << " has no function named " << Fn;
            EntryOK = false;
          }
        if (!mirFileRunsLegalizer(Text)) {
          ADD_FAILURE() << M.RuleSet << ": " << M.MirFile
                        << " has no legalizer RUN line";
          EntryOK = false;
        }
        bool OpcodeFound = false;
        for (const std::string &Opcode : M.Opcodes)
          if (!Opcode.empty() && Text.contains(Opcode)) {
            OpcodeFound = true;
            break;
          }
        if (!OpcodeFound) {
          ADD_FAILURE() << M.RuleSet
                        << ": none of the listed opcodes appears in "
                        << M.MirFile;
          EntryOK = false;
        }
      }
    } else if (M.Kind == "exclusion") {
      const bool IsLiteral = LiteralSets.count(M.RuleSet) != 0;
      const bool IsAllEscaped = AllEscaped.count(M.RuleSet) != 0;
      if (!IsLiteral && !IsAllEscaped) {
        ADD_FAILURE() << M.RuleSet
                      << " is neither literal-bearing nor all-escaped";
        EntryOK = false;
      }
      const std::string ExpectedKind =
          IsLiteral ? "mir-exclusion" : "nonliteral-shape";
      if (M.ExceptionKind != ExpectedKind) {
        ADD_FAILURE() << M.RuleSet << ": exclusion exception_kind must be "
                      << ExpectedKind << ", got " << M.ExceptionKind;
        EntryOK = false;
      }
      if (M.ExceptionKind != "mir-exclusion" &&
          M.ExceptionKind != "nonliteral-shape") {
        ADD_FAILURE() << M.RuleSet
                      << ": MirRuleSetMap does not accept exception kind '"
                      << M.ExceptionKind << "'";
        EntryOK = false;
      }
      if (WIt == WitnessAtoms->end() || !WIt->second.RuleIndex) {
        ADD_FAILURE() << M.RuleSet << ": exclusion atom " << M.Atom
                      << " has no rule_index to match the ledger";
        EntryOK = false;
      } else {
        const unsigned Index = static_cast<unsigned>(*WIt->second.RuleIndex);
        auto ApprovedIt =
            Approved.find(std::make_tuple(M.RuleSet, M.ExceptionKind, Index));
        if (ApprovedIt == Approved.end()) {
          ADD_FAILURE() << M.RuleSet << ": no approved " << M.ExceptionKind
                        << " exception for rule #" << Index << " (atom "
                        << M.Atom << ")";
          EntryOK = false;
        } else if (ApprovedIt->second->LinkedLedger.empty() ||
                   ApprovedIt->second->Proof.empty()) {
          ADD_FAILURE() << M.RuleSet << "#" << Index
                        << ": approved exclusion needs linked_ledger and proof";
          EntryOK = false;
        }
      }
    } else {
      ADD_FAILURE() << M.RuleSet << ": unknown map kind " << M.Kind;
      EntryOK = false;
    }

    if (EntryOK)
      Valid.insert(M.RuleSet);
  }

  for (const std::string &T : Tables)
    if (!Valid.count(T))
      ADD_FAILURE() << "rule set " << T
                    << " has neither a valid mir entry nor a valid approved "
                       "exclusion";
}

//===----------------------------------------------------------------------===//
// Fixed-seed differential fuzz
//===----------------------------------------------------------------------===//
//
// The exhaustive tests above enumerate a reviewed domain.  This tier adds a
// fixed-seed random walk over the same two tables: every query comes from a
// committed SplitMix64 seed, both the generated table and the reference answer
// it, and only a table decline (Escape/Outside/Untabulated) is skipped -- that
// is the documented fallback, not an answer to compare.  A mismatch is shrunk
// by replacing one type slot at a time with the class representatives, so the
// report names a smaller query that still reproduces.
//
// The tier table and the seeds live in x86-legalizer-fuzz-seeds.json.  This TU
// has no defaults: a missing or malformed artifact is a test failure, so every
// run is reproducible from the committed JSON alone.

// SplitMix64.  Small and stateless, so the sequence is a pure function of the
// seed; <random> is deliberately not used because its distributions are
// library-defined, and a digest over them would not be portable.
struct SplitMix64 {
  uint64_t State;

  explicit SplitMix64(uint64_t Seed) : State(Seed) {}

  uint64_t next() {
    uint64_t Z = (State += 0x9E3779B97F4A7C15ULL);
    Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBULL;
    return Z ^ (Z >> 31);
  }

  // Modulo bias is irrelevant here: the sequence is fixed and reviewed, not a
  // statistical claim about the generator.
  uint64_t bounded(uint64_t N) { return next() % N; }
};

static constexpr uint64_t Fnv1a64Offset = 0xCBF29CE484222325ULL;
static constexpr uint64_t Fnv1a64Prime = 0x100000001B3ULL;

static uint64_t fnv1a64Step(uint64_t Hash, uint64_t Value) {
  for (unsigned I = 0; I != 8; ++I) {
    Hash ^= (Value >> (8 * I)) & 0xFF;
    Hash *= Fnv1a64Prime;
  }
  return Hash;
}

// Fixed alignment vocabulary for the memory-descriptor axis.  It spans below
// every threshold, the thresholds themselves, and both sides of 32/64.
static constexpr uint64_t FuzzAlignVocabulary[] = {0,  1,  4,  8,   16, 31,
                                                   32, 64, 65, 128, 256};

struct FuzzSeeds {
  std::map<std::string, uint64_t> Tiers;
  std::map<std::string, uint64_t> Seeds;
};

static std::optional<uint64_t> parseFuzzNumber(StringRef S) {
  uint64_t Value = 0;
  if (S.consume_front("0x") || S.consume_front("0X")) {
    if (S.getAsInteger(16, Value))
      return std::nullopt;
    return Value;
  }
  if (S.getAsInteger(10, Value))
    return std::nullopt;
  return Value;
}

static std::optional<FuzzSeeds> loadFuzzSeeds(std::string &Err) {
  std::optional<std::string> Text =
      readTestSourceFile("x86-legalizer-fuzz-seeds.json");
  if (!Text) {
    Err = "cannot read x86-legalizer-fuzz-seeds.json from " +
          std::string(X86_LEGALIZER_TEST_SRC_DIR);
    return std::nullopt;
  }
  Expected<json::Value> Parsed = json::parse(*Text);
  if (!Parsed) {
    Err = "x86-legalizer-fuzz-seeds.json: " + toString(Parsed.takeError());
    return std::nullopt;
  }
  const json::Object *Root = Parsed->getAsObject();
  if (!Root) {
    Err = "x86-legalizer-fuzz-seeds.json: root is not an object";
    return std::nullopt;
  }
  std::optional<StringRef> Schema = Root->getString("schema");
  if (!Schema || *Schema != "llvm.gisel.x86-legalizer-fuzz-seeds") {
    Err = "x86-legalizer-fuzz-seeds.json: bad or missing schema";
    return std::nullopt;
  }
  std::optional<int64_t> Version = Root->getInteger("version");
  if (!Version || *Version != 1) {
    Err = "x86-legalizer-fuzz-seeds.json: bad or missing version";
    return std::nullopt;
  }
  std::optional<StringRef> Generator = Root->getString("generator");
  if (!Generator || *Generator != "splitmix64") {
    Err = "x86-legalizer-fuzz-seeds.json: bad or missing generator";
    return std::nullopt;
  }
  const json::Object *Tiers = Root->getObject("tiers");
  const json::Object *Seeds = Root->getObject("seeds");
  if (!Tiers || !Seeds) {
    Err = "x86-legalizer-fuzz-seeds.json: missing tiers or seeds";
    return std::nullopt;
  }
  FuzzSeeds Out;
  for (StringRef Tier : {"smoke", "nightly", "weekly", "long"}) {
    std::optional<int64_t> Queries = Tiers->getInteger(Tier);
    if (!Queries || *Queries <= 0) {
      Err = "x86-legalizer-fuzz-seeds.json: missing or invalid tier " +
            Tier.str();
      return std::nullopt;
    }
    std::optional<StringRef> SeedText = Seeds->getString(Tier);
    if (!SeedText) {
      Err = "x86-legalizer-fuzz-seeds.json: missing seed for " + Tier.str();
      return std::nullopt;
    }
    std::optional<uint64_t> Seed = parseFuzzNumber(*SeedText);
    if (!Seed) {
      Err = "x86-legalizer-fuzz-seeds.json: malformed seed for " + Tier.str();
      return std::nullopt;
    }
    Out.Tiers[Tier.str()] = static_cast<uint64_t>(*Queries);
    Out.Seeds[Tier.str()] = *Seed;
  }
  return Out;
}

struct FuzzRunConfig {
  std::string Tier;
  uint64_t Queries = 0;
  uint64_t Seed = 0;
  bool QueriesOverridden = false;
};

static std::optional<std::string> getFuzzEnv(const char *Name) {
  const char *Value = std::getenv(Name);
  if (!Value || !*Value)
    return std::nullopt;
  return std::string(Value);
}

// Resolve the tier, count and seed.  The JSON is authoritative; the three
// environment variables exist for local runs and are never a fallback for a
// missing artifact.
static std::optional<FuzzRunConfig> resolveFuzzRun(StringRef DefaultTier,
                                                   std::string &Err) {
  std::optional<FuzzSeeds> Seeds = loadFuzzSeeds(Err);
  if (!Seeds)
    return std::nullopt;
  FuzzRunConfig C;
  C.Tier = DefaultTier.str();
  if (std::optional<std::string> Env = getFuzzEnv("X86_LEGALIZER_FUZZ_TIER"))
    C.Tier = *Env;
  auto TierIt = Seeds->Tiers.find(C.Tier);
  auto SeedIt = Seeds->Seeds.find(C.Tier);
  if (TierIt == Seeds->Tiers.end() || SeedIt == Seeds->Seeds.end()) {
    Err = "unknown fuzz tier '" + C.Tier +
          "' (expected smoke, nightly, weekly or long)";
    return std::nullopt;
  }
  C.Queries = TierIt->second;
  C.Seed = SeedIt->second;
  if (std::optional<std::string> Env =
          getFuzzEnv("X86_LEGALIZER_FUZZ_QUERIES")) {
    std::optional<uint64_t> Value = parseFuzzNumber(*Env);
    if (!Value || *Value == 0) {
      Err = "malformed X86_LEGALIZER_FUZZ_QUERIES";
      return std::nullopt;
    }
    C.Queries = *Value;
    C.QueriesOverridden = true;
  }
  if (std::optional<std::string> Env = getFuzzEnv("X86_LEGALIZER_FUZZ_SEED")) {
    std::optional<uint64_t> Value = parseFuzzNumber(*Env);
    if (!Value) {
      Err = "malformed X86_LEGALIZER_FUZZ_SEED";
      return std::nullopt;
    }
    C.Seed = *Value;
  }
  return C;
}

// The fuzz type pool: the class representatives from X86TestTypeClasses that
// are not pointers, plus every LLT typeClasses() names that the class lattice
// does not (s7, s24, s200 -- they leave the class tuple and take the residual
// path).  That is 21 + 3 = 24 representatives.  Pointer and pointer-vector
// slots are deliberately absent: rules such as ScalarSameSizeAs/ClampScalar
// mutate the operand and LLT::changeElementSize asserts on pointers, so a
// pointer-typed slot is not a well-formed legalizer query for these rule sets.
static std::vector<LLT> fuzzTypeReps() {
  std::vector<LLT> Reps;
  auto Add = [&](LLT Ty) {
    if (std::find(Reps.begin(), Reps.end(), Ty) == Reps.end())
      Reps.push_back(Ty);
  };
  for (unsigned I = 0; I != X86TestNumTypeClasses; ++I) {
    LLT Ty = testTypeClassLLT(X86TestTypeClasses[I], /*PointerBits=*/64);
    if (!Ty.isPointer() && !Ty.isPointerVector())
      Add(Ty);
  }
  for (const TypeClass &T : typeClasses()) {
    LLT Ty = T.Dst;
    bool InLattice = false;
    for (unsigned I = 0; I != X86TestNumTypeClasses; ++I)
      if (testTypeClassLLT(X86TestTypeClasses[I], /*PointerBits=*/64) == Ty)
        InLattice = true;
    if (!InLattice)
      Add(Ty);
  }
  return Reps;
}

struct FuzzQuery {
  unsigned Table = 0;
  unsigned Config = 0;
  SmallVector<LLT, 2> Types;
  bool HasMem = false;
  LLT MemTy;
  uint64_t Align = 0;
};

// One fixture per run: four subtargets, four references and four tables.  The
// representatives and opcodes are precomputed so a large tier does not pay
// for them per query.
struct FuzzFixture {
  std::vector<Sigma> Sigmas;
  std::vector<std::unique_ptr<X86LegalizerInfoReference>> Oracles;
  std::vector<std::unique_ptr<TableLegalizerInfoForTest>> Tables;
  std::array<unsigned, X86TestNumTables> Opcodes{};
  std::vector<LLT> TypeReps;
  std::vector<LLT> MemReps32, MemReps64;

  const std::vector<LLT> &memReps(unsigned Config) const {
    return FullConfigs[Config].Is64 ? MemReps64 : MemReps32;
  }
};

static std::optional<FuzzFixture> makeFuzzFixture(std::string &Err) {
  FuzzFixture F;
  F.Sigmas.reserve(std::size(FullConfigs));
  F.Oracles.reserve(std::size(FullConfigs));
  F.Tables.reserve(std::size(FullConfigs));
  for (const Config &C : FullConfigs) {
    Sigma S = makeSigma(C);
    if (!S.STI) {
      Err = std::string("fuzz: no subtarget for config ") + C.ID;
      return std::nullopt;
    }
    F.Oracles.push_back(std::make_unique<X86LegalizerInfoReference>(
        *S.STI, *static_cast<const X86TargetMachine *>(S.TM.get())));
    F.Tables.push_back(std::make_unique<TableLegalizerInfoForTest>(*S.STI));
    F.Sigmas.push_back(std::move(S));
  }
  for (unsigned I = 0; I != X86TestNumTables; ++I) {
    std::optional<unsigned> Opcode = opcodeForName(X86TestTables[I].Opcode);
    if (!Opcode) {
      Err = std::string("fuzz: no TargetOpcode for ") + X86TestTables[I].Opcode;
      return std::nullopt;
    }
    F.Opcodes[I] = *Opcode;
  }
  F.TypeReps = fuzzTypeReps();
  F.MemReps32 = memClassReps(32);
  F.MemReps64 = memClassReps(64);
  return F;
}

// Draw order is part of the committed sequence: table, config, one type per
// slot, then the descriptor and its alignment.  Changing it changes every
// later query, so it is documented next to the generator.
static FuzzQuery makeFuzzQuery(SplitMix64 &Rng, const FuzzFixture &F) {
  FuzzQuery Q;
  Q.Table = static_cast<unsigned>(Rng.bounded(X86TestNumTables));
  Q.Config = static_cast<unsigned>(Rng.bounded(std::size(FullConfigs)));
  const X86TestTable &T = X86TestTables[Q.Table];
  for (unsigned I = 0; I != T.NumSlots; ++I)
    Q.Types.push_back(F.TypeReps[Rng.bounded(F.TypeReps.size())]);
  if (T.HasMem) {
    Q.HasMem = true;
    const std::vector<LLT> &MemTypes = F.memReps(Q.Config);
    Q.MemTy = MemTypes[Rng.bounded(MemTypes.size())];
    Q.Align = FuzzAlignVocabulary[Rng.bounded(std::size(FuzzAlignVocabulary))];
  }
  return Q;
}

struct FuzzAnswer {
  X86LegalizerTable::Result Table;
  LegalizeActionStep Reference{Unsupported, 0, LLT{}};
};

static FuzzAnswer answerFuzzQuery(FuzzFixture &F, const FuzzQuery &Q) {
  const X86TestTable &T = X86TestTables[Q.Table];
  LLT Types[2] = {LLT(), LLT()};
  for (unsigned I = 0; I != Q.Types.size(); ++I)
    Types[I] = Q.Types[I];
  LegalityQuery::MemDesc MemDescs[1] = {
      {Q.MemTy, Q.Align, AtomicOrdering::NotAtomic, AtomicOrdering::NotAtomic}};
  ArrayRef<LegalityQuery::MemDesc> MemRef;
  if (Q.HasMem)
    MemRef = MemDescs;
  LegalityQuery LQ(F.Opcodes[Q.Table], ArrayRef<LLT>(Types, T.NumSlots),
                   MemRef);
  FuzzAnswer A;
  A.Table = F.Tables[Q.Config]->table().query(LQ);
  A.Reference = F.Oracles[Q.Config]->getAction(LQ);
  return A;
}

static LegalizeActionStep fuzzTableStep(const X86LegalizerTable::Result &R) {
  return LegalizeActionStep(R.Action, R.TypeIdx, R.NewType);
}

// A decline is the documented fallback, not an answer: only Hit and Residual
// are compared, and the comparison is the same three-field predicate the
// exhaustive tests use.
static bool fuzzAnswersMismatch(const FuzzAnswer &A) {
  if (A.Table.Kind != X86LegalizerTable::Answer::Hit &&
      A.Table.Kind != X86LegalizerTable::Answer::Residual)
    return false;
  return !stepsAgree(fuzzTableStep(A.Table), A.Reference);
}

static std::string renderFuzzQuery(const FuzzQuery &Q) {
  std::string S;
  raw_string_ostream OS(S);
  OS << X86TestTables[Q.Table].Name << " (";
  for (unsigned I = 0; I != Q.Types.size(); ++I)
    OS << (I ? ", " : "") << witnessLLTName(Q.Types[I]);
  OS << ")";
  if (Q.HasMem)
    OS << " mem(" << witnessLLTName(Q.MemTy) << ", align " << Q.Align << ")";
  return OS.str();
}

static std::string fuzzTableText(const X86LegalizerTable::Result &R) {
  return std::string(answerName(R.Kind)) + "/" + describe(fuzzTableStep(R));
}

// Greedy one-pass shrink: for each slot in turn, keep the first class
// representative that still mismatches.  A candidate that turns the query into
// a decline does not count as reproducing the failure.
static std::string shrinkFuzzFailure(FuzzFixture &F, const FuzzQuery &Start,
                                     uint64_t Index, StringRef Tier,
                                     uint64_t Seed,
                                     const FuzzAnswer &StartAnswer) {
  FuzzQuery Cur = Start;
  FuzzAnswer CurAnswer = StartAnswer;
  for (unsigned Slot = 0; Slot != Start.Types.size(); ++Slot) {
    for (const LLT &Rep : F.TypeReps) {
      FuzzQuery Candidate = Cur;
      Candidate.Types[Slot] = Rep;
      FuzzAnswer A = answerFuzzQuery(F, Candidate);
      if (!fuzzAnswersMismatch(A))
        continue;
      Cur = Candidate;
      CurAnswer = A;
      break;
    }
  }
  std::string Msg;
  raw_string_ostream OS(Msg);
  OS << "FUZZ FAIL seed=0x" << utohexstr(Seed, /*LowerCase=*/true, 16)
     << " index=" << Index << " tier=" << Tier
     << " config=" << FullConfigs[Cur.Config].ID
     << " table=" << X86TestTables[Cur.Table].Name
     << " query=" << renderFuzzQuery(Start)
     << " table=" << fuzzTableText(CurAnswer.Table)
     << " reference=" << describe(CurAnswer.Reference)
     << " shrunk=" << renderFuzzQuery(Cur);
  return Msg;
}

struct FuzzRun {
  uint64_t Queries = 0;
  uint64_t Hits = 0;
  uint64_t Residuals = 0;
  uint64_t Escapes = 0;
  uint64_t Outsides = 0;
  uint64_t Untabulated = 0;
  uint64_t Diffs = 0;
  uint64_t Digest = Fnv1a64Offset;
  std::string Failure;
};

static uint64_t digestFuzzAnswer(uint64_t Hash,
                                 const X86LegalizerTable::Result &R) {
  Hash = fnv1a64Step(Hash, static_cast<uint64_t>(R.Kind));
  Hash = fnv1a64Step(Hash, static_cast<uint64_t>(R.Action));
  Hash = fnv1a64Step(Hash, R.TypeIdx);
  Hash = fnv1a64Step(Hash, R.NewType.getUniqueRAWLLTData());
  return Hash;
}

static FuzzRun runFuzzTier(StringRef Tier, uint64_t Seed, uint64_t Queries) {
  FuzzRun Run;
  std::string Err;
  std::optional<FuzzFixture> F = makeFuzzFixture(Err);
  if (!F) {
    Run.Failure = Err;
    return Run;
  }
  SplitMix64 Rng(Seed);
  for (uint64_t I = 0; I != Queries; ++I) {
    FuzzQuery Q = makeFuzzQuery(Rng, *F);
    FuzzAnswer A = answerFuzzQuery(*F, Q);
    ++Run.Queries;
    Run.Digest = digestFuzzAnswer(Run.Digest, A.Table);
    switch (A.Table.Kind) {
    case X86LegalizerTable::Answer::Hit:
      ++Run.Hits;
      break;
    case X86LegalizerTable::Answer::Residual:
      ++Run.Residuals;
      break;
    case X86LegalizerTable::Answer::Escape:
      ++Run.Escapes;
      break;
    case X86LegalizerTable::Answer::Outside:
      ++Run.Outsides;
      break;
    case X86LegalizerTable::Answer::Untabulated:
      ++Run.Untabulated;
      break;
    }
    if (!fuzzAnswersMismatch(A))
      continue;
    ++Run.Diffs;
    Run.Failure = shrinkFuzzFailure(*F, Q, I, Tier, Seed, A);
    break;
  }
  return Run;
}

static void printFuzzSummary(const FuzzRun &R, StringRef Tier, uint64_t Seed) {
  llvm::outs() << "fuzz tier=" << Tier << " queries=" << R.Queries
               << " hit=" << R.Hits << " residual=" << R.Residuals
               << " escape=" << R.Escapes << " diffs=" << R.Diffs << " seed=0x"
               << utohexstr(Seed, /*LowerCase=*/true, 16) << "\n";
  llvm::outs() << "fuzz tier=" << Tier << " digest=0x"
               << utohexstr(R.Digest, /*LowerCase=*/true, 16)
               << " outside=" << R.Outsides << " untabulated=" << R.Untabulated
               << "\n";
}

// A tier run must touch both compared paths; a local run with an explicit
// query override is allowed to be too small for that.  Answer::Escape is
// counted and skipped like Outside/Untabulated, but it is unreachable for a
// well-formed query here: every one of the 63 rule sets in X86TestTables has a
// generated evaluator, and X86EvalTable only returns nullopt (the opaque-cell
// signal) from its invalid-table default.  The per-kind counts are asserted to
// add up to the query count, so a decline can never be silently dropped.
static void checkFuzzCoverage(const FuzzRun &R, bool RequireCoverage) {
  if (!RequireCoverage)
    return;
  EXPECT_GT(R.Hits, 0u) << "fuzz tier never reached a table Hit";
  EXPECT_GT(R.Residuals, 0u) << "fuzz tier never reached the residual path";
}

TEST_F(X86LegalizerDifferentialTest, FuzzDifferentialSmoke) {
  std::string Err;
  std::optional<FuzzRunConfig> C = resolveFuzzRun("smoke", Err);
  ASSERT_TRUE(C.has_value()) << Err;

  const auto Started = std::chrono::steady_clock::now();
  FuzzRun First = runFuzzTier(C->Tier, C->Seed, C->Queries);
  // A second independent run over the same seed proves the generated sequence
  // and both answers are deterministic; the digest covers each query's answer
  // tuple in order.
  FuzzRun Second = runFuzzTier(C->Tier, C->Seed, C->Queries);
  const auto ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - Started)
                             .count();

  printFuzzSummary(First, C->Tier, C->Seed);
  llvm::outs() << "fuzz tier=" << C->Tier << " rerun_digest=0x"
               << utohexstr(Second.Digest, /*LowerCase=*/true, 16)
               << " deterministic="
               << (First.Digest == Second.Digest ? "yes" : "no")
               << " elapsed_ms=" << ElapsedMs << "\n";

  checkFuzzCoverage(First, !C->QueriesOverridden);
  EXPECT_EQ(First.Queries, First.Hits + First.Residuals + First.Escapes +
                               First.Outsides + First.Untabulated);
  EXPECT_EQ(First.Queries, C->Queries);
  EXPECT_EQ(Second.Queries, C->Queries);
  EXPECT_EQ(Second.Digest, First.Digest)
      << "the same seed produced a different digest";
  EXPECT_EQ(Second.Diffs, First.Diffs);
  if (!First.Failure.empty())
    ADD_FAILURE() << First.Failure;
  if (!Second.Failure.empty())
    ADD_FAILURE() << "second run: " << Second.Failure;
  EXPECT_EQ(First.Diffs, 0u);
}

// The large tiers are opt-in: gtest only runs this with
// --gtest_also_run_disabled_tests, and X86_LEGALIZER_FUZZ_TIER selects the
// tier.  It defaults to nightly so an accidental unfiltered disabled run does
// not start the long tier.
TEST_F(X86LegalizerDifferentialTest, DISABLED_FuzzDifferentialTier) {
  std::string Err;
  std::optional<FuzzRunConfig> C = resolveFuzzRun("nightly", Err);
  ASSERT_TRUE(C.has_value()) << Err;

  const auto Started = std::chrono::steady_clock::now();
  FuzzRun Run = runFuzzTier(C->Tier, C->Seed, C->Queries);
  const auto ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - Started)
                             .count();

  printFuzzSummary(Run, C->Tier, C->Seed);
  llvm::outs() << "fuzz tier=" << C->Tier << " elapsed_ms=" << ElapsedMs
               << "\n";

  checkFuzzCoverage(Run, !C->QueriesOverridden);
  EXPECT_EQ(Run.Queries, Run.Hits + Run.Residuals + Run.Escapes + Run.Outsides +
                             Run.Untabulated);
  EXPECT_EQ(Run.Queries, C->Queries);
  if (!Run.Failure.empty())
    ADD_FAILURE() << Run.Failure;
  EXPECT_EQ(Run.Diffs, 0u);
}

//===----------------------------------------------------------------------===//
// Structural and workload budgets
//===----------------------------------------------------------------------===//
//
// The generated .inc files and the witness corpus are reviewed artifacts, not
// hashes: this section bounds their growth and records what the CI path costs
// on the reference machine.  A budget is a decision threshold, so a failure
// names the artifact and both numbers rather than pointing at a diff.

static std::optional<uint64_t> getJSONUInt(const json::Object &O,
                                           StringRef Key) {
  if (std::optional<int64_t> V = O.getInteger(Key))
    if (*V >= 0)
      return static_cast<uint64_t>(*V);
  return std::nullopt;
}

static uint64_t growthCap(uint64_t Baseline, uint64_t Percent) {
  return (Baseline * (100 + Percent) + 99) / 100;
}

static std::string buildArtifactPath(StringRef Relative) {
  return std::string(X86_LEGALIZER_BUILD_DIR) + "/" + Relative.str();
}

static std::optional<uint64_t> countFileLines(StringRef Path,
                                              std::string &Error) {
  auto Buffer = MemoryBuffer::getFile(Path);
  if (!Buffer) {
    Error = "cannot read " + Path.str();
    return std::nullopt;
  }
  return (*Buffer)->getBuffer().count('\n');
}

// selfCheck() ends with a single summary line.  The counters are read from
// that line instead of from the reader's private state so the test observes
// the same artifact an operator sees in the log.
static std::optional<uint64_t> selfCheckCount(StringRef Log, StringRef Key) {
  const std::string Needle = (" " + Key + " ").str();
  const size_t Pos = Log.rfind(Needle);
  if (Pos == StringRef::npos)
    return std::nullopt;
  StringRef Rest = Log.substr(Pos + Needle.size());
  size_t Digits = 0;
  while (Digits != Rest.size() && Rest[Digits] >= '0' && Rest[Digits] <= '9')
    ++Digits;
  if (Digits == 0)
    return std::nullopt;
  uint64_t Value = 0;
  if (Rest.take_front(Digits).getAsInteger(10, Value))
    return std::nullopt;
  return Value;
}

TEST_F(X86LegalizerDifferentialTest, CodegenGrowthIsBounded) {
  std::optional<std::string> Text =
      readTestSourceFile("x86-legalizer-codegen-budget.json");
  ASSERT_TRUE(Text.has_value())
      << "cannot read x86-legalizer-codegen-budget.json";
  auto Parsed = json::parse(*Text);
  ASSERT_TRUE(!!Parsed) << toString(Parsed.takeError());
  const json::Object *Root = Parsed->getAsObject();
  ASSERT_TRUE(Root) << "codegen budget root is not an object";
  ASSERT_EQ(Root->getString("schema").value_or(""),
            "llvm.gisel.x86-legalizer-codegen-budget");

  std::optional<uint64_t> Percent = getJSONUInt(*Root, "max_growth_percent");
  ASSERT_TRUE(Percent.has_value()) << "missing max_growth_percent";
  const json::Object *Baseline = Root->getObject("baseline");
  ASSERT_TRUE(Baseline) << "missing baseline";

  struct Artifact {
    const char *Name;
    const char *Key;
    const char *Relative;
  };
  static const Artifact Artifacts[] = {
      {"X86LegalizerTable.inc", "table_inc_lines",
       "lib/Target/X86/X86LegalizerTable.inc"},
      {"X86LegalizerInfo.inc", "info_inc_lines",
       "lib/Target/X86/X86LegalizerInfo.inc"},
      {"X86LegalizerTablePointsForTest.inc", "points_inc_lines",
       "unittests/Target/X86/X86LegalizerTablePointsForTest.inc"},
      {"X86LegalizerRuleIndex.inc", "rule_index_inc_lines",
       "unittests/Target/X86/X86LegalizerRuleIndex.inc"},
  };
  for (const Artifact &A : Artifacts) {
    std::optional<uint64_t> Base = getJSONUInt(*Baseline, A.Key);
    ASSERT_TRUE(Base.has_value()) << "missing baseline " << A.Key;
    const uint64_t Cap = growthCap(*Base, *Percent);
    std::string Error;
    std::optional<uint64_t> Actual =
        countFileLines(buildArtifactPath(A.Relative), Error);
    ASSERT_TRUE(Actual.has_value()) << Error;
    EXPECT_LE(*Actual, Cap)
        << A.Name << ": " << *Actual << " lines exceeds cap " << Cap
        << " (baseline " << *Base << ", max_growth_percent " << *Percent << ")";
    llvm::outs() << "growth " << A.Name << " lines=" << *Actual
                 << " baseline=" << *Base << " cap=" << Cap << "\n";
  }

  // The recorded table/rule counts are the same denominators the coverage
  // manifest uses; a budget that drifted from it would bound a different
  // object than the tests walk.
  std::optional<std::string> CountsText =
      readTestSourceFile("x86-legalizer-rule-counts.json");
  ASSERT_TRUE(CountsText.has_value());
  auto CountsParsed = json::parse(*CountsText);
  ASSERT_TRUE(!!CountsParsed) << toString(CountsParsed.takeError());
  const json::Object *Counts = CountsParsed->getAsObject();
  ASSERT_TRUE(Counts);
  std::optional<uint64_t> ManifestTables = getJSONUInt(*Counts, "table_count");
  std::optional<uint64_t> ManifestRules = getJSONUInt(*Counts, "td_rule_count");
  std::optional<uint64_t> BaseTables = getJSONUInt(*Baseline, "tables");
  std::optional<uint64_t> BaseRules = getJSONUInt(*Baseline, "rules");
  ASSERT_TRUE(ManifestTables.has_value());
  ASSERT_TRUE(ManifestRules.has_value());
  ASSERT_TRUE(BaseTables.has_value());
  ASSERT_TRUE(BaseRules.has_value());
  EXPECT_EQ(*BaseTables, *ManifestTables)
      << "codegen budget tables do not match the coverage manifest";
  EXPECT_EQ(*BaseRules, *ManifestRules)
      << "codegen budget rules do not match the coverage manifest";

  // The rule index is generated from the same .td as the table, so it is an
  // independent read of the two counts the manifest and the budget record.
  uint64_t GeneratedRules = 0;
  for (unsigned NumRules : X86RuleIndexNumRules)
    GeneratedRules += NumRules;
  EXPECT_EQ(*BaseTables, std::size(X86RuleIndexNumRules))
      << "codegen budget tables do not match the generated rule index";
  EXPECT_EQ(*BaseRules, GeneratedRules)
      << "codegen budget rules do not match the generated rule index";

  // The domain size and the lazy-fill count are measured by the reader.  The
  // fill budget is a growth bound on the same workload, so it uses the same
  // percentage rather than a separate number.
  const Config &C = Configs[0];
  Sigma S = makeSigma(C);
  ASSERT_TRUE(S.STI) << C.ID;
  TableLegalizerInfoForTest Table(*S.STI);
  std::string Log;
  raw_string_ostream OS(Log);
  Table.mutableTable().selfCheck(OS);
  OS.flush();

  std::optional<uint64_t> Points = selfCheckCount(Log, "points");
  std::optional<uint64_t> Mismatches = selfCheckCount(Log, "mismatches");
  std::optional<uint64_t> Fills = selfCheckCount(Log, "fills");
  std::optional<uint64_t> BasePoints = getJSONUInt(*Baseline, "points");
  std::optional<uint64_t> BaseFills = getJSONUInt(*Baseline, "fills");
  ASSERT_TRUE(Points.has_value()) << Log;
  ASSERT_TRUE(Mismatches.has_value()) << Log;
  ASSERT_TRUE(Fills.has_value()) << Log;
  ASSERT_TRUE(BasePoints.has_value()) << "missing baseline points";
  ASSERT_TRUE(BaseFills.has_value()) << "missing baseline fills";
  EXPECT_EQ(*Mismatches, 0u) << Log;
  EXPECT_EQ(*Points, *BasePoints)
      << "codegen budget points do not match selfCheck";
  const uint64_t FillsCap = growthCap(*BaseFills, *Percent);
  EXPECT_LE(*Fills, FillsCap)
      << "fills: " << *Fills << " exceeds cap " << FillsCap << " (baseline "
      << *BaseFills << ", max_growth_percent " << *Percent << ")";
  llvm::outs() << "growth fills=" << *Fills << " baseline=" << *BaseFills
               << " cap=" << FillsCap << "\n";
}

// AC10: the table must answer every committed witness query itself.  Direct
// query() answers are classified first, then each atom is replayed through
// getAction() with a counting decorator installed.  A decline is allowed only
// when the table's own answer was Escape, Outside or Untabulated -- the
// documented fallback to the rule chain.
TEST_F(X86LegalizerDifferentialTest, ReleasePathNoApply) {
  std::string Error;
  std::optional<std::vector<WitnessAtom>> Atoms = readWitnessAtoms(Error);
  ASSERT_TRUE(Atoms.has_value()) << Error;
  ASSERT_EQ(Atoms->size(), 1256u);

  std::map<std::string, unsigned> ConfigForID;
  for (unsigned I = 0; I != std::size(FullConfigs); ++I)
    ConfigForID.emplace(FullConfigs[I].ID, I);

  std::vector<Sigma> Sigmas;
  Sigmas.reserve(std::size(FullConfigs));
  for (const Config &C : FullConfigs) {
    Sigmas.push_back(makeSigma(C));
    ASSERT_TRUE(Sigmas.back().STI) << C.ID;
  }
  std::vector<std::unique_ptr<TableLegalizerInfoForTest>> Tables;
  Tables.reserve(Sigmas.size());
  for (const Sigma &S : Sigmas)
    Tables.push_back(std::make_unique<TableLegalizerInfoForTest>(*S.STI));

  uint64_t Hits = 0, Residuals = 0, ExpectedFalls = 0;
  std::set<unsigned> SeenConfigs;
  for (const WitnessAtom &A : *Atoms) {
    auto It = ConfigForID.find(A.ConfigID);
    ASSERT_NE(It, ConfigForID.end()) << A.Id;
    const unsigned CI = It->second;
    SeenConfigs.insert(CI);

    X86LegalizerTable::Result Direct;
    withAtomQuery(A.Point, [&](const LegalityQuery &Q) {
      Direct = Tables[CI]->table().query(Q);
    });
    switch (Direct.Kind) {
    case X86LegalizerTable::Answer::Hit:
      ++Hits;
      break;
    case X86LegalizerTable::Answer::Residual:
      ++Residuals;
      break;
    case X86LegalizerTable::Answer::Escape:
    case X86LegalizerTable::Answer::Outside:
    case X86LegalizerTable::Answer::Untabulated:
      ++ExpectedFalls;
      break;
    }

    withAtomQuery(A.Point, [&](const LegalityQuery &Q) {
      (void)Tables[CI]->getAction(Q);
    });
  }

  uint64_t Lookups = 0, FallThroughs = 0;
  for (const auto &T : Tables) {
    Lookups += T->lookups();
    FallThroughs += T->fallThroughs();
  }
  llvm::outs() << "release-path: atoms=" << Atoms->size() << " hit=" << Hits
               << " residual=" << Residuals << " fallthroughs=" << FallThroughs
               << "\n";
  EXPECT_EQ(SeenConfigs.size(), std::size(FullConfigs))
      << "witness corpus does not cover every full configuration";
  EXPECT_EQ(Lookups, Atoms->size());
  EXPECT_EQ(FallThroughs, ExpectedFalls);
  EXPECT_EQ(ExpectedFalls, 0u)
      << "the committed corpus must stay on the table path";
}

TEST_F(X86LegalizerDifferentialTest, TableFillsAreBoundedAndStable) {
  const Config &C = Configs[0];
  Sigma S = makeSigma(C);
  ASSERT_TRUE(S.STI) << C.ID;
  TableLegalizerInfoForTest Table(*S.STI);

  auto RunSelfCheck = [&](std::string &Log) {
    raw_string_ostream OS(Log);
    Table.mutableTable().selfCheck(OS);
    OS.flush();
  };

  std::string FirstLog, SecondLog;
  RunSelfCheck(FirstLog);
  RunSelfCheck(SecondLog);

  std::optional<uint64_t> FirstPoints = selfCheckCount(FirstLog, "points");
  std::optional<uint64_t> FirstMismatches =
      selfCheckCount(FirstLog, "mismatches");
  std::optional<uint64_t> FirstFills = selfCheckCount(FirstLog, "fills");
  std::optional<uint64_t> SecondMismatches =
      selfCheckCount(SecondLog, "mismatches");
  std::optional<uint64_t> SecondFills = selfCheckCount(SecondLog, "fills");
  ASSERT_TRUE(FirstPoints.has_value()) << FirstLog;
  ASSERT_TRUE(FirstMismatches.has_value()) << FirstLog;
  ASSERT_TRUE(FirstFills.has_value()) << FirstLog;
  ASSERT_TRUE(SecondMismatches.has_value()) << SecondLog;
  ASSERT_TRUE(SecondFills.has_value()) << SecondLog;

  EXPECT_EQ(*FirstMismatches, 0u) << FirstLog;
  EXPECT_EQ(*SecondMismatches, 0u) << SecondLog;
  EXPECT_GT(*FirstFills, 0u) << FirstLog;
  EXPECT_LE(*FirstFills, *FirstPoints) << FirstLog;
  EXPECT_EQ(*SecondFills, *FirstFills)
      << "a second selfCheck() refilled cells: " << SecondLog;
  llvm::outs() << "fills=" << *FirstFills << " points=" << *FirstPoints
               << " stable=yes\n";
}

//===----------------------------------------------------------------------===//
// Two-layer corpus: generated full artifact and reviewed deep panel
//===----------------------------------------------------------------------===//
//
// The witness corpus is the default-CI baseline.  The full corpus is a nightly
// artifact generated outside the source tree; the deep panel is a small,
// reviewed source artifact that widens the boundary axis without making the
// committed corpus large.

// FNV-1a over the bytes of a string.  fnv1a64Step() hashes an integer's eight
// bytes; the artifact hashes must be a function of the emitted text and must
// not depend on host endianness.
static uint64_t fnv1a64Text(uint64_t Hash, StringRef Text) {
  for (unsigned char C : Text) {
    Hash ^= C;
    Hash *= Fnv1a64Prime;
  }
  return Hash;
}

// The working tree may be dirty, but the commit is still useful provenance.
// `git` is optional: a build without it omits the field, as the schema allows.
static std::optional<std::string> readGitHeadCommit() {
  auto Git = sys::findProgramByName("git");
  if (!Git)
    return std::nullopt;
  SmallVector<char, 128> TempPath;
  if (sys::fs::createTemporaryFile("x86-legalizer-head", "txt", TempPath))
    return std::nullopt;
  const StringRef TempRef(TempPath.data(), TempPath.size());
  SmallVector<StringRef, 6> Args{*Git, "-C", X86_LEGALIZER_TEST_SRC_DIR,
                                 "rev-parse", "HEAD"};
  std::optional<StringRef> Redirects[3] = {std::nullopt, TempRef, StringRef()};
  const int RC = sys::ExecuteAndWait(*Git, Args, std::nullopt, Redirects);
  std::optional<std::string> Result;
  if (RC == 0) {
    auto Buffer = MemoryBuffer::getFile(TempRef);
    if (Buffer) {
      StringRef Text = (*Buffer)->getBuffer().trim();
      if (!Text.empty())
        Result = Text.str();
    }
  }
  sys::fs::remove(TempRef);
  return Result;
}

static StringRef domainOriginName(DomainKind Kind) {
  switch (Kind) {
  case DomainKind::ClassPair:
    return "classpair";
  case DomainKind::Boundary:
    return "boundary";
  case DomainKind::AScalar:
    return "as-scalar";
  case DomainKind::ASVector:
    return "as-vector";
  }
  llvm_unreachable("bad domain kind");
}

// Failure-message query text shared by the full and deep corpora.
static std::string formatQueryText(StringRef RuleSet, ArrayRef<LLT> Types,
                                   ArrayRef<LegalityQuery::MemDesc> MemDescs) {
  std::string Text;
  raw_string_ostream OS(Text);
  OS << RuleSet << " (";
  for (unsigned I = 0; I != Types.size(); ++I)
    OS << (I ? ", " : "") << witnessLLTName(Types[I]);
  OS << ")";
  if (!MemDescs.empty())
    OS << " mem(" << witnessLLTName(MemDescs[0].MemoryTy) << ", align "
       << MemDescs[0].AlignInBits << ")";
  return OS.str();
}

// The 60-atom deep panel.  It starts from the reviewed 12-atom panel and adds
// scalars the X86TypeClasses vocabulary does not name (s2, s5, s512, ...) and
// vector lane counts / element widths outside the class grid (v8s8, v5s32,
// v2s128, ...).  The result is sorted by its canonical spelling so the
// committed artifact is reproducible without relying on hash iteration order.
static std::vector<LLT> deepPanelAtoms() {
  std::vector<LLT> Panel(boundaryPanel().begin(), boundaryPanel().end());
  static const unsigned ExtraScalars[] = {
      2,  4,  5,  6,  9,   12,  15,  17,  20,  23,  31,  40,
      48, 63, 72, 96, 127, 160, 192, 224, 255, 384, 512, 1024};
  for (unsigned Bits : ExtraScalars)
    Panel.push_back(LLT::scalar(Bits));
  static const std::pair<unsigned, unsigned> ExtraVectors[] = {
      {2, 8},  {3, 8},   {8, 8},   {2, 16},  {3, 16},  {4, 16},
      {6, 16}, {7, 16},  {12, 16}, {64, 16}, {5, 32},  {6, 32},
      {7, 32}, {12, 32}, {32, 32}, {3, 64},  {5, 64},  {6, 64},
      {7, 64}, {16, 64}, {32, 64}, {2, 128}, {3, 128}, {4, 128}};
  for (const auto &V : ExtraVectors)
    Panel.push_back(LLT::fixed_vector(V.first, V.second));
  std::sort(Panel.begin(), Panel.end(),
            [](LLT L, LLT R) { return witnessLLTName(L) < witnessLLTName(R); });
  return Panel;
}

static std::optional<std::vector<LLT>> readDeepPanel(std::string &Error) {
  std::optional<std::string> Text =
      readTestSourceFile("x86-legalizer-deep-panel.json");
  if (!Text) {
    Error = "cannot read x86-legalizer-deep-panel.json from " +
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
    Error = "deep panel root is not an object";
    return std::nullopt;
  }
  if (Root->getString("schema").value_or("") !=
      "llvm.gisel.x86-legalizer-deep-panel") {
    Error = "deep panel has the wrong schema";
    return std::nullopt;
  }
  if (Root->getInteger("atom_count").value_or(-1) != 60) {
    Error = "deep panel must record atom_count 60";
    return std::nullopt;
  }
  if (Root->getString("reviewed_by").value_or("").empty()) {
    Error = "deep panel must record reviewed_by";
    return std::nullopt;
  }
  const json::Array *AtomArray = Root->getArray("atoms");
  if (!AtomArray) {
    Error = "deep panel has no atoms array";
    return std::nullopt;
  }
  std::vector<LLT> Panel;
  std::set<std::string> Names;
  std::string ListText;
  Panel.reserve(AtomArray->size());
  for (const json::Value &AtomValue : *AtomArray) {
    auto Name = AtomValue.getAsString();
    if (!Name) {
      Error = "deep panel atom is not a string";
      return std::nullopt;
    }
    auto Ty = parseWitnessLLT(*Name);
    if (!Ty) {
      Error = "bad deep panel LLT " + Name->str();
      return std::nullopt;
    }
    if (!Names.insert(Name->str()).second) {
      Error = "duplicate deep panel LLT " + Name->str();
      return std::nullopt;
    }
    ListText += Name->str();
    ListText += '\n';
    Panel.push_back(*Ty);
  }
  if (Panel.size() != 60) {
    Error = "deep panel must have 60 atoms";
    return std::nullopt;
  }
  if (Root->getString("sha256").value_or("") != sha256Hex(ListText)) {
    Error = "deep panel sha256 does not match the atom list";
    return std::nullopt;
  }
  const std::string GeneratorHash =
      utohexstr(fnv1a64Text(Fnv1a64Offset, ListText), /*LowerCase=*/true, 16);
  if (Root->getString("generator_hash").value_or("") != GeneratorHash) {
    Error = "deep panel generator_hash does not match the atom list";
    return std::nullopt;
  }
  return Panel;
}

// (rule_set, rule_index) -> the single slot the rule mutates.  Only rules
// whose mutation is applied to a pointer are trimmed; everything else stays
// in the corpus.
using MutationTargetMap = std::map<std::pair<std::string, unsigned>, unsigned>;

static std::optional<MutationTargetMap>
readMutationTargets(std::string &Error) {
  auto Buffer = MemoryBuffer::getFile(X86_LEGALIZER_CORE_JSON);
  if (!Buffer) {
    Error = std::string("cannot read ") + X86_LEGALIZER_CORE_JSON;
    return std::nullopt;
  }
  auto Value = json::parse((*Buffer)->getBuffer());
  if (!Value) {
    Error = toString(Value.takeError());
    return std::nullopt;
  }
  const json::Object *Root = Value->getAsObject();
  if (!Root) {
    Error = "core.json top level is not an object";
    return std::nullopt;
  }
  const json::Array *Rulesets = Root->getArray("rulesets");
  if (!Rulesets) {
    Error = "core.json has no 'rulesets'";
    return std::nullopt;
  }

  MutationTargetMap Targets;
  for (const json::Value &RulesetValue : *Rulesets) {
    const json::Object *Ruleset = RulesetValue.getAsObject();
    if (!Ruleset) {
      Error = "core.json ruleset is not an object";
      return std::nullopt;
    }
    auto Def = Ruleset->getString("def");
    const json::Array *Rules = Ruleset->getArray("rules");
    if (!Def || !Rules) {
      Error = "core.json ruleset has no def/rules";
      return std::nullopt;
    }
    for (const json::Value &RuleValue : *Rules) {
      const json::Object *Rule = RuleValue.getAsObject();
      if (!Rule)
        continue;
      const json::Object *Mutation = Rule->getObject("mutation");
      if (!Mutation)
        continue;
      if (Mutation->get("targets")) {
        Error = "core.json rule " + Def->str() +
                " carries multiple mutation targets";
        return std::nullopt;
      }
      auto Target = Mutation->getInteger("target");
      auto RuleIndex = Rule->getInteger("rule_index");
      if (!Target || !RuleIndex || *Target < 0 || *RuleIndex < 0) {
        Error = "core.json rule " + Def->str() +
                " has an invalid mutation target or rule_index";
        return std::nullopt;
      }
      const auto Key =
          std::make_pair(Def->str(), static_cast<unsigned>(*RuleIndex));
      const unsigned Slot = static_cast<unsigned>(*Target);
      auto It = Targets.find(Key);
      if (It != Targets.end() && It->second != Slot) {
        Error = "core.json rule " + Def->str() + "#" +
                std::to_string(*RuleIndex) + " maps to two mutation targets";
        return std::nullopt;
      }
      Targets[Key] = Slot;
    }
  }
  return Targets;
}

// Canonical generation parameters shared by the generator, the artifact
// checker, and the committed manifest.  The digest covers these values plus
// the exact atom text; provenance (generator_base_commit) is excluded so a
// dirty worktree does not change it.
static std::string fullConfigsJson() {
  std::string Text;
  raw_string_ostream OS(Text);
  OS << "[";
  for (unsigned I = 0; I != std::size(FullConfigs); ++I)
    OS << (I ? ", " : "") << "\"" << FullConfigs[I].ID << "\"";
  OS << "]";
  return OS.str();
}

static constexpr StringLiteral FullSamplingPolicy =
    "class-pair + 12-atom boundary panel + scalar/pointer-vector AS";
static constexpr StringLiteral FullDomainFormula = "4*(182712+23388+436)";
static constexpr StringLiteral TrimReasonText =
    "the first matching rule applies WidenScalar or NarrowScalar to a pointer "
    "slot; LLT::changeElementSize rejects pointer and pointer-vector types, "
    "so the frozen reference cannot answer this query";

struct CorpusDigestInput {
  std::string GeneratorHash;
  uint64_t DomainAtomsRaw = 0;
  uint64_t TrimmedUnevaluable = 0;
  std::string TrimReason;
  std::string TrimExample;
};

static std::string corpusDigestText(const CorpusDigestInput &I) {
  std::string Text;
  raw_string_ostream OS(Text);
  OS << "generator_hash " << I.GeneratorHash << "\n"
     << "domain_atoms_raw " << I.DomainAtomsRaw << "\n"
     << "trimmed_unevaluable " << I.TrimmedUnevaluable << "\n"
     << "trim_reason " << I.TrimReason << "\n"
     << "trim_example " << I.TrimExample << "\n"
     << "configs " << fullConfigsJson() << "\n"
     << "sampling_policy " << FullSamplingPolicy << "\n"
     << "domain_formula " << FullDomainFormula << "\n";
  return OS.str();
}

static std::string corpusDigest(const CorpusDigestInput &I,
                                StringRef AtomsText) {
  SHA256 Hasher;
  Hasher.update(corpusDigestText(I));
  Hasher.update(AtomsText);
  return toHex(Hasher.final(), /*LowerCase=*/true);
}

// Parse the committed artifact manifest.  It is a flat `key value` record; the
// differential cross-checks it against the artifact, and the default suite
// checks its internal invariants.
static std::optional<std::map<std::string, std::string>>
readFullCorpusManifest(std::string &Error) {
  std::optional<std::string> Text =
      readTestSourceFile("x86-legalizer-full.sha256");
  if (!Text) {
    Error = "cannot read x86-legalizer-full.sha256 from " +
            std::string(X86_LEGALIZER_TEST_SRC_DIR);
    return std::nullopt;
  }
  std::map<std::string, std::string> Manifest;
  SmallVector<StringRef, 16> Lines;
  StringRef(*Text).split(Lines, '\n');
  for (StringRef Line : Lines) {
    Line = Line.trim();
    if (Line.empty())
      continue;
    auto Split = Line.split(' ');
    if (Split.second.empty()) {
      Error = "manifest line has no value: " + Line.str();
      return std::nullopt;
    }
    Manifest[Split.first.str()] = Split.second.trim().str();
  }
  return Manifest;
}

// The first-match rule is found with mutations off, so the probe itself cannot
// assert on a pointer.  Only the rule identity, its declared target slot, and
// whether its action changes element size are used; the expected decision is
// never computed for a trimmed point.
struct FirstMatchMutation {
  unsigned RuleIndex = 0;
  unsigned TargetSlot = 0;
  bool TargetOutOfRange = false;
  bool TargetIsPointer = false;
  bool ActionChangesElementSize = false;
};

static FirstMatchMutation firstMatchMutation(unsigned Table,
                                             const X86Subtarget &STI,
                                             const LegalityQuery &Q,
                                             const MutationTargetMap &Targets) {
  FirstMatchMutation M;
  unsigned RuleIndex = 0;
  LegalizeActionStep Step(Unsupported, 0, LLT{});
  {
    RuleIndexMutationsOff Guard(X86EvalRuleIndexMutations);
    if (!X86EvalRuleIndex(Table, STI, Q, RuleIndex, Step))
      return M;
  }
  M.RuleIndex = RuleIndex;
  M.ActionChangesElementSize = actionChangesElementSize(Step.Action);
  auto It = Targets.find({X86TestTables[Table].Name, RuleIndex});
  if (It == Targets.end())
    return M;
  M.TargetSlot = It->second;
  if (M.TargetSlot >= Q.Types.size()) {
    M.TargetOutOfRange = true;
    return M;
  }
  const LLT TargetTy = Q.Types[M.TargetSlot];
  M.TargetIsPointer = TargetTy.isPointer() || TargetTy.isPointerVector();
  return M;
}

// Not a test.  Generate the nightly artifact outside the source tree:
//
//   X86_LEGALIZER_FULL_OUT=<abs path> X86Tests \
//     --gtest_also_run_disabled_tests --gtest_filter='*GenerateFullCorpus*'
//
// The artifact is never committed.  Its sha256 and generation parameters are
// recorded in x86-legalizer-full.sha256.
TEST_F(X86LegalizerDifferentialTest, DISABLED_GenerateFullCorpus) {
  const char *OutPath = std::getenv("X86_LEGALIZER_FULL_OUT");
  if (!OutPath)
    GTEST_SKIP() << "set X86_LEGALIZER_FULL_OUT to the absolute output path";

  const auto Started = std::chrono::steady_clock::now();

  std::vector<Sigma> Sigmas;
  Sigmas.reserve(std::size(FullConfigs));
  std::vector<std::unique_ptr<X86LegalizerInfoReference>> Oracles;
  Oracles.reserve(std::size(FullConfigs));
  for (const Config &C : FullConfigs) {
    Sigmas.push_back(makeSigma(C));
    ASSERT_TRUE(Sigmas.back().STI) << C.ID;
    Oracles.push_back(std::make_unique<X86LegalizerInfoReference>(
        *Sigmas.back().STI,
        *static_cast<const X86TargetMachine *>(Sigmas.back().TM.get())));
  }

  std::string Error;
  std::optional<MutationTargetMap> Targets = readMutationTargets(Error);
  ASSERT_TRUE(Targets.has_value()) << Error;
  ASSERT_EQ(static_cast<int64_t>(Targets->size()),
            manifestCount("mutation_target_count"))
      << "core.json mutation map must match the rule-count manifest";

  std::string AtomsText;
  AtomsText.reserve(192 * 1024 * 1024);
  uint64_t RawAtomCount = 0;
  uint64_t AtomCount = 0;
  uint64_t TrimmedUnevaluable = 0;
  std::string TrimExample;
  uint64_t GeneratorHash = Fnv1a64Offset;
  for (unsigned CI = 0; CI != std::size(FullConfigs); ++CI) {
    const unsigned PointerBits = FullConfigs[CI].Is64 ? 64 : 32;
    for (unsigned TI = 0; TI != X86TestNumTables; ++TI) {
      const X86TestTable &T = X86TestTables[TI];
      const std::optional<unsigned> Opcode = opcodeForName(T.Opcode);
      ASSERT_TRUE(Opcode.has_value()) << T.Opcode;
      uint64_t Serial = 0;
      auto Emit = [&](DomainKind Kind, ArrayRef<LLT> Types, bool HasMem,
                      LLT MemTy, uint64_t Align) {
        X86TablePoint P;
        ASSERT_LE(Types.size(), std::size(P.Types));
        P.Opcode = *Opcode;
        P.Name = T.Opcode;
        P.NumTypes = static_cast<uint8_t>(Types.size());
        std::copy(Types.begin(), Types.end(), P.Types);
        P.MemTy = MemTy;
        P.Align = Align;
        P.HasMem = HasMem;
        FirstMatchMutation Mutation;
        withAtomQuery(P, [&](const LegalityQuery &Q) {
          Mutation = firstMatchMutation(TI, *Sigmas[CI].STI, Q, *Targets);
        });
        ++RawAtomCount;
        if (Mutation.TargetOutOfRange) {
          ADD_FAILURE() << "core.json mutation target " << Mutation.TargetSlot
                        << " is outside the query arity for "
                        << FullConfigs[CI].ID << "/" << T.Name << " rule #"
                        << Mutation.RuleIndex << " " << witnessQueryText(P);
          return;
        }
        if (Mutation.TargetIsPointer && Mutation.ActionChangesElementSize) {
          ++TrimmedUnevaluable;
          if (TrimExample.empty())
            TrimExample = std::string(FullConfigs[CI].ID) + "/" + T.Name + "/" +
                          witnessQueryText(P);
          return;
        }
        LegalizeActionStep Step(Unsupported, 0, LLT{});
        withAtomQuery(P, [&](const LegalityQuery &Q) {
          Step = Oracles[CI]->getAction(Q);
        });

        const std::string Id = "full/" + std::string(FullConfigs[CI].ID) + "/" +
                               T.Name + "#" + std::to_string(Serial);
        ++Serial;
        GeneratorHash = fnv1a64Text(GeneratorHash, Id);
        GeneratorHash = fnv1a64Text(GeneratorHash, "\n");

        std::string Atom;
        raw_string_ostream OS(Atom);
        OS << "    {\"id\":\"" << Id
           << "\",\"origin\":\"full:" << domainOriginName(Kind)
           << "\",\"rule_set\":\"" << T.Name << "\",\"config\":\""
           << FullConfigs[CI].ID << "\",\"query\":{\"types\":[";
        for (unsigned I = 0; I != P.NumTypes; ++I)
          OS << (I ? "," : "") << "\"" << witnessLLTName(P.Types[I]) << "\"";
        OS << "]";
        if (P.HasMem)
          OS << ",\"mem\":{\"type\":\"" << witnessLLTName(P.MemTy)
             << "\",\"align\":" << P.Align << "}";
        OS << "},\"expected\":";
        renderStepJson(OS, Step);
        OS << "}";
        OS.flush();
        if (AtomCount != 0)
          AtomsText += ",\n";
        AtomsText += Atom;
        ++AtomCount;
      };
      enumerateTableDomain(TI, PointerBits, Emit);
    }
  }

  ASSERT_EQ(RawAtomCount, 826144u)
      << "the raw full denominator is a hard contract, not a measurement";
  ASSERT_EQ(AtomCount + TrimmedUnevaluable, RawAtomCount)
      << "every raw atom must be either emitted or counted as trimmed";
  const std::string GeneratorHashText =
      utohexstr(GeneratorHash, /*LowerCase=*/true, 16);

  std::string TrimLogJson;
  {
    raw_string_ostream T(TrimLogJson);
    if (TrimmedUnevaluable != 0)
      T << "{\"reason\":\"" << TrimReasonText
        << "\",\"example\":\"" << TrimExample
        << "\",\"count\":" << TrimmedUnevaluable << "}";
    T.flush();
  }

  // The integrity digest covers the generation parameters and the atom text
  // only.  Provenance (generator_base_commit) is excluded so the digest is
  // reproducible before the change set is committed.
  CorpusDigestInput DigestInput;
  DigestInput.GeneratorHash = GeneratorHashText;
  DigestInput.DomainAtomsRaw = RawAtomCount;
  DigestInput.TrimmedUnevaluable = TrimmedUnevaluable;
  DigestInput.TrimReason = TrimmedUnevaluable != 0 ? TrimReasonText.str() : "";
  DigestInput.TrimExample = TrimExample;
  const std::string Sha = corpusDigest(DigestInput, AtomsText);

  static constexpr StringLiteral AtomsPrefix = "  \"atoms\": [\n";
  static constexpr StringLiteral AtomsSuffix = "\n  ]\n}\n";

  std::string Header;
  raw_string_ostream H(Header);
  H << "{\n";
  H << "  \"schema\": \"llvm.gisel.x86-legalizer-full\",\n";
  H << "  \"version\": 1,\n";
  H << "  \"comment\": \"Generated by DISABLED_GenerateFullCorpus from the "
       "frozen reference.  Nightly artifact; never commit it.\",\n";
  if (std::optional<std::string> Commit = readGitHeadCommit()) {
    H << "  \"generator_base_commit\": \"" << *Commit << "\",\n";
    H << "  \"generator_base_commit_note\": \"HEAD the working tree was "
         "based on; uncommitted changes are not represented\",\n";
  }
  H << "  \"domain_atoms_raw\": " << RawAtomCount << ",\n";
  H << "  \"atom_count\": " << AtomCount << ",\n";
  H << "  \"trimmed_unevaluable\": " << TrimmedUnevaluable << ",\n";
  H << "  \"trim_log\": [" << TrimLogJson << "],\n";
  H << "  \"configs\": " << fullConfigsJson() << ",\n";
  H << "  \"sampling_policy\": \"" << FullSamplingPolicy << "\",\n";
  H << "  \"domain_formula\": \"" << FullDomainFormula << "\",\n";
  H << "  \"generator_hash\": \"" << GeneratorHashText << "\",\n";
  H << "  \"sha256\": \"" << Sha << "\",\n";
  H.flush();
  const uint64_t Bytes = Header.size() + AtomsPrefix.size() + AtomsText.size() +
                         AtomsSuffix.size();

  std::error_code EC;
  raw_fd_ostream Out(OutPath, EC);
  ASSERT_FALSE(EC) << OutPath << ": " << EC.message();
  Out << Header << AtomsPrefix << AtomsText << AtomsSuffix;
  Out.flush();
  ASSERT_FALSE(Out.has_error()) << OutPath;

  const auto ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - Started)
                             .count();
  llvm::outs() << "full-corpus-generate: domain_atoms_raw=" << RawAtomCount
               << " trimmed_unevaluable=" << TrimmedUnevaluable
               << " atoms=" << AtomCount << " bytes=" << Bytes
               << " elapsed_ms=" << ElapsedMs << " sha256=" << Sha
               << " generator_hash=" << GeneratorHashText << " path=" << OutPath
               << "\n";
}

TEST_F(X86LegalizerDifferentialTest, DISABLED_FullCorpusDifferential) {
  const char *InPath = std::getenv("X86_LEGALIZER_FULL_IN");
  if (!InPath)
    GTEST_SKIP() << "set X86_LEGALIZER_FULL_IN to the generated artifact";

  const auto Started = std::chrono::steady_clock::now();
  auto Buffer = MemoryBuffer::getFile(InPath);
  ASSERT_TRUE(Buffer) << InPath << ": " << Buffer.getError().message();
  const StringRef Text = (*Buffer)->getBuffer();
  const std::string InputSha = sha256Hex(Text);

  auto Value = json::parse(Text);
  ASSERT_TRUE(!!Value) << toString(Value.takeError());
  const json::Object *Root = Value->getAsObject();
  ASSERT_NE(Root, nullptr);
  EXPECT_EQ(Root->getString("schema").value_or(""),
            "llvm.gisel.x86-legalizer-full");
  EXPECT_EQ(Root->getInteger("version").value_or(-1), 1);
  auto RawCount = Root->getInteger("domain_atoms_raw");
  auto TrimmedCount = Root->getInteger("trimmed_unevaluable");
  auto AtomCount = Root->getInteger("atom_count");
  ASSERT_TRUE(RawCount && TrimmedCount && AtomCount);
  EXPECT_EQ(*RawCount, 826144);
  EXPECT_EQ(*AtomCount + *TrimmedCount, *RawCount);
  const json::Array *AtomArray = Root->getArray("atoms");
  ASSERT_NE(AtomArray, nullptr);
  ASSERT_EQ(AtomArray->size(), static_cast<size_t>(*AtomCount));
  const json::Array *TrimLog = Root->getArray("trim_log");
  ASSERT_NE(TrimLog, nullptr);
  EXPECT_EQ(TrimLog->empty(), *TrimmedCount == 0);
  ASSERT_LE(TrimLog->size(), 1u);
  std::string TrimReason;
  std::string TrimExample;
  if (!TrimLog->empty()) {
    const json::Object *Entry = (*TrimLog)[0].getAsObject();
    ASSERT_NE(Entry, nullptr);
    auto Reason = Entry->getString("reason");
    auto Example = Entry->getString("example");
    auto Count = Entry->getInteger("count");
    ASSERT_TRUE(Reason && Example && Count);
    EXPECT_EQ(static_cast<uint64_t>(*Count),
              static_cast<uint64_t>(*TrimmedCount));
    TrimReason = Reason->str();
    TrimExample = Example->str();
  }

  // The digest covers the generation parameters and the exact atom text.
  static constexpr StringLiteral AtomsPrefix = "  \"atoms\": [\n";
  static constexpr StringLiteral AtomsSuffix = "\n  ]\n}\n";
  const size_t AtomsBegin = Text.find(AtomsPrefix);
  const size_t AtomsEnd = Text.rfind(AtomsSuffix);
  ASSERT_NE(AtomsBegin, StringRef::npos) << "artifact has no atoms array";
  ASSERT_NE(AtomsEnd, StringRef::npos) << "artifact has no atoms terminator";
  ASSERT_GT(AtomsEnd, AtomsBegin);
  const StringRef AtomText =
      Text.substr(AtomsBegin + AtomsPrefix.size(),
                  AtomsEnd - AtomsBegin - AtomsPrefix.size());

  CorpusDigestInput DigestInput;
  DigestInput.GeneratorHash =
      Root->getString("generator_hash").value_or("").str();
  DigestInput.DomainAtomsRaw = *RawCount;
  DigestInput.TrimmedUnevaluable = *TrimmedCount;
  DigestInput.TrimReason = TrimReason;
  DigestInput.TrimExample = TrimExample;
  const std::string RecomputedSha = corpusDigest(DigestInput, AtomText);
  EXPECT_EQ(Root->getString("sha256").value_or(""), RecomputedSha)
      << "artifact sha256 does not match its atoms and generation parameters";

  std::string ManifestError;
  std::optional<std::map<std::string, std::string>> Manifest =
      readFullCorpusManifest(ManifestError);
  ASSERT_TRUE(Manifest.has_value()) << ManifestError;
  EXPECT_EQ((*Manifest)["schema"],
            "llvm.gisel.x86-legalizer-full-manifest");
  EXPECT_EQ((*Manifest)["version"], "1");
  EXPECT_EQ((*Manifest)["sha256"], RecomputedSha);
  EXPECT_EQ((*Manifest)["atom_count"], std::to_string(*AtomCount));
  EXPECT_EQ((*Manifest)["domain_atoms_raw"], std::to_string(*RawCount));
  EXPECT_EQ((*Manifest)["trimmed_unevaluable"], std::to_string(*TrimmedCount));
  EXPECT_EQ((*Manifest)["generator_hash"], DigestInput.GeneratorHash);

  struct Fixture {
    Sigma S;
    std::unique_ptr<TableLegalizerInfoForTest> Table;
    std::unique_ptr<X86LegalizerInfoReference> Oracle;
    std::string TableConfig;
    unsigned TableIndex = ~0u;
  };
  std::map<std::string, Fixture> Fixtures;
  for (const Config &C : FullConfigs) {
    Fixture F;
    F.S = makeSigma(C);
    ASSERT_TRUE(F.S.STI) << C.ID;
    F.Oracle = std::make_unique<X86LegalizerInfoReference>(
        *F.S.STI, *static_cast<const X86TargetMachine *>(F.S.TM.get()));
    Fixtures.emplace(C.ID, std::move(F));
  }

  std::map<std::string, unsigned> TableForSet;
  for (unsigned I = 0; I != X86TestNumTables; ++I)
    TableForSet.emplace(X86RuleIndexTableNames[I], I);

  std::string TargetsError;
  std::optional<MutationTargetMap> MutationTargets =
      readMutationTargets(TargetsError);
  ASSERT_TRUE(MutationTargets.has_value()) << TargetsError;
  ASSERT_EQ(static_cast<int64_t>(MutationTargets->size()),
            manifestCount("mutation_target_count"));

  uint64_t Atoms = 0, Diffs = 0, ExpectedDiffs = 0;
  uint64_t Hits = 0, Residuals = 0, Escapes = 0;
  std::set<std::string> Ids;

  for (const json::Value &AtomValue : *AtomArray) {
    const json::Object *O = AtomValue.getAsObject();
    ASSERT_NE(O, nullptr) << "atom is not an object";
    auto Id = O->getString("id");
    auto RuleSet = O->getString("rule_set");
    auto ConfigID = O->getString("config");
    ASSERT_TRUE(Id && RuleSet && ConfigID);
    ASSERT_TRUE(Ids.insert(Id->str()).second) << "duplicate atom id " << *Id;

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

    auto TableIt = TableForSet.find(RuleSet->str());
    ASSERT_NE(TableIt, TableForSet.end()) << *RuleSet;
    const std::optional<unsigned> Opcode =
        opcodeForName(X86TestTables[TableIt->second].Opcode);
    ASSERT_TRUE(Opcode.has_value()) << X86TestTables[TableIt->second].Opcode;

    SmallVector<LLT, 3> TypeVec(Types.begin(), Types.end());
    const LegalityQuery Q(*Opcode, TypeVec, MemDescs);
    const FirstMatchMutation Mutation =
        firstMatchMutation(TableIt->second, *F.S.STI, Q, *MutationTargets);
    if (Mutation.TargetOutOfRange) {
      ADD_FAILURE() << "artifact atom " << *Id << " targets slot "
                    << Mutation.TargetSlot << " but the query has "
                    << Types.size() << " types";
      continue;
    }
    if (Mutation.TargetIsPointer && Mutation.ActionChangesElementSize) {
      ADD_FAILURE() << "artifact atom " << *Id
                    << " is a pointer element-size query that should have "
                       "been trimmed (rule #"
                    << Mutation.RuleIndex << ", target slot "
                    << Mutation.TargetSlot << ")";
      continue;
    }
    // One table instance per rule set: a fresh table has at most 24*24*48
    // cells, so its uint16_t Steps index cannot overflow.
    if (!F.Table || F.TableConfig != ConfigID->str() ||
        F.TableIndex != TableIt->second) {
      F.Table = std::make_unique<TableLegalizerInfoForTest>(*F.S.STI);
      F.TableConfig = ConfigID->str();
      F.TableIndex = TableIt->second;
    }
    const X86LegalizerTable::Result R = F.Table->table().query(Q);
    const LegalizeActionStep Got(R.Action, R.TypeIdx, R.NewType);
    const LegalizeActionStep Want = F.Oracle->getAction(Q);

    switch (R.Kind) {
    case X86LegalizerTable::Answer::Hit:
      ++Hits;
      break;
    case X86LegalizerTable::Answer::Residual:
      ++Residuals;
      break;
    default:
      ++Escapes;
      break;
    }

    if (!stepsAgree(Expected, Want)) {
      ++ExpectedDiffs;
      ADD_FAILURE() << "full corpus atom " << *Id << " rule-set=" << *RuleSet
                    << " config=" << *ConfigID << "\n  query:    "
                    << formatQueryText(*RuleSet, Types, MemDescs)
                    << "\n  committed: " << describe(Expected)
                    << "\n  oracle:    " << describe(Want);
    }
    if (!stepsAgree(Got, Want)) {
      ++Diffs;
      ADD_FAILURE() << "full corpus atom " << *Id << " rule-set=" << *RuleSet
                    << " config=" << *ConfigID << "\n  query:    "
                    << formatQueryText(*RuleSet, Types, MemDescs)
                    << "\n  expected: " << describe(Expected)
                    << "\n  table:    " << describe(Got)
                    << "\n  oracle:   " << describe(Want);
    }
    ++Atoms;
  }

  EXPECT_EQ(Atoms, static_cast<uint64_t>(*AtomCount));
  EXPECT_EQ(Diffs, 0u);
  EXPECT_EQ(ExpectedDiffs, 0u);
  EXPECT_EQ(Escapes, 0u) << "the table must answer every non-trimmed atom";
  const auto ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - Started)
                             .count();
  llvm::outs() << "full-corpus: domain_atoms_raw=" << *RawCount
               << " trimmed_unevaluable=" << *TrimmedCount << " atoms=" << Atoms
               << " diffs=" << Diffs << " expected_diffs=" << ExpectedDiffs
               << " hit=" << Hits << " residual=" << Residuals
               << " escape=" << Escapes << " elapsed_ms=" << ElapsedMs
               << " sha256=" << InputSha << "\n";
}

TEST_F(X86LegalizerDifferentialTest, DISABLED_GenerateDeepPanel) {
  const char *OutPath = std::getenv("X86_LEGALIZER_DEEP_OUT");
  if (!OutPath)
    GTEST_SKIP() << "set X86_LEGALIZER_DEEP_OUT to the absolute output path";

  const std::vector<LLT> Panel = deepPanelAtoms();
  ASSERT_EQ(Panel.size(), 60u);
  std::set<std::string> Names;
  std::string ListText;
  for (LLT T : Panel) {
    ASSERT_TRUE(T.isValid());
    EXPECT_TRUE(Names.insert(witnessLLTName(T)).second)
        << "duplicate deep panel LLT " << witnessLLTName(T);
    ListText += witnessLLTName(T);
    ListText += '\n';
  }
  ASSERT_EQ(Names.size(), 60u);
  const std::string ListSha = sha256Hex(ListText);
  const std::string GeneratorHash =
      utohexstr(fnv1a64Text(Fnv1a64Offset, ListText), /*LowerCase=*/true, 16);

  std::string Json;
  raw_string_ostream OS(Json);
  OS << "{\n";
  OS << "  \"schema\": \"llvm.gisel.x86-legalizer-deep-panel\",\n";
  OS << "  \"version\": 1,\n";
  OS << "  \"comment\": \"Reviewed 60-atom boundary panel: the 12-atom "
        "reviewed panel plus 48 scalars and vectors outside the 24-class "
        "grid.  Generated by DISABLED_GenerateDeepPanel; do not "
        "hand-edit.\",\n";
  OS << "  \"reviewed_by\": \"nodecoda\",\n";
  OS << "  \"atom_count\": 60,\n";
  OS << "  \"sha256\": \"" << ListSha << "\",\n";
  OS << "  \"generator_hash\": \"" << GeneratorHash << "\",\n";
  OS << "  \"atoms\": [\n";
  for (size_t I = 0; I != Panel.size(); ++I)
    OS << "    \"" << witnessLLTName(Panel[I]) << "\""
       << (I + 1 == Panel.size() ? "\n" : ",\n");
  OS << "  ]\n}\n";
  OS.flush();

  std::error_code EC;
  raw_fd_ostream Out(OutPath, EC);
  ASSERT_FALSE(EC) << OutPath << ": " << EC.message();
  Out << Json;
  Out.flush();
  ASSERT_FALSE(Out.has_error()) << OutPath;

  llvm::outs() << "deep-panel-generate: atoms=" << Panel.size()
               << " bytes=" << Json.size() << " sha256=" << ListSha
               << " generator_hash=" << GeneratorHash << " path=" << OutPath
               << "\n";
}

TEST_F(X86LegalizerDifferentialTest, DISABLED_DeepPanelDifferential) {
  std::string Error;
  std::optional<std::vector<LLT>> CommittedPanel = readDeepPanel(Error);
  ASSERT_TRUE(CommittedPanel.has_value()) << Error;
  ASSERT_EQ(CommittedPanel->size(), 60u);
  const ArrayRef<LLT> Panel(*CommittedPanel);

  std::string TargetsError;
  std::optional<MutationTargetMap> MutationTargets =
      readMutationTargets(TargetsError);
  ASSERT_TRUE(MutationTargets.has_value()) << TargetsError;
  ASSERT_EQ(static_cast<int64_t>(MutationTargets->size()),
            manifestCount("mutation_target_count"));

  const auto Started = std::chrono::steady_clock::now();
  uint64_t RawAtoms = 0, TrimmedUnevaluable = 0;
  uint64_t Atoms = 0, Hits = 0, Residuals = 0, Escapes = 0, Diffs = 0;
  std::string TrimExample;

  static const unsigned DeepConfigs[] = {1, 2, 3};
  for (unsigned CI : DeepConfigs) {
    Sigma S = makeSigma(FullConfigs[CI]);
    ASSERT_TRUE(S.STI) << FullConfigs[CI].ID;
    X86LegalizerInfoReference Oracle(
        *S.STI, *static_cast<const X86TargetMachine *>(S.TM.get()));
    const unsigned PointerBits = FullConfigs[CI].Is64 ? 64 : 32;
    const TableDomainAxis A = tableDomainAxis(PointerBits);

    for (unsigned TI = 0; TI != X86TestNumTables; ++TI) {
      TableLegalizerInfoForTest Table(*S.STI);
      const X86TestTable &T = X86TestTables[TI];
      const std::optional<unsigned> Opcode = opcodeForName(T.Opcode);
      ASSERT_TRUE(Opcode.has_value()) << T.Opcode;
      auto Consider = [&](ArrayRef<LLT> Types, bool HasMem, LLT MemTy,
                          uint64_t Align) {
        SmallVector<LLT, 3> TypeVec(Types.begin(), Types.end());
        SmallVector<LegalityQuery::MemDesc, 1> MemDescs;
        if (HasMem)
          MemDescs.emplace_back(MemTy, Align, AtomicOrdering::NotAtomic,
                                AtomicOrdering::NotAtomic);
        const LegalityQuery Q(*Opcode, TypeVec, MemDescs);
        ++RawAtoms;
        const FirstMatchMutation Mutation =
            firstMatchMutation(TI, *S.STI, Q, *MutationTargets);
        if (Mutation.TargetOutOfRange) {
          ADD_FAILURE() << "core.json mutation target " << Mutation.TargetSlot
                        << " is outside the query arity for "
                        << FullConfigs[CI].ID << "/" << T.Name << " rule #"
                        << Mutation.RuleIndex;
          return;
        }
        if (Mutation.TargetIsPointer && Mutation.ActionChangesElementSize) {
          ++TrimmedUnevaluable;
          if (TrimExample.empty())
            TrimExample = std::string(FullConfigs[CI].ID) + "/" + T.Name +
                          " " + formatQueryText(T.Opcode, TypeVec, MemDescs);
          return;
        }
        const X86LegalizerTable::Result R = Table.table().query(Q);
        const LegalizeActionStep Got(R.Action, R.TypeIdx, R.NewType);
        const LegalizeActionStep Want = Oracle.getAction(Q);
        switch (R.Kind) {
        case X86LegalizerTable::Answer::Hit:
          ++Hits;
          break;
        case X86LegalizerTable::Answer::Residual:
          ++Residuals;
          break;
        default:
          ++Escapes;
          break;
        }
        if (!stepsAgree(Got, Want)) {
          ++Diffs;
          ADD_FAILURE() << "deep atom rule-set=" << T.Name
                        << " config=" << FullConfigs[CI].ID
                        << "\n  query:    "
                        << formatQueryText(T.Opcode, TypeVec, MemDescs)
                        << "\n  table:    " << describe(Got)
                        << "\n  oracle:   " << describe(Want);
        }
        ++Atoms;
      };
      enumerateClassPairPoints(T, A, Consider);
      enumerateBoundaryPanelPoints(T, A, Panel, Consider);
      enumerateScalarASPoints(T, A, Consider);
      enumerateVectorASPoints(T, A, Consider);
    }
  }

  EXPECT_EQ(RawAtoms, 900264u);
  EXPECT_EQ(Atoms + TrimmedUnevaluable, RawAtoms);
  EXPECT_EQ(Diffs, 0u);
  EXPECT_EQ(Escapes, 0u) << "the table must answer every non-trimmed atom";
  const auto ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - Started)
                             .count();
  llvm::outs() << "deep-panel: domain_atoms_raw=" << RawAtoms
               << " trimmed_unevaluable=" << TrimmedUnevaluable << " atoms=" << Atoms
               << " hit=" << Hits << " residual=" << Residuals
               << " escape=" << Escapes << " diffs=" << Diffs
               << " elapsed_ms=" << ElapsedMs << "\n";
  if (TrimmedUnevaluable != 0)
    llvm::outs() << "deep-panel trim example: " << TrimExample << "\n";
}

TEST_F(X86LegalizerDifferentialTest, DeepPanelMatchesGenerator) {
  std::string Error;
  std::optional<std::vector<LLT>> Committed = readDeepPanel(Error);
  ASSERT_TRUE(Committed.has_value()) << Error;
  const std::vector<LLT> Generated = deepPanelAtoms();
  ASSERT_EQ(Committed->size(), Generated.size());
  for (size_t I = 0; I != Generated.size(); ++I)
    EXPECT_EQ(witnessLLTName((*Committed)[I]), witnessLLTName(Generated[I]))
        << "committed deep panel differs from deepPanelAtoms() at index " << I;
  llvm::outs() << "deep-panel-committed: atoms=" << Committed->size() << "\n";
}

TEST_F(X86LegalizerDifferentialTest, FullCorpusManifestIsConsistent) {
  std::string Error;
  std::optional<std::map<std::string, std::string>> Manifest =
      readFullCorpusManifest(Error);
  ASSERT_TRUE(Manifest.has_value()) << Error;

  auto Number = [&](StringRef Key) -> std::optional<uint64_t> {
    auto It = Manifest->find(Key.str());
    if (It == Manifest->end())
      return std::nullopt;
    uint64_t Value = 0;
    if (StringRef(It->second).getAsInteger(10, Value))
      return std::nullopt;
    return Value;
  };
  auto Raw = Number("domain_atoms_raw");
  auto Atoms = Number("atom_count");
  auto Trimmed = Number("trimmed_unevaluable");
  ASSERT_TRUE(Raw.has_value()) << "missing domain_atoms_raw";
  ASSERT_TRUE(Atoms.has_value()) << "missing atom_count";
  ASSERT_TRUE(Trimmed.has_value()) << "missing trimmed_unevaluable";
  EXPECT_EQ(*Atoms + *Trimmed, *Raw);
  EXPECT_LT(*Trimmed, *Raw);

  EXPECT_EQ((*Manifest)["schema"],
            "llvm.gisel.x86-legalizer-full-manifest");
  EXPECT_EQ((*Manifest)["version"], "1");
  const std::string &Digest = (*Manifest)["sha256"];
  EXPECT_EQ(Digest.size(), 64u);
  EXPECT_EQ(Digest.find_first_not_of("0123456789abcdef"), std::string::npos);
  EXPECT_EQ((*Manifest)["sha256_scope"],
            "atoms+generation-parameters (generator_base_commit excluded)");
  EXPECT_EQ((*Manifest)["generator_hash"].size(), 16u);
  llvm::outs() << "full-corpus-manifest: raw=" << *Raw << " atoms=" << *Atoms
               << " trimmed=" << *Trimmed << " sha256=" << Digest << "\n";
}

TEST_F(X86LegalizerDifferentialTest, CiBudgetsAreRecorded) {
  std::optional<std::string> Text =
      readTestSourceFile("x86-legalizer-ci-budgets.json");
  ASSERT_TRUE(Text.has_value()) << "cannot read x86-legalizer-ci-budgets.json";
  auto Parsed = json::parse(*Text);
  ASSERT_TRUE(!!Parsed) << toString(Parsed.takeError());
  const json::Object *Root = Parsed->getAsObject();
  ASSERT_TRUE(Root) << "CI budget root is not an object";
  ASSERT_EQ(Root->getString("schema").value_or(""),
            "llvm.gisel.x86-legalizer-ci-budgets");

  const json::Object *Machine = Root->getObject("machine");
  ASSERT_TRUE(Machine) << "missing machine";
  std::optional<StringRef> Uname = Machine->getString("uname");
  ASSERT_TRUE(Uname.has_value()) << "missing machine.uname";
  EXPECT_FALSE(Uname->empty());
  std::optional<uint64_t> Nproc = getJSONUInt(*Machine, "nproc");
  ASSERT_TRUE(Nproc.has_value()) << "missing machine.nproc";
  EXPECT_GT(*Nproc, 0u);

  const json::Object *Measured = Root->getObject("measured");
  ASSERT_TRUE(Measured) << "missing measured";
  static const char *const MeasuredKeys[] = {
      "x86tests_ms",           "globalisel_lit_ms", "tablegen_lit_ms",
      "x86_codegen_lit_ms",    "fuzz_smoke_ms",     "fuzz_nightly_ms",
      "full_corpus_ms",        "full_corpus_bytes", "full_corpus_atoms",
      "full_domain_atoms_raw", "full_trimmed_unevaluable", "deep_panel_ms",
      "deep_panel_atoms",      "deep_domain_atoms_raw", "deep_trimmed_unevaluable",
  };
  std::string Summary;
  raw_string_ostream OS(Summary);
  OS << "ci-budget measured:";
  for (const char *Key : MeasuredKeys) {
    std::optional<uint64_t> Value = getJSONUInt(*Measured, Key);
    ASSERT_TRUE(Value.has_value()) << "missing measured." << Key;
    EXPECT_GT(*Value, 0u) << Key;
    OS << " " << Key << "=" << *Value;
  }
  OS << "\n";
  OS.flush();
  llvm::outs() << Summary;

  const json::Object *Budgets = Root->getObject("budgets");
  ASSERT_TRUE(Budgets) << "missing budgets";
  std::optional<uint64_t> Target = getJSONUInt(*Budgets, "x86tests_target_ms");
  std::optional<uint64_t> Hard = getJSONUInt(*Budgets, "x86tests_hard_ms");
  std::optional<uint64_t> CheckLLVMDeltaMs =
      getJSONUInt(*Budgets, "check_llvm_delta_ms");
  std::optional<uint64_t> CheckLLVMDeltaPercent =
      getJSONUInt(*Budgets, "check_llvm_delta_percent");
  std::optional<uint64_t> FullCorpusMinutes =
      getJSONUInt(*Budgets, "full_corpus_minutes");
  std::optional<uint64_t> DeepPanelHours =
      getJSONUInt(*Budgets, "deep_panel_hours");
  ASSERT_TRUE(Target.has_value());
  ASSERT_TRUE(Hard.has_value());
  ASSERT_TRUE(CheckLLVMDeltaMs.has_value());
  ASSERT_TRUE(CheckLLVMDeltaPercent.has_value());
  ASSERT_TRUE(FullCorpusMinutes.has_value());
  ASSERT_TRUE(DeepPanelHours.has_value());
  EXPECT_LT(*Target, *Hard);
  EXPECT_EQ(*Hard, 10000u);
  EXPECT_EQ(*CheckLLVMDeltaMs, 2000u);
  EXPECT_EQ(*CheckLLVMDeltaPercent, 10u);
  EXPECT_EQ(*FullCorpusMinutes, 30u);
  EXPECT_EQ(*DeepPanelHours, 2u);

  std::optional<uint64_t> FullAtoms =
      getJSONUInt(*Measured, "full_corpus_atoms");
  std::optional<uint64_t> FullRaw =
      getJSONUInt(*Measured, "full_domain_atoms_raw");
  std::optional<uint64_t> FullTrimmed =
      getJSONUInt(*Measured, "full_trimmed_unevaluable");
  std::optional<uint64_t> DeepAtoms =
      getJSONUInt(*Measured, "deep_panel_atoms");
  std::optional<uint64_t> DeepRaw =
      getJSONUInt(*Measured, "deep_domain_atoms_raw");
  std::optional<uint64_t> DeepTrimmed =
      getJSONUInt(*Measured, "deep_trimmed_unevaluable");
  ASSERT_TRUE(FullAtoms.has_value()) << "missing full_corpus_atoms";
  ASSERT_TRUE(FullRaw.has_value()) << "missing full_domain_atoms_raw";
  ASSERT_TRUE(FullTrimmed.has_value()) << "missing full_trimmed_unevaluable";
  ASSERT_TRUE(DeepAtoms.has_value()) << "missing deep_panel_atoms";
  ASSERT_TRUE(DeepRaw.has_value()) << "missing deep_domain_atoms_raw";
  ASSERT_TRUE(DeepTrimmed.has_value()) << "missing deep_trimmed_unevaluable";
  EXPECT_EQ(*FullRaw, 826144u);
  EXPECT_EQ(*FullAtoms + *FullTrimmed, *FullRaw);
  EXPECT_LT(*FullTrimmed, *FullRaw);
  EXPECT_EQ(*DeepRaw, 900264u);
  EXPECT_EQ(*DeepAtoms + *DeepTrimmed, *DeepRaw);
  EXPECT_LT(*DeepTrimmed, *DeepRaw);

  const json::Object *Pending = Root->getObject("pending_g008");
  ASSERT_TRUE(Pending) << "missing pending_g008";
  EXPECT_EQ(Pending->size(), 0u)
      << "pending_g008 must be empty once both workloads are measured";
}

} // namespace
