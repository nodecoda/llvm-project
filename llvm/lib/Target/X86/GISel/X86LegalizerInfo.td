//===- X86LegalizerInfo.td - X86 GlobalISel legalizer -*-- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// X86 legalizer rules.
//
// Converted from the former hand-written chains in X86LegalizerInfo.cpp and
// reviewed rule by rule against them.  That baseline is no longer in the tree,
// so the comparison cannot be re-run mechanically.
//
// The LegalizeFeature records below declare the subtarget-feature booleans the
// rules are written in terms of, and the X86_* types declare the LLT constants,
// so this file is the whole legalizer: there is no hand-written prologue in the
// constructor.
//
// The type and feature names carry an X86_ prefix because
// llvm/Target/GlobalISel/Target.td already defines s32, s64, v2s64, v2s32,
// v4s32, v4s16, v8s16 and v8s8, and this file has to stay includable from a
// target .td that pulls in llvm/Target/Target.td.
//
// LegalizeOpaqueRule is the escape hatch for guards that are still raw C++.
// Three of the 309 rules need it, and each one is a guard the vocabulary cannot
// spell yet: G_LROUND returns a pair<unsigned, LLT> mutation, G_STORE compares
// Types[0] against MMODescrs[0].MemoryTy, and G_EXTRACT selects its type slots
// from Query.Opcode.  The merge/unmerge width check used to be the fourth; it
// is now a sizeIs dag, which is the same predicate the C++ wrote by hand.
//
// Four `for (Op : {...})` loops in the former C++ were hand-flattened into
// one record per opcode; the loop variables therefore appear as literals.
// `if (Cond) Action.foo(...)` became `Legalize...If<Cond, ...>`:
// the rule is always installed, but its guard is constant for a given
// subtarget, so a false guard is equivalent to the rule being absent.
//

include "llvm/Target/LegalizerInfo.td"

/// Subtarget-feature booleans the rules are written in terms of.  The record
/// name is the C++ identifier -gen-legalizer declares.
def Is64Bit : LegalizeFeature<"Subtarget.is64Bit()">;
def HasCMOV : LegalizeFeature<"Subtarget.canUseCMOV()">;
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
def UseX87NotSSE1 : LegalizePredicateDag<(all_of UseX87, (not HasSSE1))>;
def UseX87NotSSE2 : LegalizePredicateDag<(all_of UseX87, (not HasSSE2))>;
def UseX87Not64Bit : LegalizePredicateDag<(all_of UseX87, (not Is64Bit))>;
def SSE1OrX87 : LegalizePredicateDag<(any_of HasSSE1, UseX87)>;
def SSE2OrX87 : LegalizePredicateDag<(any_of HasSSE2, UseX87)>;
def DQIAndVLX : LegalizePredicateDag<(all_of HasDQI, HasVLX)>;
def POPCNTAnd64Bit : LegalizePredicateDag<(all_of HasPOPCNT, Is64Bit)>;
def LZCNTAnd64Bit : LegalizePredicateDag<(all_of HasLZCNT, Is64Bit)>;
def BMIAnd64Bit : LegalizePredicateDag<(all_of HasBMI, Is64Bit)>;
def SSE1And64Bit : LegalizePredicateDag<(all_of HasSSE1, Is64Bit)>;
def SSE2And64Bit : LegalizePredicateDag<(all_of HasSSE2, Is64Bit)>;
def NotCMOV : LegalizePredicateDag<(not HasCMOV)>;

// Leaves that read the query rather than the subtarget.  They combine with the
// cached bools above under the same operators, which is why a guard that used
// to be a raw lambda can now be an AST.
def IsS32In0 : LegalizeTypePredicate<"typeIs(0, X86_s32)">;
def IsS64In0 : LegalizeTypePredicate<"typeIs(0, X86_s64)">;
def NarrowerIn1 : LegalizeTypePredicate<"scalarNarrowerThan(1, Is64Bit ? 64 : 32)">;

// !HasAVX512 && ((HasSSE1 && typeIs(0, s32)) || (HasSSE2 && typeIs(0, s64)))
//   && scalarNarrowerThan(1, Is64Bit ? 64 : 32)
def IsS32In1 : LegalizeTypePredicate<"typeIs(1, X86_s32)">;
def IsS64In1 : LegalizeTypePredicate<"typeIs(1, X86_s64)">;
def NarrowerIn0 : LegalizeTypePredicate<"scalarNarrowerThan(0, Is64Bit ? 64 : 32)">;

