//===- X86LegalizerInfo.td - X86 GlobalISel legalizer -*-- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//
// X86 legalizer rules.
//
// Converted from the former hand-written chains in X86LegalizerInfo.cpp one
// opcode family at a time.  The opcodes not described here are still
// hand-written in that file; the two must not describe the same opcode, and
// -gen-legalizer enforces that within this file.
//
// The LegalizeFeature records below declare the subtarget-feature booleans the
// rules are written in terms of, and the X86_* types declare the LLT constants,
// so a rule never spells a feature or a type as C++ text.
//
// The type and feature names carry an X86_ prefix because
// llvm/Target/GlobalISel/Target.td already defines s32, s64, v2s64, v2s32,
// v4s32, v4s16, v8s16 and v8s8, and this file has to stay includable from a
// target .td that pulls in llvm/Target/Target.td.
//
// LegalizeOpaqueRule is the escape hatch for guards that are still raw C++;
// use it only for a guard the vocabulary cannot spell yet.
//
// `if (Cond) Action.foo(...)` became `Legalize...If<Cond, ...>`: the rule is
// always installed, but its guard is constant for a given subtarget, so a
// false guard is equivalent to the rule being absent.
//

include "llvm/Target/LegalizerInfo.td"

/// Subtarget-feature booleans the rules are written in terms of.  The record
/// name is the C++ identifier -gen-legalizer declares.
def Is64Bit : LegalizeFeature<"Subtarget.is64Bit()">;
def HasSSE1 : LegalizeFeature<"Subtarget.hasSSE1()">;
def HasSSE2 : LegalizeFeature<"Subtarget.hasSSE2()">;
def HasSSE41 : LegalizeFeature<"Subtarget.hasSSE41()">;
def HasAVX : LegalizeFeature<"Subtarget.hasAVX()">;
def HasAVX2 : LegalizeFeature<"Subtarget.hasAVX2()">;
def HasAVX512 : LegalizeFeature<"Subtarget.hasAVX512()">;
def HasVLX : LegalizeFeature<"Subtarget.hasVLX()">;
def HasDQI : LegalizeFeature<"Subtarget.hasAVX512() && Subtarget.hasDQI()">;
def HasBWI : LegalizeFeature<"Subtarget.hasAVX512() && Subtarget.hasBWI()">;
def UseX87 : LegalizeFeature<"!Subtarget.useSoftFloat() && Subtarget.hasX87()">;
def HasPOPCNT : LegalizeFeature<"Subtarget.hasPOPCNT()">;
def HasLZCNT : LegalizeFeature<"Subtarget.hasLZCNT()">;
def HasBMI : LegalizeFeature<"Subtarget.hasBMI()">;

/// The guards that need more than one leaf.  A `dag` argument cannot hold a
/// bare record, so the combinations are named instead of being wrapped in
/// `(all_of X)` at every use.
def DQIAndVLX : LegalizePredicateDag<(all_of HasDQI, HasVLX)>;
def POPCNTAnd64Bit : LegalizePredicateDag<(all_of HasPOPCNT, Is64Bit)>;
def LZCNTAnd64Bit : LegalizePredicateDag<(all_of HasLZCNT, Is64Bit)>;
def BMIAnd64Bit : LegalizePredicateDag<(all_of HasBMI, Is64Bit)>;
class SizeIsIn<int slot, int width>
    : LegalizeTypePredicate<"sizeIs(" # !cast<string>(slot) # ", " #
                            !cast<string>(width) # ")">;

def MergeWidths
    : LegalizeQueryPredicateDag<
          (all_of (any_of SizeIsIn<0, 16>, SizeIsIn<0, 32>, SizeIsIn<0, 64>,
                                   SizeIsIn<0, 128>, SizeIsIn<0, 256>,
                                   SizeIsIn<0, 512>),
                  (any_of SizeIsIn<1, 8>, SizeIsIn<1, 16>, SizeIsIn<1, 32>,
                          SizeIsIn<1, 64>, SizeIsIn<1, 128>,
                          SizeIsIn<1, 256>))>;
