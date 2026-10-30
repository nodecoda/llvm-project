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

#include "X86LegalizerInfoReference.h"
#include "X86Subtarget.h"
#include "X86TargetMachine.h"
#include "x86-ctpop-atoms.h"
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"
#include <string>
#include <vector>

using namespace llvm;
using namespace TargetOpcode;
using namespace LegalizeActions;
using namespace LegalityPredicates;
using namespace LegalizeMutations;

namespace {

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

struct Config {
  const char *ID;
  const char *Triple;
  const char *FS;
  bool Is64;
  bool AVX512;
  bool VPOPCNTDQ;
  bool VLX;
};

// C5/C6 exist so that (HasAVX512 vs HasVPOPCNTDQ) and (HasVLX vs
// HasVPOPCNTDQVL) are separable.  Without them a transcription error that
// swaps either pair is invisible on every atom.
static const Config Configs[] = {
    {"C0", "x86_64", "+popcnt", true, false, false, false},
    {"C1", "x86_64", "+popcnt,+avx512f,+avx512vpopcntdq", true, true, true,
     false},
    {"C2", "x86_64", "+popcnt,+avx512f,+avx512vl,+avx512vpopcntdq", true, true,
     true, true},
    {"C3", "x86_64", "", true, false, false, false},
    {"C4", "i386", "+popcnt", false, false, false, false},
    {"C5", "x86_64", "+popcnt,+avx512f", true, true, false, false},
    {"C6", "x86_64", "+popcnt,+avx512f,+avx512vl", true, true, false, true},
};

struct TypeClass {
  const char *ID;
  LLT Dst;
  LLT Src;
};

static std::vector<TypeClass> typeClasses() {
  const LLT s7 = LLT::scalar(7), s8 = LLT::scalar(8), s16 = LLT::scalar(16),
            s24 = LLT::scalar(24), s32 = LLT::scalar(32),
            s64 = LLT::scalar(64), s128 = LLT::scalar(128),
            s200 = LLT::scalar(200);
  const LLT v2s32 = LLT::fixed_vector(2, 32), v4s32 = LLT::fixed_vector(4, 32),
            v8s32 = LLT::fixed_vector(8, 32),
            v16s32 = LLT::fixed_vector(16, 32),
            v2s64 = LLT::fixed_vector(2, 64), v4s64 = LLT::fixed_vector(4, 64),
            v8s64 = LLT::fixed_vector(8, 64),
            v16s8 = LLT::fixed_vector(16, 8);
  return {
      {"T0", s8, s8},       {"T1", s16, s16},   {"T2", s32, s32},
      {"T3", s64, s64},     {"T4", s128, s128}, {"T5", v4s32, v4s32},
      {"T6", v8s32, v8s32}, {"T7", v2s64, v2s64}, {"T8", v4s64, v4s64},
      {"T9", v16s32, v16s32}, {"T10", v8s64, v8s64}, {"T11", v16s8, v16s8},
      {"T12", v2s32, v2s32}, {"T13", s7, s7},   {"T14", s24, s24},
      {"N0", s128, s64},    {"N1", s64, s32},   {"B0", s200, s200},
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
  S.TM.reset(
      T->createTargetMachine(TT, "generic", C.FS, Opts, std::nullopt));
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

// The comparison predicate.  It must be sensitive to all three fields:
// comparing only the action would miss a table that widens to the wrong type,
// and comparing only the mutation would miss a table that legalizes where the
// other widens.
static bool stepsAgree(const LegalizeActionStep &A, const LegalizeActionStep &B) {
  if (A.Action != B.Action)
    return false;
  if (A.Action == Legal || A.Action == Unsupported || A.Action == Custom)
    return true;
  return A.TypeIdx == B.TypeIdx && A.NewType == B.NewType;
}

static std::string describe(const LegalizeActionStep &S) {
  std::string Out = std::to_string(static_cast<unsigned>(S.Action));
  Out += " idx=" + std::to_string(S.TypeIdx);
  if (S.Action != Legal && S.Action != Unsupported && S.Action != Custom) {
    raw_string_ostream OS(Out);
    OS << " to=" << S.NewType;
  }
  return Out;
}

TEST_F(X86LegalizerDifferentialTest, FeatureBoolsPerConfig) {
  for (const Config &C : Configs) {
    Sigma S = makeSigma(C);
    ASSERT_TRUE(S.STI) << C.ID;
    EXPECT_EQ(S.STI->is64Bit(), C.Is64) << C.ID;
    EXPECT_EQ(S.STI->hasAVX512(), C.AVX512) << C.ID;
    EXPECT_EQ(S.STI->hasVPOPCNTDQ(), C.VPOPCNTDQ) << C.ID;
    EXPECT_EQ(S.STI->hasVLX(), C.VLX) << C.ID;
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
      OS << C.ID << "|G_CTPOP|" << T.Dst << "|" << T.Src << "|none|unused\n";
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

TEST_F(X86LegalizerDifferentialTest, FrozenCorpusMatchesJson) {
  EXPECT_EQ(typeClasses().size() * std::size(Configs),
            (size_t)X86_CTPOP_ATOM_COUNT);
  char Buf[32];
  snprintf(Buf, sizeof(Buf), "%016llx", (unsigned long long)canonicalAtomHash());
  EXPECT_STREQ(X86_CTPOP_ATOMS_FNV1A64, Buf)
      << "atom table drifted from x86-ctpop-atoms.json";
}

TEST_F(X86LegalizerDifferentialTest, FirstStepDifferential) {
  unsigned Atoms = 0, LegalBoth = 0, UnsupportedBoth = 0, Diffs = 0;
  std::string Report;

  for (const Config &C : Configs) {
    Sigma S = makeSigma(C);
    ASSERT_TRUE(S.STI) << C.ID;
    X86LegalizerInfoReference Oracle(*S.STI, *static_cast<const X86TargetMachine *>(S.TM.get()));
    GeneratedLegalizerInfoForTest SUT(*S.STI);

    for (const TypeClass &T : typeClasses()) {
      LLT Types[] = {T.Dst, T.Src};
      LegalityQuery Q(G_CTPOP, Types);
      LegalizeActionStep A = Oracle.getAction(Q);
      LegalizeActionStep B = SUT.getAction(Q);
      ++Atoms;
      if (A.Action == Legal && B.Action == Legal)
        ++LegalBoth;
      if (A.Action == Unsupported && B.Action == Unsupported)
        ++UnsupportedBoth;
      if (stepsAgree(A, B))
        continue;
      ++Diffs;
      raw_string_ostream OS(Report);
      OS << "FAIL x86/g_ctpop/" << C.ID << "/" << T.ID << "\n"
         << "  sigma:    " << C.Triple << " / " << C.FS << "\n"
         << "  features: avx512=" << C.AVX512
         << " vpopcntdq=" << C.VPOPCNTDQ << " vlx=" << C.VLX << "\n"
         << "  query:    G_CTPOP (" << T.Dst << ", " << T.Src << ")\n"
         << "  expected: " << describe(A) << "\n"
         << "  actual:   " << describe(B) << "\n";
    }
  }

  EXPECT_EQ(Atoms, 126u);
  EXPECT_GT(LegalBoth, 0u) << "corpus has no jointly-legal atom";
  EXPECT_GT(UnsupportedBoth, 0u) << "corpus has no jointly-unsupported atom";
  EXPECT_EQ(Diffs, 0u) << Report;
}

} // namespace