// G_FPTOUI mirrors G_UITOFP with the slots swapped.
def FPResultGPRNoAVX512
    : LegalizeQueryPredicateDag<(all_of (not HasAVX512),
                                        (any_of (all_of HasSSE1, IsS32In1),
                                                (all_of HasSSE2, IsS64In1)),
                                        NarrowerIn0)>;
def FPResultGPRLower64
    : LegalizeQueryPredicateDag<(all_of (not HasAVX512),
                                        (any_of (all_of HasSSE1, IsS32In1),
                                                (all_of HasSSE2, IsS64In1)),
                                        Is64Bit, IsS64In0)>;

// G_UITOFP has the operand and result the other way round from G_FPTOUI.
def GPRLower64
    : LegalizeQueryPredicateDag<(all_of (not HasAVX512),
                                        (any_of (all_of HasSSE1, IsS32In0),
                                                (all_of HasSSE2, IsS64In0)),
                                        Is64Bit, IsS64In1)>;

// The merge/unmerge legality check is "does the total width fit a register",
// asked of two slots.  LegalityPredicates::sizeIs compares getSizeInBits(), so
// a v4s32 and an s128 are the same answer here.  Written as a class because
// each slot needs the same six widths and spelling them out twice invites the
// two lists to drift.
class SizeIsIn<int slot, int width>
    : LegalizeTypePredicate<"sizeIs(" # !cast<string>(slot) # ", " #
                            !cast<string>(width) # ")">;

def V4S32 : LegalizeTypePredicate<"typeInSet(0, {X86_v4s32})">;
def V2S64V8S16V16S8
    : LegalizeTypePredicate<"typeInSet(0, {X86_v2s64, X86_v8s16, X86_v16s8})">;
def V4S64V8S32V16S16V32S8
    : LegalizeTypePredicate<
          "typeInSet(0, {X86_v4s64, X86_v8s32, X86_v16s16, X86_v32s8})">;
def V8S64V16S32V32S16V64S8
    : LegalizeTypePredicate<
          "typeInSet(0, {X86_v8s64, X86_v16s32, X86_v32s16, X86_v64s8})">;

// G_BUILD_VECTOR is custom when the destination vector type is one the target
// can build directly, and which types those are depends on the features.
def BuildVectorCustom
    : LegalizeQueryPredicateDag<(any_of (all_of HasSSE1, V4S32),
                                        (all_of HasSSE2, V2S64V8S16V16S8),
                                        (all_of HasAVX, V4S64V8S32V16S16V32S8),
                                        (all_of HasAVX512,
                                                V8S64V16S32V32S16V64S8))>;

// G_MERGE_VALUES puts the wide value in slot 0 and the part in slot 1;
// G_UNMERGE_VALUES is the mirror.  The two switch statements in the former
// C++ disagreed in exactly one place -- slot 1 of merge allowed 8 but slot 0
// of unmerge is 16 -- so the two lists are kept separate rather than shared.
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

def GPRNarrowNoAVX512
    : LegalizeQueryPredicateDag<(all_of (not HasAVX512),
                                    (any_of (all_of HasSSE1, IsS32In0),
                                            (all_of HasSSE2, IsS64In0)),
                                    NarrowerIn1)>;

// The LLT constants the rules are written in terms of.  The def name is the
// C++ identifier, exactly like the LegalizeFeature records above.
//
// TODO: AVX512 bool vector predicate types.

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

def X86_LROUND : LegalizeRuleSet<["G_LROUND", "G_LLROUND"]> {
  let Rules = [
    LegalizeOpaqueRule<BuilderWidenScalarIf, [{

      typeIs(1, X86_s16),
      [=](const LegalityQuery &) {
        return std::pair<unsigned, LLT>(1, X86_s32);
      }
    }]>,
    Libcall,
  ];
}

def X86_FCOS : LegalizeRuleSet<[
  "G_FCOS", "G_FCOSH", "G_FACOS", "G_FSIN", "G_FSINH", "G_FASIN", "G_FTAN",
  "G_FTANH", "G_FATAN", "G_FATAN2", "G_FPOW", "G_FEXP", "G_FEXP2", "G_FEXP10",
  "G_FLOG", "G_FLOG2", "G_FLOG10", "G_FPOWI", "G_FSINCOS", "G_FCEIL", "G_FFLOOR"
]> {
  let Rules = [
    Libcall,
  ];
}