def UnmergeWidths
    : LegalizeQueryPredicateDag<
          (all_of (any_of SizeIsIn<1, 16>, SizeIsIn<1, 32>, SizeIsIn<1, 64>,
                                   SizeIsIn<1, 128>, SizeIsIn<1, 256>,
                                   SizeIsIn<1, 512>),
                  (any_of SizeIsIn<0, 8>, SizeIsIn<0, 16>, SizeIsIn<0, 32>,
                          SizeIsIn<0, 64>, SizeIsIn<0, 128>,
                          SizeIsIn<0, 256>))>;

def X86_p0 : LegalizeNamedType<
    "LLT::pointer(0, Subtarget.isTarget64BitLP64() ? 64 : 32)">;
def X86_s1 : LegalizeNamedType<"LLT::scalar(1)">;
def X86_s8 : LegalizeNamedType<"LLT::scalar(8)">;
def X86_s16 : LegalizeNamedType<"LLT::scalar(16)">;
def X86_s32 : LegalizeNamedType<"LLT::scalar(32)">;
def X86_s64 : LegalizeNamedType<"LLT::scalar(64)">;
def X86_s80 : LegalizeNamedType<"LLT::scalar(80)">;
def X86_s128 : LegalizeNamedType<"LLT::scalar(128)">;
def X86_sMaxScalar : LegalizeNamedType<"Subtarget.is64Bit() ? X86_s64 : X86_s32">;

def X86_v2s32 : LegalizeNamedType<"LLT::fixed_vector(2, 32)">;
def X86_v4s8 : LegalizeNamedType<"LLT::fixed_vector(4, 8)">;

def X86_v16s8 : LegalizeNamedType<"LLT::fixed_vector(16, 8)">;
def X86_v8s16 : LegalizeNamedType<"LLT::fixed_vector(8, 16)">;
def X86_v4s32 : LegalizeNamedType<"LLT::fixed_vector(4, 32)">;
def X86_v2s64 : LegalizeNamedType<"LLT::fixed_vector(2, 64)">;
def X86_v2p0 : LegalizeNamedType<"LLT::fixed_vector(2, X86_p0)">;

def X86_v32s8 : LegalizeNamedType<"LLT::fixed_vector(32, 8)">;
def X86_v16s16 : LegalizeNamedType<"LLT::fixed_vector(16, 16)">;
def X86_v8s32 : LegalizeNamedType<"LLT::fixed_vector(8, 32)">;
def X86_v4s64 : LegalizeNamedType<"LLT::fixed_vector(4, 64)">;
def X86_v4p0 : LegalizeNamedType<"LLT::fixed_vector(4, X86_p0)">;

def X86_v64s8 : LegalizeNamedType<"LLT::fixed_vector(64, 8)">;
def X86_v32s16 : LegalizeNamedType<"LLT::fixed_vector(32, 16)">;
def X86_v16s32 : LegalizeNamedType<"LLT::fixed_vector(16, 32)">;
def X86_v8s64 : LegalizeNamedType<"LLT::fixed_vector(8, 64)">;

def X86_s8MaxVector : LegalizeNamedType<"HasAVX512 ? X86_v64s8 : HasAVX ? X86_v32s8 : X86_v16s8">;
def X86_s16MaxVector : LegalizeNamedType<
    "HasAVX512 ? X86_v32s16 : HasAVX ? X86_v16s16 : X86_v8s16">;
def X86_s32MaxVector : LegalizeNamedType<
    "HasAVX512 ? X86_v16s32 : HasAVX ? X86_v8s32 : X86_v4s32">;
def X86_s64MaxVector : LegalizeNamedType<"HasAVX512 ? X86_v8s64 : HasAVX ? X86_v4s64 : X86_v2s64">;

