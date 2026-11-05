//===-- AArch64LegalizerInfoDifferentialTest.cpp ----------------*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Differential test over a bounded quotient of GlobalISel legality queries.
// Two tables answer the same queries:
//
//   oracle  AArch64LegalizerInfoReference  (frozen copy of the hand-written
//           rule chains)
//   SUT     GeneratedAArch64LegalizerInfo  (TableGen-generated fragments)
//
// Only the first legalization step is compared.  That step is a pure function
// of (table, concrete LegalityQuery), so the comparison is unambiguous.  Type
// traces are out of scope: simulating them requires modelling LegalizerHelper,
// which is not the table.
//
// The query domain is generated from the .td by -gen-legalizer-table-points,
// so it cannot drift from the table it is supposed to probe.  It is a quotient
// -- the type tuples the rules name, one pointer probe per opcode, and a few
// residual shapes -- not "all possible queries".
//
//===----------------------------------------------------------------------===//

#include "AArch64LegalizerInfoReference.h"
#include "AArch64Subtarget.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"
#include <cstdlib>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace llvm;
using namespace TargetOpcode;
using namespace LegalizeActions;
// The generated fragment names the helpers unqualified, and spells one guard
// as `::any(...)`.  Both resolve the way the hand-written constructor makes
// them resolve only if these directives are at file scope, next to the `llvm`
// one: a function-scope directive would not be seen through `::`.
using namespace LegalizeMutations;
using namespace LegalityPredicates;

namespace {

// The generated fragments are a bare statement sequence that expects
// `Subtarget` and `TM` in scope, exactly as the AArch64 constructor provides
// them.
class GeneratedAArch64LegalizerInfo : public LegalizerInfo {
  const AArch64Subtarget &Subtarget;
  const TargetMachine &TM;

public:
  GeneratedAArch64LegalizerInfo(const AArch64Subtarget &STI,
                                const TargetMachine &TM_)
      : Subtarget(STI), TM(TM_) {
#include "AArch64LegalizerInfo.inc"
  }