def X86_FNEG : LegalizeRuleSet<["G_FNEG"]> {
  let Rules = [
    LegalForIf<UseX87NotSSE1, [X86_s32]>,
    LegalForIf<UseX87NotSSE2, [X86_s64]>,
    LegalForIf<UseX87, [X86_s80]>,
    Lower,
  ];
}

def X86_FSQRT : LegalizeRuleSet<["G_FSQRT"]> {
  let Rules = [
    LegalForIf<SSE1OrX87, [X86_s32]>,
    LegalForIf<SSE2OrX87, [X86_s64]>,
    LegalForIf<UseX87, [X86_s80]>,
  ];
}

def X86_GET_ROUNDING : LegalizeRuleSet<["G_GET_ROUNDING", "G_SET_ROUNDING"]> {
  let Rules = [
    CustomFor<[X86_s32]>,
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


def X86_SEXTLOAD : LegalizeRuleSet<["G_SEXTLOAD"]> {
  let Rules = [
    LegalForMemDesc<[LegalizeMemDesc<X86_s16, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s32, X86_p0, X86_s8, 1>,
                            LegalizeMemDesc<X86_s32, X86_p0, X86_s16, 1>]>,
    LegalForMemDescIf<Is64Bit,
                              [LegalizeMemDesc<X86_s64, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s64, X86_p0, X86_s16, 1>,
                               LegalizeMemDesc<X86_s64, X86_p0, X86_s32, 1>]>,
  ];
}

def X86_ZEXTLOAD : LegalizeRuleSet<["G_ZEXTLOAD"]> {
  let Rules = [
    LegalForMemDesc<[LegalizeMemDesc<X86_s16, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s32, X86_p0, X86_s8, 1>,
                            LegalizeMemDesc<X86_s32, X86_p0, X86_s16, 1>]>,
    LegalForMemDescIf<Is64Bit,
                              [LegalizeMemDesc<X86_s64, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s64, X86_p0, X86_s16, 1>,
                               LegalizeMemDesc<X86_s64, X86_p0, X86_s32, 1>]>,
  ];
}

def X86_FPEXTLOAD : LegalizeRuleSet<["G_FPEXTLOAD"]> {
  let Rules = [
    LegalForMemDescIf<UseX87,
                              [LegalizeMemDesc<X86_s80, X86_p0, X86_s32, 1>, LegalizeMemDesc<X86_s80, X86_p0, X86_s64, 1>,
                               LegalizeMemDesc<X86_s64, X86_p0, X86_s32, 1>]>,
  ];
}

def X86_FPTRUNCSTORE : LegalizeRuleSet<["G_FPTRUNCSTORE"]> {
  let Rules = [
    LegalForMemDescIf<UseX87,
                              [LegalizeMemDesc<X86_s80, X86_p0, X86_s32, 1>, LegalizeMemDesc<X86_s80, X86_p0, X86_s64, 1>,
                               LegalizeMemDesc<X86_s64, X86_p0, X86_s32, 1>]>,
  ];
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

def X86_FCONSTANT : LegalizeRuleSet<["G_FCONSTANT"]> {
  let Comments = ["fp constants"];
  let Rules = [
    LegalFor<[X86_s32, X86_s64]>,
    LegalForIf<UseX87, [X86_s80]>,
  ];
}

def X86_FADD : LegalizeRuleSet<["G_FADD", "G_FSUB", "G_FMUL", "G_FDIV"]> {
  let Comments = ["fp arithmetic"];
  let Rules = [
    LegalFor<[X86_s32, X86_s64]>,
    LegalForIf<HasSSE1, [X86_v4s32]>,
    LegalForIf<HasSSE2, [X86_v2s64]>,
    LegalForIf<HasAVX, [X86_v8s32, X86_v4s64]>,
    LegalForIf<HasAVX512, [X86_v16s32, X86_v8s64]>,
    LegalForIf<UseX87, [X86_s80]>,
  ];
}

def X86_FABS : LegalizeRuleSet<["G_FABS"]> {
  let Rules = [
    LegalForIf<UseX87, [X86_s80]>,
    LegalForIf<UseX87Not64Bit, [X86_s64]>,
    Lower,
  ];
}

def X86_FCMP : LegalizeRuleSet<["G_FCMP"]> {
  let Comments = ["fp comparison"];
  let Rules = [
    LegalForIf<SSE1OrX87, [X86_s8, X86_s32]>,
    LegalForIf<SSE2OrX87, [X86_s8, X86_s64]>,
    LegalForIf<UseX87, [X86_s8, X86_s80]>,
    ClampScalar<0, X86_s8, X86_s8>,
    ClampScalar<1, X86_s32, LegalizeInlineType<"HasSSE2 ? X86_s64 : X86_s32">>,
    WidenScalarToNextPow2<1>,
  ];
}

def X86_FPEXT : LegalizeRuleSet<["G_FPEXT"]> {
  let Comments = ["fp conversions"];
  let Rules = [
    LegalForPairsIf<HasSSE2, [LegalizeTypePair<X86_s64, X86_s32>]>,
    LegalForPairsIf<HasAVX, [LegalizeTypePair<X86_v4s64, X86_v4s32>]>,
    LegalForPairsIf<HasAVX512, [LegalizeTypePair<X86_v8s64, X86_v8s32>]>,
    LowerForPairsIf<UseX87, [LegalizeTypePair<X86_s64, X86_s32>, LegalizeTypePair<X86_s80, X86_s32>, LegalizeTypePair<X86_s80, X86_s64>]>,
    Libcall,
  ];
}

def X86_FPTRUNC : LegalizeRuleSet<["G_FPTRUNC"]> {
  let Rules = [
    LegalForPairsIf<HasSSE2, [LegalizeTypePair<X86_s32, X86_s64>]>,
    LegalForPairsIf<HasAVX, [LegalizeTypePair<X86_v4s32, X86_v4s64>]>,
    LegalForPairsIf<HasAVX512, [LegalizeTypePair<X86_v8s32, X86_v8s64>]>,
    LowerForPairsIf<UseX87, [LegalizeTypePair<X86_s32, X86_s64>, LegalizeTypePair<X86_s32, X86_s80>, LegalizeTypePair<X86_s64, X86_s80>]>,
  ];
}

def X86_SITOFP : LegalizeRuleSet<["G_SITOFP"]> {
  let Rules = [
    LegalForPairsIf<HasSSE1, [LegalizeTypePair<X86_s32, X86_s32>]>,
    LegalForPairsIf<SSE1And64Bit, [LegalizeTypePair<X86_s32, X86_s64>]>,
    LegalForPairsIf<HasSSE2, [LegalizeTypePair<X86_s64, X86_s32>]>,
    LegalForPairsIf<SSE2And64Bit, [LegalizeTypePair<X86_s64, X86_s64>]>,
    ClampScalar<1, LegalizeInlineType<"(UseX87 && !HasSSE1) ? X86_s16 : X86_s32">, X86_sMaxScalar>,
    WidenScalarToNextPow2<1>,
    CustomForCartesianProductIf<UseX87,
                                LegalizeInlineTypeList<[X86_s32, X86_s64, X86_s80]>,
                                LegalizeInlineTypeList<[X86_s16, X86_s32, X86_s64]>>,
    ClampScalar<0, X86_s32, LegalizeInlineType<"HasSSE2 ? X86_s64 : X86_s32">>,
    WidenScalarToNextPow2<0>,
  ];
}

def X86_FPTOSI : LegalizeRuleSet<["G_FPTOSI"]> {
  let Rules = [
    LegalForPairsIf<HasSSE1, [LegalizeTypePair<X86_s32, X86_s32>]>,
    LegalForPairsIf<SSE1And64Bit, [LegalizeTypePair<X86_s64, X86_s32>]>,
    LegalForPairsIf<HasSSE2, [LegalizeTypePair<X86_s32, X86_s64>]>,
    LegalForPairsIf<SSE2And64Bit, [LegalizeTypePair<X86_s64, X86_s64>]>,
    ClampScalar<0, LegalizeInlineType<"(UseX87 && !HasSSE1) ? X86_s16 : X86_s32">, X86_sMaxScalar>,
    WidenScalarToNextPow2<0>,
    CustomForCartesianProductIf<UseX87,
                                LegalizeInlineTypeList<[X86_s16, X86_s32, X86_s64]>,
                                LegalizeInlineTypeList<[X86_s32, X86_s64, X86_s80]>>,
    ClampScalar<1, X86_s32, LegalizeInlineType<"HasSSE2 ? X86_s64 : X86_s32">>,
    WidenScalarToNextPow2<1>,
  ];
}

def X86_UITOFP : LegalizeRuleSet<["G_UITOFP"]> {
  let Comments = [
    "For G_UITOFP and G_FPTOUI without AVX512, we have to custom legalize",
    "types",
    "<= s32 manually. Otherwise, in custom handler there is no way to",
    "understand whether s32 is an original type and we need to promote it to",
    "s64 or s32 is obtained after widening and we shouldn't widen it to s64.",
    "",
    "For AVX512 we simply widen types as there is direct mapping from opcodes",
    "to asm instructions.",
  ];
  let Rules = [
    LegalForPairsIf<HasAVX512,
                     [LegalizeTypePair<X86_s32, X86_s32>, LegalizeTypePair<X86_s32, X86_s64>, LegalizeTypePair<X86_s64, X86_s32>, LegalizeTypePair<X86_s64, X86_s64>]>,
    LegalizeQueryGuardedCall<BuilderCustomIf, GPRNarrowNoAVX512, []>,
    LegalizeQueryGuardedCall<BuilderLowerIf, GPRLower64, []>,
    ClampScalar<0, X86_s32, LegalizeInlineType<"HasSSE2 ? X86_s64 : X86_s32">>,
    WidenScalarToNextPow2<0>,
    ClampScalar<1, X86_s32, X86_sMaxScalar>,
    WidenScalarToNextPow2<1>,
  ];
}

def X86_FPTOUI : LegalizeRuleSet<["G_FPTOUI"]> {
  let Comments = [
    "TODO: replace with customized legalization using",
    "specifics of cvttsd2si. The selection of this node requires",
    "a vector type. Either G_SCALAR_TO_VECTOR is needed or more advanced",
    "support of G_BUILD_VECTOR/G_INSERT_VECTOR_ELT is required beforehand.",
  ];
  let Rules = [
    LegalForPairsIf<HasAVX512,
                     [LegalizeTypePair<X86_s32, X86_s32>, LegalizeTypePair<X86_s32, X86_s64>, LegalizeTypePair<X86_s64, X86_s32>, LegalizeTypePair<X86_s64, X86_s64>]>,
    LegalizeQueryGuardedCall<BuilderCustomIf, FPResultGPRNoAVX512, []>,
    LegalizeQueryGuardedCall<BuilderLowerIf, FPResultGPRLower64, []>,
    ClampScalar<0, X86_s32, X86_sMaxScalar>,
    WidenScalarToNextPow2<0>,
    ClampScalar<1, X86_s32, LegalizeInlineType<"HasSSE2 ? X86_s64 : X86_s32">>,
    WidenScalarToNextPow2<1>,
  ];
}

def X86_BUILD_VECTOR : LegalizeRuleSet<["G_BUILD_VECTOR"]> {
  let Comments = ["vector ops"];
  let Rules = [
    LegalizeQueryGuardedCall<BuilderCustomIf, BuildVectorCustom, []>,
    ClampNumElements<0, X86_v16s8, X86_s8MaxVector>,
    ClampNumElements<0, X86_v8s16, X86_s16MaxVector>,
    ClampNumElements<0, X86_v4s32, X86_s32MaxVector>,
    ClampNumElements<0, X86_v2s64, X86_s64MaxVector>,
    MoreElementsToNextPow2<0>,
  ];
}

def X86_EXTRACT : LegalizeRuleSet<["G_EXTRACT", "G_INSERT"]> {
  let Rules = [
    LegalizeOpaqueRule<BuilderLegalIf, [{

      [=](const LegalityQuery &Query) {
              unsigned SubIdx = Query.Opcode == G_EXTRACT ? 0 : 1;
              unsigned FullIdx = Query.Opcode == G_EXTRACT ? 1 : 0;
              return (HasAVX && typePairInSet(SubIdx, FullIdx,
                                              {{X86_v16s8, X86_v32s8},
                                               {X86_v8s16, X86_v16s16},
                                               {X86_v4s32, X86_v8s32},
                                               {X86_v2s64, X86_v4s64}})(Query)) ||
                     (HasAVX512 && typePairInSet(SubIdx, FullIdx,
                                                 {{X86_v16s8, X86_v64s8},
                                                  {X86_v32s8, X86_v64s8},
                                                  {X86_v8s16, X86_v32s16},
                                                  {X86_v16s16, X86_v32s16},
                                                  {X86_v4s32, X86_v16s32},
                                                  {X86_v8s32, X86_v16s32},
                                                  {X86_v2s64, X86_v8s64},
                                                  {X86_v4s64, X86_v8s64}})(Query));
            }
    }]>,
  ];
}

def X86_CONCAT_VECTORS : LegalizeRuleSet<["G_CONCAT_VECTORS"]> {
  let Comments = [
    "todo: only permit dst types up to max legal vector register size?",
  ];
  let Rules = [
    LegalForPairsIf<HasSSE1,
                     [LegalizeTypePair<X86_v32s8, X86_v16s8>, LegalizeTypePair<X86_v16s16, X86_v8s16>,
                      LegalizeTypePair<X86_v8s32, X86_v4s32>, LegalizeTypePair<X86_v4s64, X86_v2s64>]>,
    LegalForPairsIf<HasAVX,
                     [LegalizeTypePair<X86_v64s8, X86_v16s8>, LegalizeTypePair<X86_v64s8, X86_v32s8>, LegalizeTypePair<X86_v32s16, X86_v8s16>,
                      LegalizeTypePair<X86_v32s16, X86_v16s16>, LegalizeTypePair<X86_v16s32, X86_v4s32>, LegalizeTypePair<X86_v16s32, X86_v8s32>,
                      LegalizeTypePair<X86_v8s64, X86_v2s64>, LegalizeTypePair<X86_v8s64, X86_v4s64>]>,
  ];
}

def X86_SELECT : LegalizeRuleSet<["G_SELECT"]> {
  let Comments = ["todo: vectors and address spaces"];
  let Rules = [
    LegalForPairs<[LegalizeTypePair<X86_s16, X86_s32>, LegalizeTypePair<X86_s32, X86_s32>, LegalizeTypePair<X86_p0, X86_s32>]>,
    LegalForPairsIf<NotCMOV, [LegalizeTypePair<X86_s8, X86_s32>]>,
    LegalForPairsIf<Is64Bit, [LegalizeTypePair<X86_s64, X86_s32>]>,
    LegalForPairsIf<UseX87, [LegalizeTypePair<X86_s80, X86_s32>]>,
    ClampScalar<1, X86_s32, X86_s32>,
    WidenScalarToNextPow2<0, 8>,
    ClampScalar<0, LegalizeInlineType<"HasCMOV ? X86_s16 : X86_s8">, X86_sMaxScalar>,
  ];
}

def X86_MEMCPY : LegalizeRuleSet<["G_MEMCPY", "G_MEMMOVE", "G_MEMSET"]> {
  let Comments = ["memory intrinsics"];
  let Rules = [
    Libcall,
  ];
}

def X86_DYN_STACKALLOC : LegalizeRuleSet<[
  "G_DYN_STACKALLOC", "G_STACKSAVE", "G_STACKRESTORE"
]> {
  let Rules = [
    Lower,
  ];
}

def X86_IS_FPCLASS : LegalizeRuleSet<["G_IS_FPCLASS"]> {
  let Comments = [
    "fp intrinsics",
    "fpclass for i686 is disabled for llvm issue #171992",
  ];
  let Rules = [
    LowerForPairsIf<Is64Bit, [LegalizeTypePair<X86_s1, X86_s32>, LegalizeTypePair<X86_s1, X86_s64>, LegalizeTypePair<X86_s1, X86_s80>]>,
  ];
}

def X86_INTRINSIC_ROUNDEVEN : LegalizeRuleSet<[
  "G_INTRINSIC_ROUNDEVEN", "G_INTRINSIC_TRUNC"
]> {
  let Rules = [
    Scalarize<0>,
    MinScalar<0, LegalizeInlineType<"LLT::scalar(32)">>,
    Libcall,
  ];
}

def X86_INTRINSIC : LegalizeRuleSet<[
  "G_INTRINSIC", "G_INTRINSIC_W_SIDE_EFFECTS"
]> {
  let Rules = [
    AlwaysLegal,
  ];
}

def X86_TRAP : LegalizeRuleSet<["G_TRAP", "G_DEBUGTRAP", "G_UBSANTRAP"]> {
  let Rules = [
    AlwaysLegal,
  ];
}

def X86_INVOKE_REGION_START : LegalizeRuleSet<["G_INVOKE_REGION_START"]> {
  let Rules = [
    AlwaysLegal,
  ];
}