// Types used by individual rule sets.
def X86_IntTypes32 : LegalizeNamedTypeList<[X86_s8, X86_s16, X86_s32, X86_p0]>;
def X86_IntTypes64 : LegalizeNamedTypeList<[X86_s8, X86_s16, X86_s32, X86_s64, X86_p0]>;
def X86_PtrTypes32 : LegalizeNamedTypeList<[X86_s1, X86_s8, X86_s16, X86_s32]>;
def X86_PtrTypes64 : LegalizeNamedTypeList<[X86_s1, X86_s8, X86_s16, X86_s32, X86_s64]>;

def X86_IMPLICIT_DEF : LegalizeRuleSet<[
  "G_IMPLICIT_DEF", "G_PHI", "G_FREEZE", "G_CONSTANT_FOLD_BARRIER"
]> {
  let Comments = [
    "implicit/constants",
    "32/64-bits needs support for s64/s128 to handle cases:",
    "s64 = EXTEND (G_IMPLICIT_DEF s32) -> s64 = G_IMPLICIT_DEF",
    "s128 = EXTEND (G_IMPLICIT_DEF s32/s64) -> s128 = G_IMPLICIT_DEF",
  ];
  let Rules = [
    LegalFor<[X86_p0, X86_s1, X86_s8, X86_s16, X86_s32, X86_s64]>,
    LegalForIf<UseX87, [X86_s80]>,
    LegalForIf<Is64Bit, [X86_s128]>,
    LegalForIf<HasSSE2, [X86_v16s8, X86_v8s16, X86_v4s32, X86_v2s64]>,
    LegalForIf<HasAVX, [X86_v32s8, X86_v16s16, X86_v8s32, X86_v4s64]>,
    LegalForIf<HasAVX512, [X86_v64s8, X86_v32s16, X86_v16s32, X86_v8s64]>,
    WidenScalarOrEltToNextPow2<0, 8>,
    ClampScalarOrElt<0, X86_s8, X86_sMaxScalar>,
    MoreElementsToNextPow2<0>,
    ClampNumElements<0, X86_v16s8, X86_s8MaxVector>,
    ClampNumElements<0, X86_v8s16, X86_s16MaxVector>,
    ClampNumElements<0, X86_v4s32, X86_s32MaxVector>,
    ClampNumElements<0, X86_v2s64, X86_s64MaxVector>,
    ClampMaxNumElements<0, X86_p0,
                                "Is64Bit ? X86_s64MaxVector.getNumElements() : X86_s32MaxVector.getNumElements()">,
    ScalarizeIf<LegalizeQueryPredicate<"scalarOrEltWiderThan(0, 64)">, 0>,
  ];
}

def X86_CONSTANT : LegalizeRuleSet<["G_CONSTANT"]> {
  let Rules = [
    LegalFor<[X86_p0, X86_s8, X86_s16, X86_s32]>,
    LegalForIf<Is64Bit, [X86_s64]>,
    WidenScalarToNextPow2<0, 8>,
    ClampScalar<0, X86_s8, X86_sMaxScalar>,
  ];
}

def X86_MERGE_VALUES : LegalizeRuleSet<["G_MERGE_VALUES"]> {
  let Rules = [
    WidenScalarToNextPow2<1, 8>,
    WidenScalarToNextPow2<0, 16>,
    MinScalar<1, X86_s8>,
    MinScalar<0, X86_s32>,
    LegalizeQueryGuardedCall<BuilderLegalIf, MergeWidths, []>,
  ];
}

def X86_UNMERGE_VALUES : LegalizeRuleSet<["G_UNMERGE_VALUES"]> {
  let Rules = [
    WidenScalarToNextPow2<0, 8>,
    WidenScalarToNextPow2<1, 16>,
    MinScalar<0, X86_s8>,
    MinScalar<1, X86_s32>,
    LegalizeQueryGuardedCall<BuilderLegalIf, UnmergeWidths, []>,
  ];
}

def X86_UMIN : LegalizeRuleSet<["G_UMIN", "G_UMAX", "G_SMIN", "G_SMAX"]> {
  let Rules = [
    WidenScalarToNextPow2<0, 32>,
    Lower,
  ];
}