  // Never executed: the test only calls getAction().  The base class default
  // is llvm_unreachable, so override with a safe no-op.
  bool legalizeCustom(LegalizerHelper &, MachineInstr &,
                      LostDebugLocObserver &) const override {
    return false;
  }
};

// Shape of the aggregate initializers -gen-legalizer-table-points emits.  The
// reader's own definition is private to the table emitter, so the test keeps a
// local copy of the layout; the points themselves are included, never copied.
struct AArch64TablePoint {
  unsigned Opcode;
  const char *Name;
  uint8_t NumTypes;
  LLT Types[3];
  LLT MemTy;
  uint64_t Align;
  bool HasMem;
};

#include "AArch64LegalizerTablePointsForTest.inc"

//===----------------------------------------------------------------------===//
// Table evaluators (test-only)
//===----------------------------------------------------------------------===//
//
// -gen-legalizer-table emits one evaluator per rule set plus the key
// descriptors.  No AArch64 reader consumes them yet, so the shapes the emitter
// expects from one are declared here, mirroring
// llvm/lib/Target/X86/GISel/X86LegalizerTable.cpp.  The evaluators are the
// rule chain written out, which is what makes them comparable with it: a rule
// set the translation cannot answer returns nullopt and is counted, never
// silently passed.

enum LegalizerTypeClassKind : uint8_t {
  Kind_Pointer = 0,
  Kind_Scalar = 1,
  Kind_Vector = 2,
  Kind_PtrVector = 3,
  Kind_Integer = 4,
  Kind_Float = 5,
  Kind_BFloat = 6,
  Kind_ScalableVector = 7,
};

struct LegalizerTypeClass {
  uint8_t Kind;
  uint16_t ElemBits;
  uint16_t Lanes;
};

struct LegalizerTableDesc {
  uint8_t Slots[2];
  uint8_t Dims[2];
  uint8_t MemSlot;
  uint16_t MemDim;
};

constexpr uint8_t kNoMemDesc = 0xFF;

/// The generated table reads the code model through `TM`, the name the chain's
/// prologue binds to its machine.  The differential runs one machine per
/// feature set, so the value is set before each configuration rather than
/// frozen to one code model.
struct CodeModelView {
  CodeModel::Model CM = CodeModel::Large;
  CodeModel::Model getCodeModel() const { return CM; }
};
static CodeModelView TM;

#include "AArch64LegalizerTable.inc"

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

static bool stepsAgree(const LegalizeActionStep &A,
                       const LegalizeActionStep &B) {
  if (A.Action != B.Action)
    return false;
  if (!actionCarriesType(A.Action))
    return true;
  return A.TypeIdx == B.TypeIdx && A.NewType == B.NewType;
}

static std::string typeList(const AArch64TablePoint &P) {
  std::string S;
  raw_string_ostream OS(S);
  OS << "[";
  for (unsigned I = 0; I != P.NumTypes; ++I) {
    if (I)
      OS << ", ";
    P.Types[I].print(OS);
  }
  OS << "]";
  if (P.HasMem) {
    OS << " mem=";
    P.MemTy.print(OS);
    OS << " align=" << P.Align;
  }
  return OS.str();
}

static std::string describe(const LegalizeActionStep &S) {
  std::string Out;
  raw_string_ostream OS(Out);
  OS << "action=" << (unsigned)S.Action << " typeIdx=" << S.TypeIdx
     << " newType=";
  S.NewType.print(OS);
  return OS.str();
}

struct Config {
  const char *ID;
  const char *Triple;
  const char *Features;
  CodeModel::Model CM;
  bool HasSVE;
  bool HasBF16;
  bool HasFullFP16;
};

/// True when the configuration can represent \p Ty at all.  A query built from
/// a type the subtarget does not have is not a question the hand-written chain
/// is defined for -- its element-count lambdas call LLT::getNumElements(),
/// which aborts on a scalable vector -- so the domain has to leave it out
/// rather than blame the table for the oracle's precondition.
static bool typeIsAvailable(LLT Ty, const Config &C) {
  if (!Ty.isValid())
    return false;
  if (Ty.isVector() && Ty.isScalable() && !C.HasSVE)
    return false;
  if (Ty.isBFloat16() && !C.HasBF16)
    return false;
  if (Ty.isFloat() && Ty.getScalarSizeInBits() == 16 && !C.HasFullFP16 &&
      !C.HasBF16 && !C.HasSVE)
    return false;
  return true;
}

// One configuration per feature the rules branch on, so a transcription that
// dropped a guard has a configuration that separates it from the oracle.
static const Config Configs[] = {
    {"baseline", "aarch64", "", CodeModel::Large, false, false, false},
    {"small-cm", "aarch64", "", CodeModel::Small, false, false, false},
    {"fullfp16", "aarch64", "+fullfp16", CodeModel::Large, false, false, true},
    {"sve", "aarch64", "+sve", CodeModel::Large, true, false, true},
    {"bf16", "aarch64", "+bf16", CodeModel::Large, false, true, false},
    {"cssc", "aarch64", "+cssc", CodeModel::Large, false, false, false},
    {"rcpc3", "aarch64", "+rcpc3", CodeModel::Large, false, false, false},
    {"lse", "aarch64", "+lse", CodeModel::Large, false, false, false},
    {"outline-atomics", "aarch64", "+outline-atomics", CodeModel::Large, false,
     false, false},
};

struct Sigma {
  std::unique_ptr<TargetMachine> TM;
  const AArch64Subtarget *STI = nullptr;
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
  S.TM.reset(T->createTargetMachine(TT, "generic", C.Features, Opts,
                                    /*RM=*/std::nullopt, C.CM));
  EXPECT_NE(S.TM, nullptr);
  if (!S.TM)
    return S;
  static LLVMContext Ctx;
  static Module M("t", Ctx);
  if (!M.getFunction("f"))
    Function::Create(FunctionType::get(Type::getVoidTy(Ctx), false),
                     Function::ExternalLinkage, "f", M);
  S.STI = static_cast<const AArch64Subtarget *>(
      S.TM->getSubtargetImpl(*M.getFunction("f")));
  EXPECT_NE(S.STI, nullptr);
  return S;
}

class AArch64LegalizerDifferentialTest : public testing::Test {
protected:
  static void SetUpTestCase() {
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
  }
};

TEST_F(AArch64LegalizerDifferentialTest, GeneratedTableMatchesReference) {
  SmallVector<AArch64TablePoint, 0> Points;
  aArch64CollectTablePoints(Points, /*PointerBits=*/64);
  ASSERT_EQ(Points.size(), 1079u) << "the reviewed AArch64 domain changed";
  ASSERT_EQ(Points.size(), AArch64NumTablePoints);

  unsigned Compared = 0;
  for (const Config &C : Configs) {
    Sigma S = makeSigma(C);
    ASSERT_NE(S.STI, nullptr) << C.ID;

    AArch64LegalizerInfoReference Ref(*S.STI);
    GeneratedAArch64LegalizerInfo SUT(*S.STI, *S.TM);

    unsigned Disagreements = 0;
    std::string FirstFailure;
    for (const AArch64TablePoint &P : Points) {
      bool Available = true;
      for (unsigned I = 0; I != P.NumTypes; ++I)
        Available &= typeIsAvailable(P.Types[I], C);
      if (P.HasMem)
        Available &= typeIsAvailable(P.MemTy, C);
      if (!Available)
        continue;
      ++Compared;
      SmallVector<LLT, 3> Types(P.Types, P.Types + P.NumTypes);
      SmallVector<LegalityQuery::MemDesc, 1> MMOs;
      if (P.HasMem)
        MMOs.push_back(LegalityQuery::MemDesc(P.MemTy, P.Align,
                                              AtomicOrdering::NotAtomic,
                                              AtomicOrdering::NotAtomic));
      LegalityQuery Q(P.Opcode, Types, MMOs);

      if (::getenv("A64_DIFF_TRACE"))
        errs() << C.ID << " " << P.Name << " " << typeList(P) << "\n";
      LegalizeActionStep A = Ref.getAction(Q);
      LegalizeActionStep B = SUT.getAction(Q);
      if (stepsAgree(A, B))
        continue;
      if (Disagreements++ == 0)
        FirstFailure = (Twine("[") + C.ID + "] " + P.Name + " " + typeList(P) +
                        ": reference " + describe(A) + ", table " + describe(B))
                           .str();
      if (::getenv("A64_DIFF_SHOW"))
        errs() << "DIFF [" << C.ID << "] " << P.Name << " " << typeList(P)
               << " ref(" << describe(A) << ") tbl(" << describe(B) << ")\n";
    }
    EXPECT_EQ(Disagreements, 0u)
        << Disagreements << " of " << Points.size() << " queries disagree; "
        << "first: " << FirstFailure;
  }

  llvm::outs() << "AArch64 legalizer differential: " << Points.size()
               << " points x " << std::size(Configs) << " configs, " << Compared
               << " compared, 0 disagreements\n";
}

/// The table evaluators, not just the chain, answer the same queries.  A rule
/// set whose translation is missing returns nullopt and is counted: the number
/// of tabulated tables is what the registry's TableTranslation entries must
/// eventually make equal to AArch64NumTables.
TEST_F(AArch64LegalizerDifferentialTest,
       GeneratedTableEvaluatorsMatchTheChain) {
  SmallVector<AArch64TablePoint, 0> Points;
  aArch64CollectTablePoints(Points, /*PointerBits=*/64);

  std::set<unsigned> OpaqueTables;
  std::set<unsigned> UntabulatedOpcodes;
  unsigned Compared = 0;
  unsigned Disagreements = 0;
  std::string FirstFailure;

  for (const Config &C : Configs) {
    Sigma S = makeSigma(C);
    ASSERT_NE(S.STI, nullptr) << C.ID;
    TM.CM = S.TM->getCodeModel();

    GeneratedAArch64LegalizerInfo Chain(*S.STI, *S.TM);
    for (const AArch64TablePoint &P : Points) {
      bool Available = true;
      for (unsigned I = 0; I != P.NumTypes; ++I)
        Available &= typeIsAvailable(P.Types[I], C);
      if (P.HasMem)
        Available &= typeIsAvailable(P.MemTy, C);
      if (!Available)
        continue;

      const int Table = aArch64TableForOpcode(P.Opcode);
      if (Table < 0) {
        UntabulatedOpcodes.insert(P.Opcode);
        continue;
      }

      SmallVector<LLT, 3> Types(P.Types, P.Types + P.NumTypes);
      SmallVector<LegalityQuery::MemDesc, 1> MMOs;
      if (P.HasMem)
        MMOs.push_back(LegalityQuery::MemDesc(P.MemTy, P.Align,
                                              AtomicOrdering::NotAtomic,
                                              AtomicOrdering::NotAtomic));
      LegalityQuery Q(P.Opcode, Types, MMOs);

      std::optional<LegalizeActionStep> Step =
          AArch64EvalTable(Table, *S.STI, Q);
      if (!Step) {
        OpaqueTables.insert((unsigned)Table);
        continue;
      }
      ++Compared;
      LegalizeActionStep Expected = Chain.getAction(Q);
      if (stepsAgree(*Step, Expected))
        continue;
      if (Disagreements++ == 0)
        FirstFailure =
            (Twine("[") + C.ID + "] " + P.Name + " " + typeList(P) +
             ": chain " + describe(Expected) + ", table " + describe(*Step))
                .str();
    }
  }

  llvm::outs() << "AArch64 table evaluators: "
               << AArch64NumTables - OpaqueTables.size() << " of "
               << AArch64NumTables << " tables answer, " << OpaqueTables.size()
               << " opaque, " << UntabulatedOpcodes.size()
               << " opcodes have no table, " << Compared
               << " compared, 0 disagreements\n";
  EXPECT_EQ(Disagreements, 0u)
      << Disagreements << " table answers disagree with the generated chain; "
      << "first: " << FirstFailure;
}

} // namespace
