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
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
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

// The comparison predicate.  It must be sensitive to all three fields:
// comparing only the action would miss a table that widens to the wrong type,
// and comparing only the mutation would miss a table that legalizes where the
// other widens.
static bool stepsAgree(const LegalizeActionStep &A,
                       const LegalizeActionStep &B) {
  if (A.Action != B.Action)
    return false;
  if (A.Action == Legal || A.Action == Unsupported || A.Action == Custom)
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
  if (S.Action != Legal && S.Action != Unsupported && S.Action != Custom)
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
  EXPECT_EQ(LiteralArray->size(), 25u);
  for (const json::Value &V : *LiteralArray) {
    auto S = V.getAsString();
    ASSERT_TRUE(S.has_value());
    EXPECT_NE(ReferenceText->find(*S), std::string::npos)
        << "literal not present in the reference source: " << *S;
  }
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

static void renderStepJson(raw_ostream &OS, const LegalizeActionStep &S) {
  OS << "{\"action\":\"" << S.Action << "\"";
  if (S.Action != Legal && S.Action != Unsupported && S.Action != Custom)
    OS << ",\"idx\":" << S.TypeIdx << ",\"to\":\"" << shortLLT(S.NewType)
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
    LLT Types[] = {ValueTy, PtrTy};
    LegalityQuery::MemDesc MemDescs[] = {{MemTy, /*AlignInBits=*/8,
                                          AtomicOrdering::NotAtomic,
                                          AtomicOrdering::NotAtomic}};
    LegalityQuery Q(G_STORE, Types, MemDescs);

    X86LegalizerTable::Result R = Table.table().query(Q);
    EXPECT_EQ(R.Kind, X86LegalizerTable::Answer::Residual)
        << Name << ": a non-zero address space must not use a table cell";

    LegalizeActionStep Got(R.Action, R.TypeIdx, R.NewType);
    LegalizeActionStep Want = Oracle.getAction(Q);
    EXPECT_TRUE(stepsAgree(Got, Want))
        << Name << ": table " << describe(Got) << ", oracle " << describe(Want);
    return Want;
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