def X86_ADD : LegalizeRuleSet<["G_ADD", "G_SUB"]> {
  let Comments = ["integer addition/subtraction"];
  let Rules = [
    LegalFor<[X86_s8, X86_s16, X86_s32]>,
    LegalForIf<Is64Bit, [X86_s64]>,
    LegalForIf<HasSSE2, [X86_v16s8, X86_v8s16, X86_v4s32, X86_v2s64]>,
    LegalForIf<HasAVX2, [X86_v32s8, X86_v16s16, X86_v8s32, X86_v4s64]>,
    LegalForIf<HasAVX512, [X86_v16s32, X86_v8s64]>,
    LegalForIf<HasBWI, [X86_v64s8, X86_v32s16]>,
    ClampMinNumElements<0, X86_s8, "16">,
    ClampMinNumElements<0, X86_s16, "8">,
    ClampMinNumElements<0, X86_s32, "4">,
    ClampMinNumElements<0, X86_s64, "2">,
    ClampMaxNumElements<0, X86_s8, "HasBWI ? 64 : (HasAVX2 ? 32 : 16)">,
    ClampMaxNumElements<0, X86_s16, "HasBWI ? 32 : (HasAVX2 ? 16 : 8)">,
    ClampMaxNumElements<0, X86_s32,
                                "HasAVX512 ? 16 : (HasAVX2 ? 8 : 4)">,
    ClampMaxNumElements<0, X86_s64,
                                "HasAVX512 ? 8 : (HasAVX2 ? 4 : 2)">,
    WidenScalarToNextPow2<0, 32>,
    ClampScalar<0, X86_s8, X86_sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_UADDE : LegalizeRuleSet<["G_UADDE", "G_UADDO", "G_USUBE", "G_USUBO"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<X86_s8, X86_s8>, LegalizeTypePair<X86_s16, X86_s8>, LegalizeTypePair<X86_s32, X86_s8>]>,
    LegalForPairsIf<Is64Bit, [LegalizeTypePair<X86_s64, X86_s8>]>,
    WidenScalarToNextPow2<0, 32>,
    ClampScalar<0, X86_s8, X86_sMaxScalar>,
    ClampScalar<1, X86_s8, X86_s8>,
    Scalarize<0>,
  ];
}

def X86_MUL : LegalizeRuleSet<["G_MUL"]> {
  let Comments = ["integer multiply"];
  let Rules = [
    LegalFor<[X86_s8, X86_s16, X86_s32]>,
    LegalForIf<Is64Bit, [X86_s64]>,
    LegalForIf<HasSSE2, [X86_v8s16]>,
    LegalForIf<HasSSE41, [X86_v4s32]>,
    LegalForIf<HasAVX2, [X86_v16s16, X86_v8s32]>,
    LegalForIf<HasAVX512, [X86_v16s32]>,
    LegalForIf<HasDQI, [X86_v8s64]>,
    LegalForIf<DQIAndVLX, [X86_v2s64, X86_v4s64]>,
    LegalForIf<HasBWI, [X86_v32s16]>,
    ClampMinNumElements<0, X86_s16, "8">,
    ClampMinNumElements<0, X86_s32, "4">,
    ClampMinNumElements<0, X86_s64, "HasVLX ? 2 : 8">,
    ClampMaxNumElements<0, X86_s16, "HasBWI ? 32 : (HasAVX2 ? 16 : 8)">,
    ClampMaxNumElements<0, X86_s32,
                                "HasAVX512 ? 16 : (HasAVX2 ? 8 : 4)">,
    ClampMaxNumElements<0, X86_s64, "8">,
    WidenScalarToNextPow2<0, 32>,
    ClampScalar<0, X86_s8, X86_sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_SMULH : LegalizeRuleSet<["G_SMULH", "G_UMULH"]> {
  let Rules = [
    LegalFor<[X86_s8, X86_s16, X86_s32]>,
    LegalForIf<Is64Bit, [X86_s64]>,
    WidenScalarToNextPow2<0, 32>,
    ClampScalar<0, X86_s8, X86_sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_SDIV : LegalizeRuleSet<["G_SDIV", "G_SREM", "G_UDIV", "G_UREM"]> {
  let Comments = ["integer divisions"];
  let Rules = [
    LegalFor<[X86_s8, X86_s16, X86_s32]>,
    LegalForIf<Is64Bit, [X86_s64]>,
    LibcallFor<[X86_s64]>,
    ClampScalar<0, X86_s8, X86_sMaxScalar>,
  ];
}

def X86_SHL : LegalizeRuleSet<["G_SHL", "G_LSHR", "G_ASHR"]> {
  let Comments = ["integer shifts"];
  let Rules = [
    LegalForPairs<[LegalizeTypePair<X86_s8, X86_s8>, LegalizeTypePair<X86_s16, X86_s8>, LegalizeTypePair<X86_s32, X86_s8>]>,
    LegalForPairsIf<Is64Bit, [LegalizeTypePair<X86_s64, X86_s8>]>,
    ClampScalar<0, X86_s8, X86_sMaxScalar>,
    ClampScalar<1, X86_s8, X86_s8>,
  ];
}

def X86_AND : LegalizeRuleSet<["G_AND", "G_OR", "G_XOR"]> {
  let Comments = ["integer logic"];
  let Rules = [
    LegalFor<[X86_s8, X86_s16, X86_s32]>,
    LegalForIf<Is64Bit, [X86_s64]>,
    LegalForIf<HasSSE2, [X86_v16s8, X86_v8s16, X86_v4s32, X86_v2s64]>,
    LegalForIf<HasAVX, [X86_v32s8, X86_v16s16, X86_v8s32, X86_v4s64]>,
    LegalForIf<HasAVX512, [X86_v64s8, X86_v32s16, X86_v16s32, X86_v8s64]>,
    ClampNumElements<0, X86_v16s8, X86_s8MaxVector>,
    ClampNumElements<0, X86_v8s16, X86_s16MaxVector>,
    ClampNumElements<0, X86_v4s32, X86_s32MaxVector>,
    ClampNumElements<0, X86_v2s64, X86_s64MaxVector>,
    WidenScalarToNextPow2<0, 32>,
    ClampScalar<0, X86_s8, X86_sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_ICMP : LegalizeRuleSet<["G_ICMP"]> {
  let Rules = [
    LegalForCartesianProduct<
        LegalizeInlineTypeList<[X86_s8]>,
        LegalizeTypeListChoice<"Is64Bit ? X86_IntTypes64 : X86_IntTypes32">>,
    ClampScalar<0, X86_s8, X86_s8>,
    WidenScalarToNextPow2<1, 8>,
    ClampScalar<1, X86_s8, X86_sMaxScalar>,
  ];
}

def X86_BSWAP : LegalizeRuleSet<["G_BSWAP"]> {
  let Comments = ["bswap"];
  let Rules = [
    LegalFor<[X86_s32]>,
    LegalForIf<Is64Bit, [X86_s64]>,
    WidenScalarToNextPow2<0, 32>,
    ClampScalar<0, X86_s32, X86_sMaxScalar>,
  ];
}

def X86_CTPOP : LegalizeRuleSet<["G_CTPOP"]> {
  let Comments = ["popcount"];
  let Rules = [
    LegalForPairsIf<HasPOPCNT, [LegalizeTypePair<X86_s16, X86_s16>, LegalizeTypePair<X86_s32, X86_s32>]>,
    LegalForPairsIf<POPCNTAnd64Bit, [LegalizeTypePair<X86_s64, X86_s64>]>,
    WidenScalarToNextPow2<1, 16>,
    ClampScalar<1, X86_s16, X86_sMaxScalar>,
    ScalarSameSizeAs<0, 1>,
  ];
}

def X86_CTLZ : LegalizeRuleSet<["G_CTLZ"]> {
  let Comments = ["count leading zeros (LZCNT)"];
  let Rules = [
    LegalForPairsIf<HasLZCNT, [LegalizeTypePair<X86_s16, X86_s16>, LegalizeTypePair<X86_s32, X86_s32>]>,
    LegalForPairsIf<LZCNTAnd64Bit, [LegalizeTypePair<X86_s64, X86_s64>]>,
    WidenScalarToNextPow2<1, 16>,
    ClampScalar<1, X86_s16, X86_sMaxScalar>,
    ScalarSameSizeAs<0, 1>,
  ];
}

def X86_CTTZ_ZERO_POISON : LegalizeRuleSet<["G_CTTZ_ZERO_POISON"]> {
  let Comments = ["count trailing zeros"];
  let Rules = [
    LegalForPairs<[LegalizeTypePair<X86_s16, X86_s16>, LegalizeTypePair<X86_s32, X86_s32>]>,
    LegalForPairsIf<Is64Bit, [LegalizeTypePair<X86_s64, X86_s64>]>,
    WidenScalarToNextPow2<1, 16>,
    ClampScalar<1, X86_s16, X86_sMaxScalar>,
    ScalarSameSizeAs<0, 1>,
  ];
}

def X86_CTTZ : LegalizeRuleSet<["G_CTTZ"]> {
  let Rules = [
    LegalForPairsIf<HasBMI, [LegalizeTypePair<X86_s16, X86_s16>, LegalizeTypePair<X86_s32, X86_s32>]>,
    LegalForPairsIf<BMIAnd64Bit, [LegalizeTypePair<X86_s64, X86_s64>]>,
    WidenScalarToNextPow2<1, 16>,
    ClampScalar<1, X86_s16, X86_sMaxScalar>,
    ScalarSameSizeAs<0, 1>,
  ];
}

def X86_BR : LegalizeRuleSet<["G_BR"]> {
  let Rules = [
    AlwaysLegal,
  ];
}

def X86_BRCOND : LegalizeRuleSet<["G_BRCOND"]> {
  let Rules = [
    LegalFor<[X86_s1]>,
  ];
}

def X86_PTRTOINT : LegalizeRuleSet<["G_PTRTOINT"]> {
  let Rules = [
    LegalForCartesianProduct<
        LegalizeTypeListChoice<"Is64Bit ? X86_PtrTypes64 : X86_PtrTypes32">,
        LegalizeInlineTypeList<[X86_p0]>>,
    MaxScalar<0, X86_sMaxScalar>,
    WidenScalarToNextPow2<0, 8>,
  ];
}

def X86_INTTOPTR : LegalizeRuleSet<["G_INTTOPTR"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<X86_p0, X86_sMaxScalar>]>,
  ];
}

def X86_CONSTANT_POOL : LegalizeRuleSet<["G_CONSTANT_POOL"]> {
  let Rules = [
    LegalFor<[X86_p0]>,
  ];
}

def X86_PTR_ADD : LegalizeRuleSet<["G_PTR_ADD"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<X86_p0, X86_s32>]>,
    LegalForPairsIf<Is64Bit, [LegalizeTypePair<X86_p0, X86_s64>]>,
    WidenScalarToNextPow2<1, 32>,
    ClampScalar<1, X86_s32, X86_sMaxScalar>,
  ];
}

def X86_FRAME_INDEX : LegalizeRuleSet<["G_FRAME_INDEX"]> {
  let Rules = [
    LegalFor<[X86_p0]>,
  ];
}

def X86_GLOBAL_VALUE : LegalizeRuleSet<["G_GLOBAL_VALUE"]> {
  let Rules = [
    CustomFor<[X86_p0]>,
  ];
}

// The load/store family is one shape with one disagreement.  Both members
// accept the type/shape pairs below; what differs is only the tail:
//
//   LOAD   additionally legalizes extending loads from a GPR-sized memory type
//   STORE  rejects a value whose type differs from the memory type
//
// Writing the common part once is not tidiness.  As two literal copies the six
// `LegalForMemDesc*` entries below had to be kept in step by hand, and a type
// missing from one list does not fail to compile: it silently legalizes worse
// on one subtarget.  Here the disagreement is the parameter and nothing else
// can drift.
multiclass X86_MemOp<string Op, list<string> Note, list<LegalizeRule> Tail> {
  def NAME : LegalizeRuleSet<[Op]> {
    let Comments = Note;
    let Rules = [
      LegalForMemDesc<[LegalizeMemDesc<X86_s8, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s16, X86_p0, X86_s16, 1>,
                       LegalizeMemDesc<X86_s32, X86_p0, X86_s32, 1>, LegalizeMemDesc<X86_s80, X86_p0, X86_s80, 1>,
                       LegalizeMemDesc<X86_p0, X86_p0, X86_p0, 1>, LegalizeMemDesc<X86_v4s8, X86_p0, X86_v4s8, 1>]>,
      LegalForMemDescIf<Is64Bit,
                        [LegalizeMemDesc<X86_s64, X86_p0, X86_s64, 1>, LegalizeMemDesc<X86_v2s32, X86_p0, X86_v2s32, 1>]>,
      LegalForMemDescIf<HasSSE1, [LegalizeMemDesc<X86_v4s32, X86_p0, X86_v4s32, 1>]>,
      LegalForMemDescIf<HasSSE2,
                        [LegalizeMemDesc<X86_v16s8, X86_p0, X86_v16s8, 1>, LegalizeMemDesc<X86_v8s16, X86_p0, X86_v8s16, 1>,
                         LegalizeMemDesc<X86_v2s64, X86_p0, X86_v2s64, 1>, LegalizeMemDesc<X86_v2p0, X86_p0, X86_v2p0, 1>]>,
      LegalForMemDescIf<HasAVX,
                        [LegalizeMemDesc<X86_v32s8, X86_p0, X86_v32s8, 1>, LegalizeMemDesc<X86_v16s16, X86_p0, X86_v16s16, 1>,
                         LegalizeMemDesc<X86_v8s32, X86_p0, X86_v8s32, 1>, LegalizeMemDesc<X86_v4s64, X86_p0, X86_v4s64, 1>,
                         LegalizeMemDesc<X86_v4p0, X86_p0, X86_v4p0, 1>]>,
      LegalForMemDescIf<HasAVX512,
                        [LegalizeMemDesc<X86_v64s8, X86_p0, X86_v64s8, 1>, LegalizeMemDesc<X86_v32s16, X86_p0, X86_v32s16, 1>,
                         LegalizeMemDesc<X86_v16s32, X86_p0, X86_v16s32, 1>, LegalizeMemDesc<X86_v8s64, X86_p0, X86_v8s64, 1>]>
    ] # Tail;
  }
}

defm X86_LOAD : X86_MemOp<"G_LOAD", ["load/store: add more corner cases"], [
  // X86 supports extending loads but not stores for GPRs.
  LegalForMemDesc<[LegalizeMemDesc<X86_s8, X86_p0, X86_s1, 1>, LegalizeMemDesc<X86_s16, X86_p0, X86_s8, 1>,
                   LegalizeMemDesc<X86_s32, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s32, X86_p0, X86_s16, 1>]>,
  LegalForMemDescIf<Is64Bit,
                    [LegalizeMemDesc<X86_s64, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s64, X86_p0, X86_s16, 1>,
                     LegalizeMemDesc<X86_s64, X86_p0, X86_s32, 1>]>,
  WidenScalarToNextPow2<0, 8>,
  ClampScalar<0, X86_s8, X86_sMaxScalar>,
  Scalarize<0>,
]>;

defm X86_STORE : X86_MemOp<"G_STORE", [], [
  // A store whose value type differs from the memory type is not legalized
  // here; the type has to match exactly.
  LegalizeOpaqueRule<BuilderCustomIf, [{

    [=](const LegalityQuery &Query) {
      return Query.Types[0] != Query.MMODescrs[0].MemoryTy;
    }
  }]>,
  WidenScalarToNextPow2<0, 8>,
  ClampScalar<0, X86_s8, X86_sMaxScalar>,
  Scalarize<0>,
]>;


// Both extending loads accept exactly the same (type, memory type) pairs; the
// hand-written chain built them in one `for (Op : {G_SEXTLOAD, G_ZEXTLOAD})`,
// so this body existed once.  Sharing it keeps it that way.
defvar X86_ExtLoadRules = [
  LegalForMemDesc<[LegalizeMemDesc<X86_s16, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s32, X86_p0, X86_s8, 1>,
                   LegalizeMemDesc<X86_s32, X86_p0, X86_s16, 1>]>,
  LegalForMemDescIf<Is64Bit,
                    [LegalizeMemDesc<X86_s64, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s64, X86_p0, X86_s16, 1>,
                     LegalizeMemDesc<X86_s64, X86_p0, X86_s32, 1>]>,
  // TODO - SSE41/AVX2/AVX512F/AVX512BW vector extensions
];

def X86_SEXTLOAD : LegalizeRuleSet<["G_SEXTLOAD"]> {
  let Rules = X86_ExtLoadRules;
}

def X86_ZEXTLOAD : LegalizeRuleSet<["G_ZEXTLOAD"]> {
  let Rules = X86_ExtLoadRules;
}

// The x87 load and store accept the same three (type, memory type) pairs; one
// `for (Op : {G_FPEXTLOAD, G_FPTRUNCSTORE})` in the former C++.
defvar X86_X87MemRules = [
  LegalForMemDescIf<UseX87,
                    [LegalizeMemDesc<X86_s80, X86_p0, X86_s32, 1>, LegalizeMemDesc<X86_s80, X86_p0, X86_s64, 1>,
                     LegalizeMemDesc<X86_s64, X86_p0, X86_s32, 1>]>,
];

def X86_FPEXTLOAD : LegalizeRuleSet<["G_FPEXTLOAD"]> {
  let Rules = X86_X87MemRules;
}

def X86_FPTRUNCSTORE : LegalizeRuleSet<["G_FPTRUNCSTORE"]> {
  let Rules = X86_X87MemRules;
}

def X86_ANYEXT : LegalizeRuleSet<["G_ANYEXT"]> {
  let Comments = ["sext, zext, and anyext"];
  let Rules = [
    LegalFor<[X86_s8, X86_s16, X86_s32, X86_s128]>,
    LegalForIf<Is64Bit, [X86_s64]>,
    WidenScalarToNextPow2<0, 8>,
    ClampScalar<0, X86_s8, X86_sMaxScalar>,
    WidenScalarToNextPow2<1, 8>,
    ClampScalar<1, X86_s8, X86_sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_SEXT : LegalizeRuleSet<["G_SEXT", "G_ZEXT"]> {
  let Rules = [
    LegalFor<[X86_s8, X86_s16, X86_s32]>,
    LegalForIf<Is64Bit, [X86_s64]>,
    WidenScalarToNextPow2<0, 8>,
    ClampScalar<0, X86_s8, X86_sMaxScalar>,
    WidenScalarToNextPow2<1, 8>,
    ClampScalar<1, X86_s8, X86_sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_TRUNC : LegalizeRuleSet<["G_TRUNC"]> {
  let Rules = [
    LegalForCartesianProduct<
        LegalizeInlineTypeList<[X86_s1, X86_s8, X86_s16, X86_s32, X86_s64]>,
        LegalizeInlineTypeList<[X86_s8, X86_s16, X86_s32, X86_s64, X86_s128]>>,
  ];
}

def X86_SEXT_INREG : LegalizeRuleSet<["G_SEXT_INREG"]> {
  let Rules = [
    Lower,
  ];
}

