//===- AArch64LegalizerInfo.td - AArch64 GlobalISel legalizer -*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// AArch64 legalizer rules, written in the same schema as X86LegalizerInfo.td.
//
// This is a portability test for the schema: the rules are a transcription of
// AArch64LegalizerInfo.cpp, including the constructs X86 did not use
// (custom(), bitcastIf(), minScalarOrEltIf(), libcallIf(), immIdx()).
//
// Every legalize rule here is a core rule: no rule carries a C++ predicate and
// no rule carries a raw mutation, the lane clamp is two core rules (a guard
// plus a whole-type assignment), a unary type list is a named type set
// (`(CoreIn (Slot 0), Set)`), a list of type pairs or triples is a named tuple
// set (`(CoreInPairs ...)`, `(CoreInTriples ...)`), and a list of memory
// descriptors is a named memdesc set (`(CoreInMemDesc ...)`), which lower to
// `typePairInSet`, `typeTupleInSet` and `typePairAndMemDescInSet`.
//
// The only builder left is `immIdx`, which is not a rule: it marks a covered
// immediate index and the emitter gives it neither a row nor an escape.
// `-gen-legalizer-check` therefore reports `rules == core_rules + coverage`
// with `escapes == 0`, and the three `immIdx` markers are the coverage.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/LegalizerInfo.td"

def AArch64_HasFP16 : LegalizeFeature<"Subtarget.hasFullFP16()">;
def AArch64_HasCSSC : LegalizeFeature<"Subtarget.hasCSSC()">;
def AArch64_HasRCPC3 : LegalizeFeature<"Subtarget.hasRCPC3()">;
def AArch64_HasSVE : LegalizeFeature<"Subtarget.hasSVE()">;
def AArch64_UseOutlineAtomics
    : LegalizeFeature<"Subtarget.outlineAtomics() && !Subtarget.hasLSE()">;
def AArch64_SmallCodeModel
    : LegalizeFeature<"TM.getCodeModel() == CodeModel::Small">;
def AArch64_HasBF16 : LegalizeFeature<"Subtarget.hasBF16()">;
def AArch64_HasMOPS : LegalizeFeature<"Subtarget.hasMOPS()">;

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

def AArch64_p0 : LegalizeNamedType<"LLT::pointer(0, 64)">;
def AArch64_s1 : LegalizeNamedType<"LLT::scalar(1)">;
def AArch64_s8 : LegalizeNamedType<"LLT::scalar(8)">;
def AArch64_s16 : LegalizeNamedType<"LLT::scalar(16)">;
def AArch64_s32 : LegalizeNamedType<"LLT::scalar(32)">;
def AArch64_s64 : LegalizeNamedType<"LLT::scalar(64)">;
def AArch64_s128 : LegalizeNamedType<"LLT::scalar(128)">;
def AArch64_i8 : LegalizeNamedType<"LLT::integer(8)">;
def AArch64_i16 : LegalizeNamedType<"LLT::integer(16)">;
def AArch64_i32 : LegalizeNamedType<"LLT::integer(32)">;
def AArch64_i64 : LegalizeNamedType<"LLT::integer(64)">;
def AArch64_i128 : LegalizeNamedType<"LLT::integer(128)">;
def AArch64_bf16 : LegalizeNamedType<"LLT::bfloat16()">;
def AArch64_f16 : LegalizeNamedType<"LLT::float16()">;
def AArch64_f32 : LegalizeNamedType<"LLT::float32()">;
def AArch64_f64 : LegalizeNamedType<"LLT::float64()">;
def AArch64_f128 : LegalizeNamedType<"LLT::float128()">;

def AArch64_v2s8 : LegalizeNamedType<"LLT::fixed_vector(2, 8)">;
def AArch64_v4s8 : LegalizeNamedType<"LLT::fixed_vector(4, 8)">;
def AArch64_v8s8 : LegalizeNamedType<"LLT::fixed_vector(8, 8)">;
def AArch64_v16s8 : LegalizeNamedType<"LLT::fixed_vector(16, 8)">;
def AArch64_v2s16 : LegalizeNamedType<"LLT::fixed_vector(2, 16)">;
def AArch64_v4s16 : LegalizeNamedType<"LLT::fixed_vector(4, 16)">;
def AArch64_v8s16 : LegalizeNamedType<"LLT::fixed_vector(8, 16)">;
def AArch64_v2s32 : LegalizeNamedType<"LLT::fixed_vector(2, 32)">;
def AArch64_v4s32 : LegalizeNamedType<"LLT::fixed_vector(4, 32)">;
def AArch64_v2s64 : LegalizeNamedType<"LLT::fixed_vector(2, 64)">;
def AArch64_v2p0 : LegalizeNamedType<"LLT::fixed_vector(2, LLT::pointer(0, 64))">;
def AArch64_v8i8 : LegalizeNamedType<"LLT::fixed_vector(8, LLT::integer(8))">;
def AArch64_v16i8 : LegalizeNamedType<"LLT::fixed_vector(16, LLT::integer(8))">;
def AArch64_v4i16 : LegalizeNamedType<"LLT::fixed_vector(4, LLT::integer(16))">;
def AArch64_v8i16 : LegalizeNamedType<"LLT::fixed_vector(8, LLT::integer(16))">;
def AArch64_v2i32 : LegalizeNamedType<"LLT::fixed_vector(2, LLT::integer(32))">;
def AArch64_v4i32 : LegalizeNamedType<"LLT::fixed_vector(4, LLT::integer(32))">;
def AArch64_v2i64 : LegalizeNamedType<"LLT::fixed_vector(2, LLT::integer(64))">;
def AArch64_v4bf16 : LegalizeNamedType<"LLT::fixed_vector(4, LLT::bfloat16())">;
def AArch64_v8bf16 : LegalizeNamedType<"LLT::fixed_vector(8, LLT::bfloat16())">;
def AArch64_v4f16 : LegalizeNamedType<"LLT::fixed_vector(4, LLT::float16())">;
def AArch64_v8f16 : LegalizeNamedType<"LLT::fixed_vector(8, LLT::float16())">;
def AArch64_v2f32 : LegalizeNamedType<"LLT::fixed_vector(2, LLT::float32())">;
def AArch64_v4f32 : LegalizeNamedType<"LLT::fixed_vector(4, LLT::float32())">;
def AArch64_v2f64 : LegalizeNamedType<"LLT::fixed_vector(2, LLT::float64())">;
def AArch64_nxv16s8 : LegalizeNamedType<"LLT::scalable_vector(16, 8)">;
def AArch64_nxv8s16 : LegalizeNamedType<"LLT::scalable_vector(8, 16)">;
def AArch64_nxv4s32 : LegalizeNamedType<"LLT::scalable_vector(4, 32)">;
def AArch64_nxv2s64 : LegalizeNamedType<"LLT::scalable_vector(2, 64)">;
def AArch64_nxv16i8 : LegalizeNamedType<"LLT::scalable_vector(16, LLT::integer(8))">;
def AArch64_nxv8i16 : LegalizeNamedType<"LLT::scalable_vector(8, LLT::integer(16))">;
def AArch64_nxv4i32 : LegalizeNamedType<"LLT::scalable_vector(4, LLT::integer(32))">;
def AArch64_nxv2i64 : LegalizeNamedType<"LLT::scalable_vector(2, LLT::integer(64))">;

def AArch64_PackedVectorAllTypes : LegalizeNamedTypeList<
    [AArch64_v16s8, AArch64_v8s16, AArch64_v4s32, AArch64_v2s64, AArch64_v2p0,
     AArch64_v8s8, AArch64_v4s16, AArch64_v2s32]>;

/// The type sets the terminal rules need.  Each one is the list the predicate
/// it replaces spelled out; naming it lets the rule say `CoreIn` instead of a
/// `typeInSet` call inside a lambda.  A list that a rule chain still uses is
/// reused through `.Elems` rather than repeated.
def AArch64_S8To32Set : LegalizeCoreTypeSet<
    [AArch64_s8, AArch64_s16, AArch64_s32]>;
def AArch64_S16To64Set : LegalizeCoreTypeSet<
    [AArch64_s16, AArch64_s32, AArch64_s64]>;
def AArch64_S16To64OrPtrSet : LegalizeCoreTypeSet<
    [AArch64_s16, AArch64_s32, AArch64_s64, AArch64_p0]>;
def AArch64_S32Or64Set : LegalizeCoreTypeSet<
    [AArch64_s32, AArch64_s64]>;
def AArch64_S32Or64OrPtrSet : LegalizeCoreTypeSet<
    [AArch64_s32, AArch64_s64, AArch64_p0]>;
def AArch64_S32Or64Or128OrPtrSet : LegalizeCoreTypeSet<
    [AArch64_s32, AArch64_s64, AArch64_s128, AArch64_p0]>;
def AArch64_S8To128Set : LegalizeCoreTypeSet<
    [AArch64_s8, AArch64_s16, AArch64_s32, AArch64_s64, AArch64_s128]>;
def AArch64_V2s16OrV2s8Set : LegalizeCoreTypeSet<
    [AArch64_v2s16, AArch64_v2s8]>;
def AArch64_S64OrPtrSet : LegalizeCoreTypeSet<
    [AArch64_s64, AArch64_p0]>;
def AArch64_F16OrBF16Set : LegalizeCoreTypeSet<
    [AArch64_f16, AArch64_bf16]>;
def AArch64_MemS8OrS16Set : LegalizeCoreTypeSet<
    [AArch64_s8, AArch64_s16]>;
def AArch64_MemS8To32Set : LegalizeCoreTypeSet<
    [AArch64_s8, AArch64_s16, AArch64_s32]>;
def AArch64_ShuffleSrcSet : LegalizeCoreTypeSet<
    [AArch64_v8s8, AArch64_v16s8, AArch64_v4s16, AArch64_v8s16,
     AArch64_v2s32, AArch64_v4s32, AArch64_v2s64]>;
def AArch64_PackedVectorCoreSet : LegalizeCoreTypeSet<
    AArch64_PackedVectorAllTypes.Elems>;
def AArch64_ScalarAndPtrCoreSet : LegalizeCoreTypeSet<
    [AArch64_s8, AArch64_s16, AArch64_s32, AArch64_s64, AArch64_p0]>;
/// The packed vectors plus `v2s16`: what G_EXTRACT_VECTOR_ELT can index.
def AArch64_PackedVectorOrV2s16Set : LegalizeCoreTypeSet<
    [AArch64_v8s8, AArch64_v16s8, AArch64_v2s16, AArch64_v4s16,
     AArch64_v8s16, AArch64_v2s32, AArch64_v4s32, AArch64_v2s64,
     AArch64_v2p0]>;


/// The type lists the unary rules name.  A list that exists only for one rule
/// set is named after it; a list that reads as a shape of its own, or that
/// several rules share, is named for its contents.
def AArch64_ImplicitDefScalarSet : LegalizeCoreTypeSet<
    [AArch64_p0, AArch64_s8, AArch64_s16, AArch64_s32, AArch64_s64,
     AArch64_s128]>;
def AArch64_ImplicitDefVectorSet : LegalizeCoreTypeSet<
    [AArch64_v2s8, AArch64_v4s8, AArch64_v8s8, AArch64_v16s8, AArch64_v2s16,
     AArch64_v4s16, AArch64_v8s16, AArch64_v2s32, AArch64_v4s32, AArch64_v2s64,
     AArch64_v2p0]>;
def AArch64_PhiTypesSet : LegalizeCoreTypeSet<
    [AArch64_p0, AArch64_s16, AArch64_s32, AArch64_s64]>;
def AArch64_I32Or64OrIntVecSet : LegalizeCoreTypeSet<
    [AArch64_i32, AArch64_i64, AArch64_v8i8, AArch64_v16i8, AArch64_v4i16,
     AArch64_v8i16, AArch64_v2i32, AArch64_v4i32, AArch64_v2i64]>;
def AArch64_I32OrI64Set : LegalizeCoreTypeSet<[AArch64_i32, AArch64_i64]>;
def AArch64_SmulhTypesSet : LegalizeCoreTypeSet<
    [AArch64_i64, AArch64_v16i8, AArch64_v8i16, AArch64_v4i32]>;
def AArch64_IntVecNoV2i64Set : LegalizeCoreTypeSet<
    [AArch64_v8i8, AArch64_v16i8, AArch64_v4i16, AArch64_v8i16, AArch64_v2i32,
     AArch64_v4i32]>;
def AArch64_PackedVectorOrV2p0Set : LegalizeCoreTypeSet<
    [AArch64_v16i8, AArch64_v8i16, AArch64_v4i32, AArch64_v2i64, AArch64_v2p0,
     AArch64_v8i8, AArch64_v4i16, AArch64_v2i32]>;
def AArch64_BitreverseTypesSet : LegalizeCoreTypeSet<
    [AArch64_i32, AArch64_i64, AArch64_v8i8, AArch64_v16i8]>;
def AArch64_V8i8OrV16i8Set : LegalizeCoreTypeSet<[AArch64_v8i8, AArch64_v16i8]>;
def AArch64_BswapTypesSet : LegalizeCoreTypeSet<
    [AArch64_i32, AArch64_i64, AArch64_v4i16, AArch64_v8i16, AArch64_v2i32,
     AArch64_v4i32, AArch64_v2i64]>;
def AArch64_UaddsatTypesSet : LegalizeCoreTypeSet<
    [AArch64_v8i8, AArch64_v16i8, AArch64_v4i16, AArch64_v8i16, AArch64_v2i32,
     AArch64_v4i32, AArch64_v2i64]>;
def AArch64_FPScalarOrVecSet : LegalizeCoreTypeSet<
    [AArch64_f32, AArch64_f64, AArch64_v2f32, AArch64_v4f32, AArch64_v2f64]>;
def AArch64_S16To128Set : LegalizeCoreTypeSet<
    [AArch64_s16, AArch64_s32, AArch64_s64, AArch64_s128]>;
def AArch64_S32Set : LegalizeCoreTypeSet<[AArch64_s32]>;
def AArch64_PtrSet : LegalizeCoreTypeSet<[AArch64_p0]>;
def AArch64_I128Set : LegalizeCoreTypeSet<[AArch64_i128]>;
def AArch64_F128Set : LegalizeCoreTypeSet<[AArch64_f128]>;
def AArch64_F32To128Set : LegalizeCoreTypeSet<
    [AArch64_f32, AArch64_f64, AArch64_f128]>;
def AArch64_SremTypesSet : LegalizeCoreTypeSet<
    [AArch64_i8, AArch64_i16, AArch64_i32, AArch64_i64, AArch64_v2i32,
     AArch64_v4i32, AArch64_v2i64]>;
def AArch64_F16OrBF16VecSet : LegalizeCoreTypeSet<
    [AArch64_f16, AArch64_bf16, AArch64_v4f16, AArch64_v4bf16, AArch64_v8f16,
     AArch64_v8bf16]>;
def AArch64_SVEIntVecSet : LegalizeCoreTypeSet<
    [AArch64_nxv16i8, AArch64_nxv8i16, AArch64_nxv4i32, AArch64_nxv2i64]>;
def AArch64_F16OrV4V8F16Set : LegalizeCoreTypeSet<
    [AArch64_f16, AArch64_v4f16, AArch64_v8f16]>;

/// The self-bitcast lists.  `legalForCartesianProduct` with a single list
/// applies it to both slots, so each of these is the set of types a bitcast of
/// that width may have on either side.
def AArch64_Bitcast16Set : LegalizeCoreTypeSet<[AArch64_s16]>;
def AArch64_Bitcast32Set : LegalizeCoreTypeSet<
    [AArch64_s32, AArch64_v2s16, AArch64_v4s8]>;
def AArch64_Bitcast64Set : LegalizeCoreTypeSet<
    [AArch64_s64, AArch64_v8s8, AArch64_v4s16, AArch64_v2s32]>;
def AArch64_Bitcast128Set : LegalizeCoreTypeSet<
    [AArch64_s128, AArch64_v16s8, AArch64_v8s16, AArch64_v4s32, AArch64_v2s64,
     AArch64_v2p0]>;

/// The type tuples the rules name.  A pair set is a table, not a condition:
/// the rule says `(CoreInPairs (Slot 0), (Slot 1), Set)`, which lowers to
/// `typePairInSet`, so the pairs stay one datum instead of a disjunction of
/// slot comparisons.
def AArch64_SHLPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_i32>,
     LegalizeTypePair<AArch64_i32, AArch64_i64>,
     LegalizeTypePair<AArch64_i64, AArch64_i64>,
     LegalizeTypePair<AArch64_v8i8, AArch64_v8i8>,
     LegalizeTypePair<AArch64_v16i8, AArch64_v16i8>,
     LegalizeTypePair<AArch64_v4i16, AArch64_v4i16>,
     LegalizeTypePair<AArch64_v8i16, AArch64_v8i16>,
     LegalizeTypePair<AArch64_v2i32, AArch64_v2i32>,
     LegalizeTypePair<AArch64_v4i32, AArch64_v4i32>,
     LegalizeTypePair<AArch64_v2i64, AArch64_v2i64>]>;
def AArch64_PTR_ADDPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_p0, AArch64_i64>,
     LegalizeTypePair<AArch64_v2p0, AArch64_v2i64>]>;
def AArch64_PTRMASKPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_p0, AArch64_s64>]>;
def AArch64_SADDEPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_i32>,
     LegalizeTypePair<AArch64_i64, AArch64_i32>]>;
def AArch64_FSHLPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_i32>,
     LegalizeTypePair<AArch64_i32, AArch64_i64>,
     LegalizeTypePair<AArch64_i64, AArch64_i64>]>;
def AArch64_ROTRPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_i64>,
     LegalizeTypePair<AArch64_i64, AArch64_i64>]>;
def AArch64_SBFXPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_s32, AArch64_s32>,
     LegalizeTypePair<AArch64_s64, AArch64_s64>]>;
def AArch64_CTPOPPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_i32>,
     LegalizeTypePair<AArch64_i64, AArch64_i64>]>;
def AArch64_CTPOPPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_v8i8, AArch64_v8i8>,
     LegalizeTypePair<AArch64_v16i8, AArch64_v16i8>]>;
def AArch64_CTPOPPairSet3 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_s32, AArch64_s32>,
     LegalizeTypePair<AArch64_s64, AArch64_s64>]>;
def AArch64_CTPOPPairSet4 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_s128, AArch64_s128>,
     LegalizeTypePair<AArch64_v4s16, AArch64_v4s16>,
     LegalizeTypePair<AArch64_v8s16, AArch64_v8s16>,
     LegalizeTypePair<AArch64_v2s32, AArch64_v2s32>,
     LegalizeTypePair<AArch64_v4s32, AArch64_v4s32>,
     LegalizeTypePair<AArch64_v2s64, AArch64_v2s64>]>;
def AArch64_CTLZPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_i32>,
     LegalizeTypePair<AArch64_i64, AArch64_i64>,
     LegalizeTypePair<AArch64_v8i8, AArch64_v8i8>,
     LegalizeTypePair<AArch64_v16i8, AArch64_v16i8>,
     LegalizeTypePair<AArch64_v4i16, AArch64_v4i16>,
     LegalizeTypePair<AArch64_v8i16, AArch64_v8i16>,
     LegalizeTypePair<AArch64_v2i32, AArch64_v2i32>,
     LegalizeTypePair<AArch64_v4i32, AArch64_v4i32>]>;
def AArch64_FPOWIPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_f32, AArch64_i32>,
     LegalizeTypePair<AArch64_f64, AArch64_i32>,
     LegalizeTypePair<AArch64_f128, AArch64_i32>]>;
def AArch64_LROUNDPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_f32>,
     LegalizeTypePair<AArch64_i32, AArch64_f64>,
     LegalizeTypePair<AArch64_i64, AArch64_f32>,
     LegalizeTypePair<AArch64_i64, AArch64_f64>]>;
def AArch64_LROUNDPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_f16>,
     LegalizeTypePair<AArch64_i64, AArch64_f16>]>;
def AArch64_LROUNDPairSet3 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_s64, AArch64_s128>]>;
def AArch64_LLROUNDPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i64, AArch64_f32>,
     LegalizeTypePair<AArch64_i64, AArch64_f64>]>;
def AArch64_LLROUNDPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i64, AArch64_f16>]>;
def AArch64_LLROUNDPairSet3 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_s64, AArch64_s128>]>;
def AArch64_ICMPPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_i32>,
     LegalizeTypePair<AArch64_i32, AArch64_i64>,
     LegalizeTypePair<AArch64_i32, AArch64_p0>]>;
def AArch64_FCMPPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_f32>,
     LegalizeTypePair<AArch64_i32, AArch64_f64>,
     LegalizeTypePair<AArch64_v4i32, AArch64_v4f32>,
     LegalizeTypePair<AArch64_v2i32, AArch64_v2f32>,
     LegalizeTypePair<AArch64_v2i64, AArch64_v2f64>]>;
def AArch64_FCMPPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_f16>,
     LegalizeTypePair<AArch64_v4i16, AArch64_v4f16>,
     LegalizeTypePair<AArch64_v8i16, AArch64_v8f16>]>;
def AArch64_FCMPPairSet3 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_s32, AArch64_s128>]>;
def AArch64_ZEXTPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_v8s16, AArch64_v8s8>,
     LegalizeTypePair<AArch64_v4s32, AArch64_v4s16>,
     LegalizeTypePair<AArch64_v2s64, AArch64_v2s32>]>;
def AArch64_TRUNCPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_v8s8, AArch64_v8s16>,
     LegalizeTypePair<AArch64_v4s16, AArch64_v4s32>,
     LegalizeTypePair<AArch64_v2s32, AArch64_v2s64>]>;
def AArch64_TRUNC_SSAT_SPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_v8i8, AArch64_v8i16>,
     LegalizeTypePair<AArch64_v4i16, AArch64_v4i32>,
     LegalizeTypePair<AArch64_v2i32, AArch64_v2i64>]>;
def AArch64_FPTRUNCPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_f16, AArch64_f32>,
     LegalizeTypePair<AArch64_f16, AArch64_f64>,
     LegalizeTypePair<AArch64_f32, AArch64_f64>,
     LegalizeTypePair<AArch64_v4f16, AArch64_v4f32>,
     LegalizeTypePair<AArch64_v2f32, AArch64_v2f64>]>;
def AArch64_FPTRUNCPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_bf16, AArch64_f32>,
     LegalizeTypePair<AArch64_v4bf16, AArch64_v4f32>]>;
def AArch64_FPTRUNCPairSet3 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_f16, AArch64_f128>,
     LegalizeTypePair<AArch64_f32, AArch64_f128>,
     LegalizeTypePair<AArch64_f64, AArch64_f128>]>;
def AArch64_FPEXTPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_f32, AArch64_f16>,
     LegalizeTypePair<AArch64_f64, AArch64_f16>,
     LegalizeTypePair<AArch64_f32, AArch64_bf16>,
     LegalizeTypePair<AArch64_f64, AArch64_f32>,
     LegalizeTypePair<AArch64_v4f32, AArch64_v4f16>,
     LegalizeTypePair<AArch64_v4f32, AArch64_v4bf16>,
     LegalizeTypePair<AArch64_v2f64, AArch64_v2f32>]>;
def AArch64_FPEXTPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_f128, AArch64_f64>,
     LegalizeTypePair<AArch64_f128, AArch64_f32>,
     LegalizeTypePair<AArch64_f128, AArch64_f16>]>;
def AArch64_FPTOSIPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_f32>,
     LegalizeTypePair<AArch64_i64, AArch64_f32>,
     LegalizeTypePair<AArch64_i32, AArch64_f64>,
     LegalizeTypePair<AArch64_i64, AArch64_f64>,
     LegalizeTypePair<AArch64_v2i32, AArch64_v2f32>,
     LegalizeTypePair<AArch64_v4i32, AArch64_v4f32>,
     LegalizeTypePair<AArch64_v2i64, AArch64_v2f64>]>;
def AArch64_FPTOSIPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_f16>,
     LegalizeTypePair<AArch64_i64, AArch64_f16>,
     LegalizeTypePair<AArch64_v4i16, AArch64_v4f16>,
     LegalizeTypePair<AArch64_v8i16, AArch64_v8f16>]>;
def AArch64_FPTOSIPairSet3 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_f128>,
     LegalizeTypePair<AArch64_i64, AArch64_f128>,
     LegalizeTypePair<AArch64_i128, AArch64_f128>,
     LegalizeTypePair<AArch64_i128, AArch64_f32>,
     LegalizeTypePair<AArch64_i128, AArch64_f64>]>;
def AArch64_FPTOSI_SATPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i32, AArch64_f32>,
     LegalizeTypePair<AArch64_i64, AArch64_f32>,
     LegalizeTypePair<AArch64_i32, AArch64_f64>,
     LegalizeTypePair<AArch64_i64, AArch64_f64>,
     LegalizeTypePair<AArch64_v2i32, AArch64_v2f32>,
     LegalizeTypePair<AArch64_v4i32, AArch64_v4f32>,
     LegalizeTypePair<AArch64_v2i64, AArch64_v2f64>]>;
def AArch64_FPTOSI_SATPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i16, AArch64_f16>,
     LegalizeTypePair<AArch64_i32, AArch64_f16>,
     LegalizeTypePair<AArch64_i64, AArch64_f16>,
     LegalizeTypePair<AArch64_v4i16, AArch64_v4f16>,
     LegalizeTypePair<AArch64_v8i16, AArch64_v8f16>]>;
def AArch64_SITOFPPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_f32, AArch64_i32>,
     LegalizeTypePair<AArch64_f64, AArch64_i32>,
     LegalizeTypePair<AArch64_f32, AArch64_i64>,
     LegalizeTypePair<AArch64_f64, AArch64_i64>,
     LegalizeTypePair<AArch64_v2f32, AArch64_v2i32>,
     LegalizeTypePair<AArch64_v4f32, AArch64_v4i32>,
     LegalizeTypePair<AArch64_v2f64, AArch64_v2i64>]>;
def AArch64_SITOFPPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_f16, AArch64_i32>,
     LegalizeTypePair<AArch64_f16, AArch64_i64>,
     LegalizeTypePair<AArch64_v4f16, AArch64_v4i16>,
     LegalizeTypePair<AArch64_v8f16, AArch64_v8i16>]>;
def AArch64_SITOFPPairSet3 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_f16, AArch64_i128>,
     LegalizeTypePair<AArch64_f32, AArch64_i128>,
     LegalizeTypePair<AArch64_f64, AArch64_i128>,
     LegalizeTypePair<AArch64_f128, AArch64_i128>,
     LegalizeTypePair<AArch64_f128, AArch64_i32>,
     LegalizeTypePair<AArch64_f128, AArch64_i64>]>;
def AArch64_SELECTPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_s32, AArch64_s32>,
     LegalizeTypePair<AArch64_s64, AArch64_s32>,
     LegalizeTypePair<AArch64_p0, AArch64_s32>]>;
def AArch64_PTRTOINTPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i64, AArch64_p0>,
     LegalizeTypePair<AArch64_v2i64, AArch64_v2p0>]>;
def AArch64_INTTOPTRPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_p0, AArch64_i64>,
     LegalizeTypePair<AArch64_v2p0, AArch64_v2i64>]>;
def AArch64_ATOMIC_CMPXCHGPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_s32, AArch64_p0>,
     LegalizeTypePair<AArch64_s64, AArch64_p0>]>;
def AArch64_ATOMIC_CMPXCHGPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_s128, AArch64_p0>]>;
def AArch64_ATOMIC_CMPXCHGPairSet3 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_s8, AArch64_p0>,
     LegalizeTypePair<AArch64_s16, AArch64_p0>,
     LegalizeTypePair<AArch64_s32, AArch64_p0>,
     LegalizeTypePair<AArch64_s64, AArch64_p0>,
     LegalizeTypePair<AArch64_s128, AArch64_p0>]>;
def AArch64_ATOMICRMW_XCHGPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_s32, AArch64_p0>,
     LegalizeTypePair<AArch64_s64, AArch64_p0>]>;
def AArch64_ATOMICRMW_XCHGPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_s8, AArch64_p0>,
     LegalizeTypePair<AArch64_s16, AArch64_p0>,
     LegalizeTypePair<AArch64_s32, AArch64_p0>,
     LegalizeTypePair<AArch64_s64, AArch64_p0>]>;
def AArch64_EXTRACT_VECTOR_ELTTripleSet : LegalizeCoreTypeTripleSet<
    [LegalizeTypeTriple<AArch64_s16, AArch64_nxv16s8, AArch64_s64>,
     LegalizeTypeTriple<AArch64_s16, AArch64_nxv8s16, AArch64_s64>,
     LegalizeTypeTriple<AArch64_s32, AArch64_nxv4s32, AArch64_s64>,
     LegalizeTypeTriple<AArch64_s64, AArch64_nxv2s64, AArch64_s64>]>;
def AArch64_INSERT_VECTOR_ELTTripleSet : LegalizeCoreTypeTripleSet<
    [LegalizeTypeTriple<AArch64_nxv16s8, AArch64_s32, AArch64_s64>,
     LegalizeTypeTriple<AArch64_nxv8s16, AArch64_s32, AArch64_s64>,
     LegalizeTypeTriple<AArch64_nxv4s32, AArch64_s32, AArch64_s64>,
     LegalizeTypeTriple<AArch64_nxv2s64, AArch64_s64, AArch64_s64>]>;
def AArch64_BUILD_VECTORPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_v8s8, AArch64_s8>,
     LegalizeTypePair<AArch64_v16s8, AArch64_s8>,
     LegalizeTypePair<AArch64_v4s16, AArch64_s16>,
     LegalizeTypePair<AArch64_v8s16, AArch64_s16>,
     LegalizeTypePair<AArch64_v2s32, AArch64_s32>,
     LegalizeTypePair<AArch64_v4s32, AArch64_s32>,
     LegalizeTypePair<AArch64_v2s64, AArch64_s64>,
     LegalizeTypePair<AArch64_v2p0, AArch64_p0>]>;
def AArch64_SPLAT_VECTORPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_v8s8, AArch64_s8>,
     LegalizeTypePair<AArch64_v8s8, AArch64_s32>,
     LegalizeTypePair<AArch64_v16s8, AArch64_s8>,
     LegalizeTypePair<AArch64_v16s8, AArch64_s32>,
     LegalizeTypePair<AArch64_v4s16, AArch64_s16>,
     LegalizeTypePair<AArch64_v4s16, AArch64_s32>,
     LegalizeTypePair<AArch64_v8s16, AArch64_s16>,
     LegalizeTypePair<AArch64_v8s16, AArch64_s32>,
     LegalizeTypePair<AArch64_v2s32, AArch64_s32>,
     LegalizeTypePair<AArch64_v4s32, AArch64_s32>,
     LegalizeTypePair<AArch64_v2s64, AArch64_s64>,
     LegalizeTypePair<AArch64_v2p0, AArch64_p0>]>;
def AArch64_SPLAT_VECTORPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_nxv4s32, AArch64_s32>,
     LegalizeTypePair<AArch64_nxv2s64, AArch64_s64>]>;
def AArch64_CONCAT_VECTORSPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_v16s8, AArch64_v8s8>,
     LegalizeTypePair<AArch64_v8s16, AArch64_v4s16>,
     LegalizeTypePair<AArch64_v4s32, AArch64_v2s32>]>;
def AArch64_EXTRACT_SUBVECTORPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_v8s8, AArch64_v16s8>,
     LegalizeTypePair<AArch64_v4s16, AArch64_v8s16>,
     LegalizeTypePair<AArch64_v2s32, AArch64_v4s32>]>;
def AArch64_BRJTPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_p0, AArch64_s64>]>;
def AArch64_VECREDUCE_FADDPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_f32, AArch64_v2f32>,
     LegalizeTypePair<AArch64_f32, AArch64_v4f32>,
     LegalizeTypePair<AArch64_f64, AArch64_v2f64>]>;
def AArch64_VECREDUCE_FADDPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_f16, AArch64_v4f16>,
     LegalizeTypePair<AArch64_f16, AArch64_v8f16>]>;
def AArch64_VECREDUCE_ADDPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i8, AArch64_v8i8>,
     LegalizeTypePair<AArch64_i8, AArch64_v16i8>,
     LegalizeTypePair<AArch64_i16, AArch64_v4i16>,
     LegalizeTypePair<AArch64_i16, AArch64_v8i16>,
     LegalizeTypePair<AArch64_i32, AArch64_v2i32>,
     LegalizeTypePair<AArch64_i32, AArch64_v4i32>,
     LegalizeTypePair<AArch64_i64, AArch64_v2i64>]>;
def AArch64_VECREDUCE_FMINPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_f32, AArch64_v2f32>,
     LegalizeTypePair<AArch64_f32, AArch64_v4f32>,
     LegalizeTypePair<AArch64_f64, AArch64_v2f64>]>;
def AArch64_VECREDUCE_FMINPairSet2 : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_f16, AArch64_v4f16>,
     LegalizeTypePair<AArch64_f16, AArch64_v8f16>]>;
def AArch64_VECREDUCE_SMINPairSet : LegalizeCoreTypePairSet<
    [LegalizeTypePair<AArch64_i8, AArch64_v8i8>,
     LegalizeTypePair<AArch64_i8, AArch64_v16i8>,
     LegalizeTypePair<AArch64_i16, AArch64_v4i16>,
     LegalizeTypePair<AArch64_i16, AArch64_v8i16>,
     LegalizeTypePair<AArch64_i32, AArch64_v2i32>,
     LegalizeTypePair<AArch64_i32, AArch64_v4i32>]>;

/// The memory descriptors the load and store rules name.  A descriptor is a
/// second axis of the query, so the rule says `(CoreInMemDesc (Slot 0),
/// (Slot 1), (MemSlot 0), Set)`, which lowers to `typePairAndMemDescInSet`.
/// The predicate compares with `isCompatible`: the alignment is a lower bound
/// and the memory type is compared by size.
def AArch64_LOADMemDescSet : LegalizeCoreMemDescSet<
    [LegalizeMemDesc<AArch64_s8, AArch64_p0, AArch64_s8, 8>,
     LegalizeMemDesc<AArch64_s16, AArch64_p0, AArch64_s16, 8>,
     LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s32, 8>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_s128, AArch64_p0, AArch64_s128, 8>,
     LegalizeMemDesc<AArch64_v8s8, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_v16s8, AArch64_p0, AArch64_s128, 8>,
     LegalizeMemDesc<AArch64_v4s16, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_v8s16, AArch64_p0, AArch64_s128, 8>,
     LegalizeMemDesc<AArch64_v2s32, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_v4s32, AArch64_p0, AArch64_s128, 8>,
     LegalizeMemDesc<AArch64_v2s64, AArch64_p0, AArch64_s128, 8>]>;
def AArch64_LOADMemDescSet2 : LegalizeCoreMemDescSet<
    [LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s8, 8>,
     LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s16, 8>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s32, 8>]>;
def AArch64_LOADMemDescSet3 : LegalizeCoreMemDescSet<
    [LegalizeMemDesc<AArch64_nxv16s8, AArch64_p0, AArch64_nxv16s8, 8>,
     LegalizeMemDesc<AArch64_nxv8s16, AArch64_p0, AArch64_nxv8s16, 8>,
     LegalizeMemDesc<AArch64_nxv4s32, AArch64_p0, AArch64_nxv4s32, 8>,
     LegalizeMemDesc<AArch64_nxv2s64, AArch64_p0, AArch64_nxv2s64, 8>]>;
def AArch64_STOREMemDescSet : LegalizeCoreMemDescSet<
    [LegalizeMemDesc<AArch64_s8, AArch64_p0, AArch64_s8, 8>,
     LegalizeMemDesc<AArch64_s16, AArch64_p0, AArch64_s8, 8>,
     LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s8, 8>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s8, 8>,
     LegalizeMemDesc<AArch64_s16, AArch64_p0, AArch64_s16, 8>,
     LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s16, 8>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s16, 8>,
     LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s8, 8>,
     LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s16, 8>,
     LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s32, 8>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s32, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_s128, AArch64_p0, AArch64_s128, 8>,
     LegalizeMemDesc<AArch64_v16s8, AArch64_p0, AArch64_s128, 8>,
     LegalizeMemDesc<AArch64_v8s8, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_v4s16, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_v8s16, AArch64_p0, AArch64_s128, 8>,
     LegalizeMemDesc<AArch64_v2s32, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_v4s32, AArch64_p0, AArch64_s128, 8>,
     LegalizeMemDesc<AArch64_v2s64, AArch64_p0, AArch64_s128, 8>]>;
def AArch64_STOREMemDescSet2 : LegalizeCoreMemDescSet<
    [LegalizeMemDesc<AArch64_nxv16s8, AArch64_p0, AArch64_nxv16s8, 8>,
     LegalizeMemDesc<AArch64_nxv8s16, AArch64_p0, AArch64_nxv8s16, 8>,
     LegalizeMemDesc<AArch64_nxv4s32, AArch64_p0, AArch64_nxv4s32, 8>,
     LegalizeMemDesc<AArch64_nxv2s64, AArch64_p0, AArch64_nxv2s64, 8>]>;
def AArch64_INDEXED_STOREMemDescSet : LegalizeCoreMemDescSet<
    [LegalizeMemDesc<AArch64_p0, AArch64_s8, AArch64_s8, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_s16, AArch64_s16, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_s32, AArch64_s8, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_s32, AArch64_s16, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_s32, AArch64_s32, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_s64, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_p0, AArch64_p0, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_v8s8, AArch64_v8s8, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_v16s8, AArch64_v16s8, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_v4s16, AArch64_v4s16, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_v8s16, AArch64_v8s16, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_v2s32, AArch64_v2s32, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_v4s32, AArch64_v4s32, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_v2s64, AArch64_v2s64, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_v2p0, AArch64_v2p0, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_s128, AArch64_s128, 8>]>;
def AArch64_SEXTLOADMemDescSet : LegalizeCoreMemDescSet<
    [LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s8, 8>,
     LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s16, 8>,
     LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s32, 8>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s8, 2>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s16, 2>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s32, 4>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_v2s32, AArch64_p0, AArch64_s64, 8>]>;
def AArch64_ZEXTLOADMemDescSet : LegalizeCoreMemDescSet<
    [LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s8, 8>,
     LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s16, 8>,
     LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s32, 8>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s8, 2>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s16, 2>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s32, 4>,
     LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_p0, AArch64_p0, AArch64_s64, 8>,
     LegalizeMemDesc<AArch64_v2s32, AArch64_p0, AArch64_s64, 8>]>;

//===----------------------------------------------------------------------===//
// Rule sets
//===----------------------------------------------------------------------===//

def AArch64_IMPLICIT_DEF : LegalizeRuleSet<["G_IMPLICIT_DEF", "G_FREEZE", "G_CONSTANT_FOLD_BARRIER"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_ImplicitDefScalarSet)>,
    CoreLegal<(CoreIn (Slot 0), AArch64_ImplicitDefVectorSet)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 0), (Ty AArch64_s8)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreWiden<(ElemOf (Slot 0)), (MinVectorSize 64)>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_p0, 2>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>
  ];
}

def AArch64_PHI : LegalizeRuleSet<["G_PHI"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_PhiTypesSet)>,
    CoreLegal<(CoreIn (Slot 0), AArch64_PackedVectorCoreSet)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreWiden<(Slot 0), (Ty AArch64_s16)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_p0, 2>,
    CoreWidenPow2OrMinSize<(Slot 0), 8>
  ];
}

def AArch64_INSERT : LegalizeRuleSet<["G_INSERT"]> {
  let Rules = [
    CoreLegal<(CoreAll (CoreIn (Slot 0), AArch64_S32Or64OrPtrSet),
                       (CoreIn (Slot 1), AArch64_S8To32Set),
                       (CoreLt (SizeOf (Slot 1)), (SizeOf (Slot 0))))>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreWiden<(Slot 1), (Pow2)>,
    CoreWiden<(Slot 1), (Ty AArch64_s8)>,
    CoreNarrowIf<(Slot 1), (Ty AArch64_s16),
                 (CoreEq (Slot 0), (Ty AArch64_s32))>,
    CoreNarrowIf<(Slot 1), (Ty AArch64_s32),
                 (CoreIn (Slot 0), AArch64_S64OrPtrSet)>
  ];
}

def AArch64_EXTRACT : LegalizeRuleSet<["G_EXTRACT"]> {
  let Rules = [
    CoreLegal<(CoreAll (CoreIn (Slot 0), AArch64_S16To64OrPtrSet),
                       (CoreIn (Slot 1), AArch64_S32Or64Or128OrPtrSet),
                       (CoreLt (SizeOf (Slot 0)), (SizeOf (Slot 1))))>,
    CoreWiden<(Slot 1), (Pow2)>,
    CoreWiden<(Slot 1), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 1), (Ty AArch64_s128)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 0), (Ty AArch64_s16)>,
    CoreNarrowIf<(Slot 0), (Ty AArch64_s16),
                 (CoreEq (Slot 1), (Ty AArch64_s32))>,
    CoreNarrowIf<(Slot 0), (Ty AArch64_s32),
                 (CoreIn (Slot 1), AArch64_S64OrPtrSet)>,
    CoreNarrowIf<(Slot 0), (Ty AArch64_s64),
                 (CoreEq (Slot 1), (Ty AArch64_s128))>
  ];
}

def AArch64_ADD : LegalizeRuleSet<["G_ADD", "G_SUB", "G_AND", "G_OR", "G_XOR"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_I32Or64OrIntVecSet)>,
    CoreLegal<(CoreAll AArch64_HasSVE,
                       (CoreIn (Slot 0), AArch64_SVEIntVecSet))>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreWidenIf<(ElemOf (Slot 0)), (Ty AArch64_s32),
                (CoreLe (NumElems (Slot 0)), 2)>,
    CoreWidenIf<(ElemOf (Slot 0)), (Ty AArch64_s16), (CoreLe (NumElems (Slot 0)), 4)>,
    CoreWidenIf<(ElemOf (Slot 0)), (Ty AArch64_s8),
                (CoreLe (NumElems (Slot 0)), 16)>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreMore<(Slot 0), (Pow2)>
  ];
}

def AArch64_MUL : LegalizeRuleSet<["G_MUL"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_I32Or64OrIntVecSet)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreWidenIf<(ElemOf (Slot 0)), (Ty AArch64_s32),
                (CoreLe (NumElems (Slot 0)), 2)>,
    CoreWidenIf<(ElemOf (Slot 0)), (Ty AArch64_s16), (CoreLe (NumElems (Slot 0)), 4)>,
    CoreWidenIf<(ElemOf (Slot 0)), (Ty AArch64_s8),
                (CoreLe (NumElems (Slot 0)), 16)>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreMore<(Slot 0), (Pow2)>
  ];
}

def AArch64_SHL : LegalizeRuleSet<["G_SHL", "G_ASHR", "G_LSHR"]> {
  let Rules = [
    CoreCustom<(CoreAll (CoreNot (CoreEq (SortOf (Slot 0)), KindVector)),
                        (CoreEq (SizeOf (Slot 0)), 32),
                        (CoreEq (SizeOf (Slot 1)), 32))>,
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_SHLPairSet)>,
    CoreWiden<(Slot 1), (Pow2)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 1), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 1), (Ty AArch64_s64)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 1), (SameAs 0)>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreWidenIf<(Slot 1), (SameAs 0),
                (CoreEq (SortOf (Slot 0)), KindVector)>,
    CoreNarrowIf<(Slot 1), (Replace (SameAs 0)),
                 (CoreAll (CoreEq (SortOf (Slot 0)), KindVector),
                          (CoreLt (BitsOf (Slot 0)), (BitsOf (Slot 1))))>
  ];
}

def AArch64_PTR_ADD : LegalizeRuleSet<["G_PTR_ADD"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_PTR_ADDPairSet)>,
    CoreWiden<(ElemOf (Slot 1)), (Ty AArch64_s64)>,
    CoreNarrow<(ElemOf (Slot 1)), (Ty AArch64_s64)>,
    CoreClampMinNumElements<0, AArch64_p0, 2>,
    CoreClampMaxNumElements<0, AArch64_p0, 2>
  ];
}

def AArch64_PTRMASK : LegalizeRuleSet<["G_PTRMASK"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_PTRMASKPairSet)>
  ];
}

def AArch64_SDIV : LegalizeRuleSet<["G_SDIV", "G_UDIV"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_I32OrI64Set)>,
    CoreLibcall<(CoreIn (Slot 0), AArch64_I128Set)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreScalarize<(Slot 0)>
  ];
}

def AArch64_SREM : LegalizeRuleSet<["G_SREM", "G_UREM", "G_SDIVREM", "G_UDIVREM"]> {
  let Rules = [
    CoreLower<(CoreIn (Slot 0), AArch64_SremTypesSet)>,
    CoreLibcall<(CoreIn (Slot 0), AArch64_I128Set)>,
    CoreWiden<(ElemOf (Slot 0)), (Pow2)>,
    CoreWiden<(ElemOf (Slot 0)), (Ty AArch64_s32)>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreScalarize<(Slot 0)>
  ];
}

def AArch64_SMULO : LegalizeRuleSet<["G_SMULO", "G_UMULO"]> {
  let Rules = [
    CoreWiden<(Slot 0), (Pow2Min 32)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreAlwaysLower
  ];
}

def AArch64_SMULH : LegalizeRuleSet<["G_SMULH", "G_UMULH"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_SmulhTypesSet)>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreAlwaysLower
  ];
}

def AArch64_SMULFIX : LegalizeRuleSet<["G_SMULFIX", "G_UMULFIX", "G_SMULFIXSAT", "G_UMULFIXSAT"]> {
  let Rules = [
    CoreAlwaysLower
  ];
}

def AArch64_SMIN : LegalizeRuleSet<["G_SMIN", "G_SMAX", "G_UMIN", "G_UMAX"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_IntVecNoV2i64Set)>,
    CoreLegal<(CoreAll AArch64_HasCSSC,
                       (CoreIn (Slot 0), AArch64_I32OrI64Set))>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreAlwaysLower
  ];
}

def AArch64_ABS : LegalizeRuleSet<["G_ABS"]> {
  let Rules = [
    CoreLegal<(CoreAll AArch64_HasCSSC,
                       (CoreIn (Slot 0), AArch64_I32OrI64Set))>,
    CoreLegal<(CoreIn (Slot 0), AArch64_PackedVectorOrV2p0Set)>,
    CoreCustom<(CoreAll (CoreEq (SortOf (Slot 0)), KindScalar),
                        (CoreLt (SizeOf (Slot 0)), 128))>,
    CoreWidenIf<(Slot 0), (Replace (Ty AArch64_v4i16)),
                (CoreEq (Slot 0), (Ty AArch64_v4s8))>,
    CoreWidenIf<(Slot 0), (Replace (Ty AArch64_v2i32)),
                (CoreEq (Slot 0), (Ty AArch64_v2s16))>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreAlwaysLower
  ];
}

def AArch64_ABDS : LegalizeRuleSet<["G_ABDS", "G_ABDU", "G_UAVGFLOOR", "G_UAVGCEIL", "G_SAVGFLOOR", "G_SAVGCEIL"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_IntVecNoV2i64Set)>,
    CoreAlwaysLower
  ];
}

def AArch64_SADDE : LegalizeRuleSet<["G_SADDE", "G_SSUBE", "G_UADDE", "G_USUBE", "G_SADDO", "G_SSUBO", "G_UADDO", "G_USUBO"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_SADDEPairSet)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreWiden<(Slot 1), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 1), (Ty AArch64_s64)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreAlwaysLower
  ];
}

def AArch64_FSHL : LegalizeRuleSet<["G_FSHL", "G_FSHR"]> {
  let Rules = [
    CoreCustom<(CoreInPairs (Slot 0), (Slot 1), AArch64_FSHLPairSet)>,
    CoreAlwaysLower
  ];
}

def AArch64_ROTR : LegalizeRuleSet<["G_ROTR"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_ROTRPairSet)>,
    CoreCustom<(CoreAll (CoreEq (SortOf (Slot 0)), KindScalar),
                        (CoreLt (BitsOf (Slot 1)), 64))>,
    CoreAlwaysLower
  ];
}

def AArch64_ROTL : LegalizeRuleSet<["G_ROTL"]> {
  let Rules = [
    CoreAlwaysLower
  ];
}

def AArch64_SBFX : LegalizeRuleSet<["G_SBFX", "G_UBFX"]> {
  let Rules = [
    CoreCustom<(CoreInPairs (Slot 0), (Slot 1), AArch64_SBFXPairSet)>
  ];
}

def AArch64_CTPOP : LegalizeRuleSet<["G_CTPOP"]> {
  let Rules = [
    CoreLegal<(CoreAll AArch64_HasCSSC, (CoreInPairs (Slot 0), (Slot 1), AArch64_CTPOPPairSet))>,
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_CTPOPPairSet2)>,
    CoreCustom<(CoreAll (CoreNot AArch64_HasCSSC), (CoreInPairs (Slot 0), (Slot 1), AArch64_CTPOPPairSet3))>,
    CoreCustom<(CoreInPairs (Slot 0), (Slot 1), AArch64_CTPOPPairSet4)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s128)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWidenPow2OrMinSize<(Slot 0), 8>,
    CoreWiden<(Slot 1), (SameAs 0)>,
    CoreNarrowIf<(Slot 1), (Replace (SameAs 0)),
                 (CoreLt (BitsOf (Slot 0)), (BitsOf (Slot 1)))>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>
  ];
}

def AArch64_CTLZ : LegalizeRuleSet<["G_CTLZ", "G_CTLS"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_CTLZPairSet)>,
    CoreWiden<(Slot 1), (Pow2Min 32)>,
    CoreWiden<(Slot 1), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 1), (Ty AArch64_s64)>,
    CoreWidenPow2OrMinSize<(Slot 1), 8>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 32)>,
    CoreWiden<(Slot 0), (SameAs 1)>,
    CoreNarrow<(Slot 0), (SameAs 1)>
  ];
}

def AArch64_INSERT_SUBVECTOR : LegalizeRuleSet<["G_INSERT_SUBVECTOR"]> {
  let Rules = [
    CoreAlwaysLower
  ];
}

def AArch64_CTLZ_ZERO_POISON : LegalizeRuleSet<["G_CTLZ_ZERO_POISON"]> {
  let Rules = [
    CoreAlwaysLower
  ];
}

def AArch64_CTTZ : LegalizeRuleSet<["G_CTTZ"]> {
  let Rules = [
    CoreLower<(CoreEq (SortOf (Slot 0)), KindVector)>,
    CoreWiden<(Slot 1), (Pow2Min 32)>,
    CoreWiden<(Slot 1), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 1), (Ty AArch64_s64)>,
    CoreWiden<(Slot 0), (SameAs 1)>,
    CoreNarrow<(Slot 0), (SameAs 1)>,
    CoreLegal<(CoreAll AArch64_HasCSSC,
                       (CoreIn (Slot 0), AArch64_S32Or64Set))>,
    CoreCustom<(CoreAll (CoreNot AArch64_HasCSSC),
                       (CoreIn (Slot 0), AArch64_S32Or64Set))>
  ];
}

def AArch64_CTTZ_ZERO_POISON : LegalizeRuleSet<["G_CTTZ_ZERO_POISON"]> {
  let Rules = [
    CoreAlwaysLower
  ];
}

def AArch64_BITREVERSE : LegalizeRuleSet<["G_BITREVERSE"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_BitreverseTypesSet)>,
    CoreWiden<(Slot 0), (Pow2Min 32)>,
    CoreWidenPow2OrMinSize<(Slot 0), 8>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreAlwaysLower
  ];
}

def AArch64_CLMUL : LegalizeRuleSet<["G_CLMUL"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_V8i8OrV16i8Set)>
  ];
}

def AArch64_BSWAP : LegalizeRuleSet<["G_BSWAP"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_BswapTypesSet)>,
    CoreWiden<(ElemOf (Slot 0)), (Pow2Min 16)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreMore<(Slot 0), (Pow2)>
  ];
}

def AArch64_UADDSAT : LegalizeRuleSet<["G_UADDSAT", "G_SADDSAT", "G_USUBSAT", "G_SSUBSAT"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_UaddsatTypesSet)>,
    CoreLegal<(CoreAll AArch64_HasSVE,
                       (CoreIn (Slot 0), AArch64_SVEIntVecSet))>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreAlwaysLower
  ];
}

def AArch64_FADD : LegalizeRuleSet<["G_FADD", "G_FSUB", "G_FMUL", "G_FDIV", "G_FMA", "G_FSQRT", "G_FMAXNUM", "G_FMINNUM", "G_FMAXIMUM", "G_FMINIMUM", "G_FCEIL", "G_FFLOOR", "G_FRINT", "G_FNEARBYINT", "G_INTRINSIC_TRUNC", "G_INTRINSIC_ROUND", "G_INTRINSIC_ROUNDEVEN"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_FPScalarOrVecSet)>,
    CoreLegal<(CoreAll AArch64_HasFP16,
                       (CoreIn (Slot 0), AArch64_F16OrV4V8F16Set))>,
    CoreLibcall<(CoreIn (Slot 0), AArch64_F128Set)>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreWidenIf<(Slot 0), (Replace (EltTypeTo (Slot 0), (Ty AArch64_f32))),
                (CoreAny (CoreAll (CoreNot AArch64_HasFP16),
                                  (IsFloat16 (ScalarOf (Slot 0)))),
                         (IsBFloat16 (ScalarOf (Slot 0))))>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreMore<(Slot 0), (Pow2)>
  ];
}

def AArch64_FABS : LegalizeRuleSet<["G_FABS", "G_FNEG"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_FPScalarOrVecSet)>,
    CoreLegal<(CoreAll AArch64_HasFP16,
                       (CoreIn (Slot 0), AArch64_F16OrBF16VecSet))>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreLower<(CoreGt (BitsOf (Slot 0)), 64)>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreLower<(CoreIn (Slot 0), AArch64_F16OrBF16VecSet)>
  ];
}

def AArch64_FREM : LegalizeRuleSet<["G_FREM", "G_FCOS", "G_FSIN", "G_FPOW", "G_FLOG", "G_FLOG2", "G_FLOG10", "G_FTAN", "G_FEXP", "G_FEXP2", "G_FEXP10", "G_FACOS", "G_FASIN", "G_FATAN", "G_FATAN2", "G_FCOSH", "G_FSINH", "G_FTANH", "G_FMODF"]> {
  let Rules = [
    CoreLibcall<(CoreIn (Slot 0), AArch64_F32To128Set)>,
    CoreWidenIf<(Slot 0), (Replace (EltTypeTo (Slot 0), (Ty AArch64_f32))),
                (CoreIn (Slot 0), AArch64_F16OrBF16Set)>,
    CoreScalarize<(Slot 0)>
  ];
}

def AArch64_FPOWI : LegalizeRuleSet<["G_FPOWI", "G_FLDEXP"]> {
  let Rules = [
    CoreLibcall<(CoreInPairs (Slot 0), (Slot 1), AArch64_FPOWIPairSet)>,
    CoreWidenIf<(Slot 0), (Replace (EltTypeTo (Slot 0), (Ty AArch64_f32))),
                (CoreIn (Slot 0), AArch64_F16OrBF16Set)>,
    CoreScalarize<(Slot 0)>
  ];
}

def AArch64_LROUND : LegalizeRuleSet<["G_LROUND", "G_INTRINSIC_LRINT"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_LROUNDPairSet)>,
    CoreLegal<(CoreAll AArch64_HasFP16, (CoreInPairs (Slot 0), (Slot 1), AArch64_LROUNDPairSet2))>,
    CoreWiden<(Slot 1), (Ty AArch64_s32)>,
    CoreLibcall<(CoreInPairs (Slot 0), (Slot 1), AArch64_LROUNDPairSet3)>,
    CoreAlwaysLower
  ];
}

def AArch64_LLROUND : LegalizeRuleSet<["G_LLROUND", "G_INTRINSIC_LLRINT"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_LLROUNDPairSet)>,
    CoreLegal<(CoreAll AArch64_HasFP16, (CoreInPairs (Slot 0), (Slot 1), AArch64_LLROUNDPairSet2))>,
    CoreWiden<(Slot 0), (Ty AArch64_s64)>,
    CoreWiden<(Slot 1), (Ty AArch64_s32)>,
    CoreLibcall<(CoreInPairs (Slot 0), (Slot 1), AArch64_LLROUNDPairSet3)>,
    CoreAlwaysLower
  ];
}

def AArch64_FCOPYSIGN : LegalizeRuleSet<["G_FCOPYSIGN"]> {
  let Rules = [
    CoreMoreIf<(Slot 0),
               (Replace (VecOf (CoreSelect (CoreEq (Slot 0), (Ty AArch64_s16)),
                                4, 2),
                               (Slot 0))),
               (CoreEq (SortOf (Slot 0)), KindScalar)>,
    CoreAlwaysLower
  ];
}

def AArch64_FMAD : LegalizeRuleSet<["G_FMAD"]> {
  let Rules = [
    CoreAlwaysLower
  ];
}

def AArch64_LOAD : LegalizeRuleSet<["G_LOAD"]> {
  let Rules = [
    CoreCustom<(CoreAll AArch64_HasRCPC3,
                        (CoreEq (Slot 0), (Ty AArch64_s128)),
                        (CoreEq (MemOrdering (MemSlot 0)),
                                OrderingAcquire))>,
    CoreCustom<(CoreAll (CoreEq (Slot 0), (Ty AArch64_s128)),
                        (CoreGe (MemOrdering (MemSlot 0)),
                                OrderingUnordered))>,
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), AArch64_LOADMemDescSet)>,
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), AArch64_LOADMemDescSet2)>,
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), AArch64_LOADMemDescSet3)>,
    CoreWiden<(Slot 0), (Pow2Min 8)>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_p0, 2>,
    CoreLower<(CoreAny (CoreNot (IsByteSized (MemTy (MemSlot 0)))),
                       (CoreNot (CoreIsPow2
                                     (SizeInBytesOf (MemTy (MemSlot 0))))))>,
    CoreWiden<(Slot 0), (Ty AArch64_s8)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreNarrowIf<(Slot 0), (Replace (Ty AArch64_i32)),
                 (CoreAll (CoreEq (SortOf (Slot 0)), KindScalar),
                          (CoreNe (Slot 0), (MemTy (MemSlot 0))),
                          (CoreGt (SizeOf (Slot 0)), 32))>,
    CoreBitcastIf<(Slot 0), (Replace (IntOfSize (SizeOf (Slot 0)))),
                  (CoreEq (Slot 0), (Ty AArch64_v4s8))>,
    CoreCustom<(CoreAll (CoreEq (SortOf (Slot 0)), KindPointerVector),
                        (CoreEq (AddrSpace (Slot 0)), 0))>,
    CoreScalarizeIf<(Slot 0), (CoreIn (Slot 0), AArch64_V2s16OrV2s8Set)>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>
  ];
}

def AArch64_STORE : LegalizeRuleSet<["G_STORE"]> {
  let Rules = [
    CoreCustom<(CoreAll AArch64_HasRCPC3,
                        (CoreEq (Slot 0), (Ty AArch64_s128)),
                        (CoreEq (MemOrdering (MemSlot 0)),
                                OrderingRelease))>,
    CoreCustom<(CoreAll (CoreEq (Slot 0), (Ty AArch64_s128)),
                        (CoreGe (MemOrdering (MemSlot 0)),
                                OrderingUnordered))>,
    CoreWidenIf<(Slot 0), (Ty AArch64_s32),
                (CoreGe (MemOrdering (MemSlot 0)), OrderingRelease)>,
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), AArch64_STOREMemDescSet)>,
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), AArch64_STOREMemDescSet2)>,
    CoreWiden<(Slot 0), (Ty AArch64_s8)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreWiden<(ElemOf (Slot 0)), (Ty AArch64_s8)>,
    CoreLower<(CoreAll (CoreEq (SortOf (Slot 0)), KindScalar),
                       (CoreNe (Slot 0), (MemTy (MemSlot 0))))>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_p0, 2>,
    CoreLower<(CoreNot
                   (CoreIsPow2 (SizeInBytesOf (MemTy (MemSlot 0)))))>,
    CoreBitcastIf<(Slot 0), (Replace (IntOfSize (SizeOf (Slot 0)))),
                  (CoreAll (CoreEq (Slot 0), (Ty AArch64_v4s8)),
                           (CoreEq (SizeOf (Slot 0)), (SizeOf (MemSlot 0))))>,
    CoreCustom<(CoreAll (CoreEq (SortOf (Slot 0)), KindPointerVector),
                        (CoreEq (AddrSpace (Slot 0)), 0))>,
    CoreScalarizeIf<(Slot 0), (CoreIn (Slot 0), AArch64_V2s16OrV2s8Set)>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreAlwaysLower
  ];
}

def AArch64_INDEXED_STORE : LegalizeRuleSet<["G_INDEXED_STORE"]> {
  let Rules = [
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), AArch64_INDEXED_STOREMemDescSet)>,
    CoreAlwaysUnsupported
  ];
}

def AArch64_INDEXED_LOAD : LegalizeRuleSet<["G_INDEXED_LOAD"]> {
  let Rules = [
    CoreUnsupported<(CoreGe (MemOrdering (MemSlot 0)), OrderingUnordered)>,
    CoreLegal<(CoreAll (CoreAny (CoreIn (Slot 0),
                                        AArch64_PackedVectorCoreSet),
                               (CoreIn (Slot 0),
                                       AArch64_ScalarAndPtrCoreSet),
                               (CoreEq (Slot 0), (Ty AArch64_s128))),
                       (CoreEq (Slot 1), (Ty AArch64_p0)))>,
    CoreAlwaysUnsupported
  ];
}

def AArch64_INDEXED_SEXTLOAD : LegalizeRuleSet<["G_INDEXED_SEXTLOAD", "G_INDEXED_ZEXTLOAD"]> {
  let Rules = [
    CoreUnsupported<(CoreGe (MemOrdering (MemSlot 0)), OrderingUnordered)>,
    CoreLegal<(CoreAll (CoreIn (Slot 0), AArch64_S16To64Set),
                       (CoreEq (Slot 1), (Ty AArch64_p0)),
                       (CoreAny
                           (CoreAll (CoreEq (Slot 0), (Ty AArch64_s16)),
                                    (CoreEq (MemTy (MemSlot 0)),
                                            (Ty AArch64_s8))),
                           (CoreAll (CoreEq (Slot 0), (Ty AArch64_s32)),
                                    (CoreIn (MemTy (MemSlot 0)),
                                            AArch64_MemS8OrS16Set)),
                           (CoreAll (CoreEq (Slot 0), (Ty AArch64_s64)),
                                    (CoreIn (MemTy (MemSlot 0)),
                                            AArch64_MemS8To32Set))))>,
    CoreAlwaysUnsupported
  ];
}

def AArch64_CONSTANT : LegalizeRuleSet<["G_CONSTANT"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_ScalarAndPtrCoreSet)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 0), (Ty AArch64_s8)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>
  ];
}

def AArch64_FCONSTANT : LegalizeRuleSet<["G_FCONSTANT"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_S16To128Set)>
  ];
}

def AArch64_ICMP : LegalizeRuleSet<["G_ICMP"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_ICMPPairSet)>,
    CoreWiden<(ElemOf (Slot 1)), (Pow2)>,
    CoreWiden<(ElemOf (Slot 1)), (Ty AArch64_s8)>,
    CoreWiden<(Slot 1), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 1), (Ty AArch64_s64)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s32)>,
    CoreScalarizeIf<(Slot 1), (CoreGt (BitsOf (Slot 1)), 64)>,
    CoreWidenIf<(Slot 0), (SameAs 1),
                (CoreAll (CoreEq (SortOf (Slot 0)), KindVector),
                         (CoreNot (CoreEq (SortOf (Slot 1)), KindPointerVector)),
                         (CoreNe (ElemOf (Slot 0)), (ElemOf (Slot 1))))>,
    CoreWidenIf<(ElemOf (Slot 1)), (Ty AArch64_s32),
                (CoreEq (Slot 1), (Ty AArch64_v2s16))>,
    CoreWidenIf<(ElemOf (Slot 0)), (Ty AArch64_s64),
                (CoreEq (SortOf (Slot 1)), KindPointerVector)>,
    CoreMore<(Slot 1), (Pow2)>,
    CoreClampMinNumElements<1, AArch64_s8, 8>,
    CoreClampMaxNumElements<1, AArch64_s8, 16>,
    CoreClampMinNumElements<1, AArch64_s16, 4>,
    CoreClampMaxNumElements<1, AArch64_s16, 8>,
    CoreClampMinNumElements<1, AArch64_s32, 2>,
    CoreClampMaxNumElements<1, AArch64_s32, 4>,
    CoreClampMinNumElements<1, AArch64_s64, 2>,
    CoreClampMaxNumElements<1, AArch64_s64, 2>,
    CoreClampMinNumElements<1, AArch64_p0, 2>,
    CoreClampMaxNumElements<1, AArch64_p0, 2>,
    CoreCustom<(CoreEq (SortOf (Slot 0)), KindVector)>
  ];
}

def AArch64_FCMP : LegalizeRuleSet<["G_FCMP"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_FCMPPairSet)>,
    CoreLegal<(CoreAll AArch64_HasFP16, (CoreInPairs (Slot 0), (Slot 1), AArch64_FCMPPairSet2))>,
    CoreWiden<(ElemOf (Slot 1)), (Pow2)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s32)>,
    CoreWidenIf<(Slot 1), (Replace (EltTypeTo (Slot 1), (Ty AArch64_f32))),
                (CoreAny (CoreAll (CoreNot AArch64_HasFP16),
                                  (IsFloat16 (ScalarOf (Slot 1)))),
                         (IsBFloat16 (ScalarOf (Slot 1))))>,
    CoreScalarizeIf<(Slot 1), (CoreGt (BitsOf (Slot 1)), 64)>,
    CoreWidenIf<(Slot 0), (SameAs 1),
                (CoreAll (CoreEq (SortOf (Slot 0)), KindVector),
                         (CoreNot (CoreEq (SortOf (Slot 1)), KindPointerVector)),
                         (CoreNe (ElemOf (Slot 0)), (ElemOf (Slot 1))))>,
    CoreClampMinNumElements<1, AArch64_s16, 4>,
    CoreClampMaxNumElements<1, AArch64_s16, 8>,
    CoreClampMinNumElements<1, AArch64_s32, 2>,
    CoreClampMaxNumElements<1, AArch64_s32, 4>,
    CoreClampMaxNumElements<1, AArch64_s64, 2>,
    CoreMore<(Slot 1), (Pow2)>,
    CoreLibcall<(CoreInPairs (Slot 0), (Slot 1), AArch64_FCMPPairSet3)>
  ];
}

def AArch64_ZEXT : LegalizeRuleSet<["G_ZEXT", "G_SEXT", "G_ANYEXT"]> {
  let Rules = [
    CoreLegal<(CoreAll (CoreNot (CoreEq (SortOf (Slot 0)), KindVector)),
                       (CoreGe (SizeOf (Slot 0)), 8),
                       (CoreLt (SizeOf (Slot 0)), 128),
                       (CoreIsPow2 (SizeOf (Slot 0))),
                       (CoreGe (SizeOf (Slot 1)), 8),
                       (CoreIsPow2 (SizeOf (Slot 1))))>,
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_ZEXTPairSet)>,
    CoreWiden<(Slot 0), (Ty AArch64_s64)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreClampMaxNumElements<1, AArch64_s8, 8>,
    CoreClampMaxNumElements<1, AArch64_s16, 4>,
    CoreClampMaxNumElements<1, AArch64_s32, 2>,
    CoreLower<(CoreAll (CoreEq (SortOf (Slot 0)), KindVector),
                       (CoreGt (BitsOf (Slot 0)),
                               (CoreMul (BitsOf (Slot 1)), 2)),
                       (CoreAny (CoreEq (BitsOf (Slot 1)), 8),
                                (CoreEq (BitsOf (Slot 1)), 16)))>,
    CoreClampMinNumElements<1, AArch64_s8, 8>,
    CoreClampMinNumElements<1, AArch64_s16, 4>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>
  ];
}

def AArch64_TRUNC : LegalizeRuleSet<["G_TRUNC"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_TRUNCPairSet)>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreClampMaxNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s32, 2>,
    CoreWidenIf<(ElemOf (Slot 0)), (Ty AArch64_s8),
                (CoreEq (SortOf (Slot 0)), KindVector)>,
    CoreLower<(CoreAll (CoreEq (SortOf (Slot 0)), KindVector),
                       (CoreGt (SizeOf (Slot 1)), 128),
                       (CoreLe (CoreMul (BitsOf (Slot 0)), 2),
                               (BitsOf (Slot 1))))>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreAlwaysLegal
  ];
}

def AArch64_TRUNC_SSAT_S : LegalizeRuleSet<["G_TRUNC_SSAT_S", "G_TRUNC_SSAT_U", "G_TRUNC_USAT_U"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_TRUNC_SSAT_SPairSet)>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 8>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 4>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 2>,
    CoreAlwaysLower
  ];
}

def AArch64_SEXT_INREG : LegalizeRuleSet<["G_SEXT_INREG"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_I32Or64OrIntVecSet)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreAlwaysLower
  ];
}

def AArch64_FPTRUNC : LegalizeRuleSet<["G_FPTRUNC"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_FPTRUNCPairSet)>,
    CoreLegal<(CoreAll AArch64_HasBF16, (CoreInPairs (Slot 0), (Slot 1), AArch64_FPTRUNCPairSet2))>,
    CoreLibcall<(CoreInPairs (Slot 0), (Slot 1), AArch64_FPTRUNCPairSet3)>,
    CoreMore<(Slot 1), (Pow2)>,
    CoreCustom<(CoreAll (CoreEq (BitsOf (Slot 1)), 64),
                       (CoreEq (BitsOf (Slot 0)), 16))>,
    CoreLower<(CoreInPairs (Slot 0), (Slot 1), AArch64_FPTRUNCPairSet2)>,
    CoreClampMinNumElements<1, AArch64_s32, 4>,
    CoreClampMaxNumElements<1, AArch64_s32, 4>,
    CoreClampMinNumElements<1, AArch64_s64, 2>,
    CoreClampMaxNumElements<1, AArch64_s64, 2>,
    CoreScalarize<(Slot 0)>
  ];
}

def AArch64_FPEXT : LegalizeRuleSet<["G_FPEXT"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_FPEXTPairSet)>,
    CoreLibcall<(CoreInPairs (Slot 0), (Slot 1), AArch64_FPEXTPairSet2)>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreWidenIf<(Slot 1), (Replace (EltTypeTo (Slot 1), (Ty AArch64_f32))),
                (CoreAll (CoreEq (SortOf (Slot 1)), KindVector),
                         (CoreEq (SortOf (Slot 0)), KindVector),
                         (CoreEq (BitsOf (Slot 1)), 16),
                         (CoreEq (BitsOf (Slot 0)), 64))>,
    CoreClampMinNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreScalarize<(Slot 0)>
  ];
}

def AArch64_FPTOSI : LegalizeRuleSet<["G_FPTOSI", "G_FPTOUI"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_FPTOSIPairSet)>,
    CoreLegal<(CoreAll AArch64_HasFP16, (CoreInPairs (Slot 0), (Slot 1), AArch64_FPTOSIPairSet2))>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreScalarizeIf<(Slot 1), (CoreGt (BitsOf (Slot 1)), 64)>,
    CoreNarrowIf<(Slot 0), (Replace (Ty AArch64_i64)),
                 (CoreAll (CoreEq (Slot 1), (Ty AArch64_f16)),
                          (CoreGt (SizeOf (Slot 0)), 64))>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreWiden<(ElemOf (Slot 0)), (Pow2)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreWidenIf<(Slot 1), (Replace (EltTypeTo (Slot 1), (Ty AArch64_f32))),
                (CoreAny (CoreAll (CoreNot AArch64_HasFP16),
                                  (IsFloat16 (ScalarOf (Slot 1)))),
                         (IsBFloat16 (ScalarOf (Slot 1))))>,
    CoreWidenIf<(Slot 1), (SameAs 0), (CoreLe (BitsOf (Slot 0)), 64)>,
    CoreWidenIf<(Slot 0), (SameAs 1), (CoreLe (BitsOf (Slot 1)), 64)>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreLibcall<(CoreInPairs (Slot 0), (Slot 1), AArch64_FPTOSIPairSet3)>
  ];
}

def AArch64_FPTOSI_SAT : LegalizeRuleSet<["G_FPTOSI_SAT", "G_FPTOUI_SAT"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_FPTOSI_SATPairSet)>,
    CoreLegal<(CoreAll AArch64_HasFP16, (CoreInPairs (Slot 0), (Slot 1), AArch64_FPTOSI_SATPairSet2))>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreScalarizeIf<(Slot 1), (CoreGt (BitsOf (Slot 1)), 64)>,
    CoreNarrowIf<(Slot 0), (Replace (Ty AArch64_i64)),
                 (CoreAll (CoreEq (Slot 1), (Ty AArch64_f16)),
                          (CoreGt (SizeOf (Slot 0)), 64))>,
    CoreLower<(CoreAny (CoreAll (CoreEq (SortOf (Slot 0)), KindScalar),
                                (CoreGt (SizeOf (Slot 0)), 64)),
                       (CoreAll (CoreEq (SortOf (Slot 1)), KindScalar),
                                (CoreGt (SizeOf (Slot 1)), 64)))>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 0), (Pow2Min 32)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreWidenIf<(Slot 1), (Replace (EltTypeTo (Slot 1), (Ty AArch64_f32))),
                (CoreAny (CoreAll (CoreNot AArch64_HasFP16),
                                  (IsFloat16 (ScalarOf (Slot 1)))),
                         (IsBFloat16 (ScalarOf (Slot 1))))>,
    CoreWidenIf<(Slot 1), (SameAs 0),
                (CoreAny (CoreEq (BitsOf (Slot 0)), 16),
                         (CoreEq (BitsOf (Slot 0)), 32),
                         (CoreEq (BitsOf (Slot 0)), 64))>,
    CoreWidenIf<(Slot 0), (SameAs 1),
                (CoreAny (CoreEq (BitsOf (Slot 1)), 16),
                         (CoreEq (BitsOf (Slot 1)), 32),
                         (CoreEq (BitsOf (Slot 1)), 64))>,
    CoreWiden<(ElemOf (Slot 0)), (Pow2)>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>
  ];
}

def AArch64_SITOFP : LegalizeRuleSet<["G_SITOFP", "G_UITOFP"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_SITOFPPairSet)>,
    CoreLegal<(CoreAll AArch64_HasFP16, (CoreInPairs (Slot 0), (Slot 1), AArch64_SITOFPPairSet2))>,
    CoreUnsupported<(IsBFloat16 (ScalarOf (Slot 0)))>,
    CoreScalarizeIf<(Slot 1), (CoreGt (BitsOf (Slot 1)), 64)>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreMore<(Slot 1), (Pow2)>,
    CoreWiden<(ElemOf (Slot 1)), (Pow2)>,
    CoreWiden<(Slot 1), (Ty AArch64_f32)>,
    CoreLower<(CoreAll (CoreEq (SortOf (Slot 1)), KindVector),
                       (CoreEq (BitsOf (Slot 1)), 64),
                       (CoreEq (BitsOf (Slot 0)), 16))>,
    CoreWiden<(Slot 0), (CoreSelect AArch64_HasFP16, 16, 32)>,
    CoreScalarizeIf<(Slot 0), (CoreAll (CoreEq (BitsOf (Slot 0)), 32),
                                  (CoreEq (BitsOf (Slot 1)), 64))>,
    CoreWidenIf<(Slot 0), (SameAs 1), (CoreLe (BitsOf (Slot 1)), 64)>,
    CoreWidenIf<(Slot 1), (SameAs 0), (CoreLe (BitsOf (Slot 0)), 64)>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreLibcall<(CoreInPairs (Slot 0), (Slot 1), AArch64_SITOFPPairSet3)>
  ];
}

def AArch64_BR : LegalizeRuleSet<["G_BR"]> {
  let Rules = [
    CoreAlwaysLegal
  ];
}

def AArch64_BRCOND : LegalizeRuleSet<["G_BRCOND"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_S32Set)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s32)>
  ];
}

def AArch64_BRINDIRECT : LegalizeRuleSet<["G_BRINDIRECT"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_PtrSet)>
  ];
}

def AArch64_SELECT : LegalizeRuleSet<["G_SELECT"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_SELECTPairSet)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreWiden<(Slot 1), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 1), (Ty AArch64_s32)>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreWidenIf<(Slot 1), (SameAs 0),
                (CoreAll (CoreEq (SortOf (Slot 0)), KindVector),
                         (CoreEq (SortOf (Slot 1)), KindVector))>,
    CoreLower<(CoreEq (SortOf (Slot 0)), KindVector)>
  ];
}

def AArch64_FRAME_INDEX : LegalizeRuleSet<["G_FRAME_INDEX"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_PtrSet)>
  ];
}

def AArch64_GLOBAL_VALUE : LegalizeRuleSet<["G_GLOBAL_VALUE"]> {
  let Rules = [
    CoreCustom<(CoreAll AArch64_SmallCodeModel)>,
    CoreLegal<(CoreNot AArch64_SmallCodeModel)>
  ];
}

def AArch64_PTRAUTH_GLOBAL_VALUE : LegalizeRuleSet<["G_PTRAUTH_GLOBAL_VALUE"]> {
  let Rules = [
    CoreLegal<(CoreAll (CoreEq (Slot 0), (Ty AArch64_p0)),
                       (CoreEq (Slot 1), (Ty AArch64_p0)))>
  ];
}

def AArch64_PTRTOINT : LegalizeRuleSet<["G_PTRTOINT"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_PTRTOINTPairSet)>,
    CoreWiden<(Slot 0), (Pow2Min 64)>,
    CoreWiden<(Slot 0), (Ty AArch64_s64)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>
  ];
}

def AArch64_INTTOPTR : LegalizeRuleSet<["G_INTTOPTR"]> {
  let Rules = [
    CoreUnsupported<(CoreNe (SizeOf (Slot 0)), (SizeOf (Slot 1)))>,
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_INTTOPTRPairSet)>,
    CoreClampMaxNumElements<1, AArch64_s64, 2>
  ];
}

def AArch64_BITCAST : LegalizeRuleSet<["G_BITCAST"]> {
  let Rules = [
    CoreLegal<(CoreAll (CoreIn (Slot 0), AArch64_Bitcast16Set),
                       (CoreIn (Slot 1), AArch64_Bitcast16Set))>,
    CoreLegal<(CoreAll (CoreIn (Slot 0), AArch64_Bitcast32Set),
                       (CoreIn (Slot 1), AArch64_Bitcast32Set))>,
    CoreLegal<(CoreAll (CoreIn (Slot 0), AArch64_Bitcast64Set),
                       (CoreIn (Slot 1), AArch64_Bitcast64Set))>,
    CoreLegal<(CoreAll (CoreIn (Slot 0), AArch64_Bitcast128Set),
                       (CoreIn (Slot 1), AArch64_Bitcast128Set))>,
    CoreCustom<(CoreAll (CoreEq (SortOf (Slot 0)), KindScalar),
                        (CoreEq (SortOf (Slot 1)), KindVector),
                        (CoreEq (BitsOf (Slot 1)), 1))>,
    CoreLower<(CoreAny
                   (CoreAll (CoreEq (SortOf (Slot 0)), KindVector),
                            (CoreNot (CoreEq (SortOf (Slot 1)), KindVector))),
                   (CoreAll (CoreNot (CoreEq (SortOf (Slot 0)), KindVector)),
                            (CoreEq (SortOf (Slot 1)), KindVector)))>,
    CoreLower<(CoreAll (CoreEq (SortOf (Slot 0)), KindVector),
                       (CoreNot (CoreEq (SortOf (Slot 0)),
                                        KindScalableVector)),
                       (CoreEq (SortOf (Slot 1)), KindVector),
                       (CoreNot (CoreEq (SortOf (Slot 1)),
                                        KindScalableVector)),
                       (CoreNe (CoreURem
                                    (CoreMul (NumElems (Slot 1)),
                                             (Pow2Ceil (NumElems (Slot 0)))),
                                    (NumElems (Slot 0))),
                               0))>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreAlwaysLower
  ];
}

def AArch64_VASTART : LegalizeRuleSet<["G_VASTART"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_PtrSet)>
  ];
}

def AArch64_VAARG : LegalizeRuleSet<["G_VAARG"]> {
  let Rules = [
    CoreCustom<(CoreAll (CoreIn (Slot 0), AArch64_ScalarAndPtrCoreSet),
                        (CoreIn (Slot 1), AArch64_PtrSet))>,
    CoreWiden<(Slot 0), (Ty AArch64_s8)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreWiden<(Slot 0), (Pow2Min 8)>
  ];
}

def AArch64_ATOMIC_CMPXCHG_WITH_SUCCESS : LegalizeRuleSet<["G_ATOMIC_CMPXCHG_WITH_SUCCESS"]> {
  let Rules = [
    CoreLower<(CoreAll (CoreIn (Slot 0), AArch64_S8To128Set),
                       (CoreEq (Slot 2), (Ty AArch64_p0)))>
  ];
}

def AArch64_ATOMIC_CMPXCHG : LegalizeRuleSet<["G_ATOMIC_CMPXCHG"]> {
  let Rules = [
    CoreLegal<(CoreAll (CoreNot AArch64_UseOutlineAtomics), (CoreInPairs (Slot 0), (Slot 1), AArch64_ATOMIC_CMPXCHGPairSet))>,
    CoreCustom<(CoreAll (CoreNot AArch64_UseOutlineAtomics), (CoreInPairs (Slot 0), (Slot 1), AArch64_ATOMIC_CMPXCHGPairSet2))>,
    CoreLibcall<(CoreAll AArch64_UseOutlineAtomics, (CoreInPairs (Slot 0), (Slot 1), AArch64_ATOMIC_CMPXCHGPairSet3))>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>
  ];
}

def AArch64_ATOMICRMW_XCHG : LegalizeRuleSet<["G_ATOMICRMW_XCHG", "G_ATOMICRMW_ADD", "G_ATOMICRMW_SUB", "G_ATOMICRMW_AND", "G_ATOMICRMW_OR", "G_ATOMICRMW_XOR"]> {
  let Rules = [
    CoreLegal<(CoreAll (CoreNot AArch64_UseOutlineAtomics), (CoreInPairs (Slot 0), (Slot 1), AArch64_ATOMICRMW_XCHGPairSet))>,
    CoreLibcall<(CoreAll AArch64_UseOutlineAtomics, (CoreInPairs (Slot 0), (Slot 1), AArch64_ATOMICRMW_XCHGPairSet2))>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>
  ];
}

def AArch64_ATOMICRMW_MIN : LegalizeRuleSet<["G_ATOMICRMW_MIN", "G_ATOMICRMW_MAX", "G_ATOMICRMW_UMIN", "G_ATOMICRMW_UMAX"]> {
  let Rules = [
    CoreLegal<(CoreAll (CoreIn (Slot 0), AArch64_S32Or64Set),
                       (CoreEq (Slot 1), (Ty AArch64_p0)))>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>
  ];
}

def AArch64_BLOCK_ADDR : LegalizeRuleSet<["G_BLOCK_ADDR"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_PtrSet)>
  ];
}

def AArch64_EXTRACT_VECTOR_ELT : LegalizeRuleSet<["G_EXTRACT_VECTOR_ELT"]> {
  let Rules = [
    CoreLegal<(CoreAll AArch64_HasSVE, (CoreInTriples (Slot 0), (Slot 1), (Slot 2), AArch64_EXTRACT_VECTOR_ELTTripleSet))>,
    CoreUnsupported<(CoreAll (CoreNot (CoreEq (SortOf (Slot 1)),
                                               KindScalableVector)),
                             (CoreNe (Slot 0), (ElemOf (Slot 1))))>,
    CoreWiden<(Slot 2), (Ty AArch64_s64)>,
    CoreCustom<(CoreIn (Slot 1), AArch64_PackedVectorOrV2s16Set)>,
    CoreWidenIf<(ElemOf (Slot 0)), (Ty AArch64_s64),
                (CoreAll (CoreEq (SortOf (Slot 1)), KindVector),
                         (CoreNot (CoreEq (SortOf (Slot 1)), KindScalableVector)),
                         (CoreLe (NumElems (Slot 1)), 2))>,
    CoreWidenIf<(ElemOf (Slot 0)), (Ty AArch64_s32),
                (CoreAll (CoreEq (SortOf (Slot 1)), KindVector),
                         (CoreNot (CoreEq (SortOf (Slot 1)), KindScalableVector)),
                         (CoreLe (NumElems (Slot 1)), 4))>,
    CoreWidenIf<(ElemOf (Slot 0)), (Ty AArch64_s16),
                (CoreAll (CoreEq (SortOf (Slot 1)), KindVector),
                         (CoreNot (CoreEq (SortOf (Slot 1)), KindScalableVector)),
                         (CoreLe (NumElems (Slot 1)), 8))>,
    CoreWidenIf<(ElemOf (Slot 0)), (Ty AArch64_s8),
                (CoreAll (CoreEq (SortOf (Slot 1)), KindVector),
                         (CoreNot (CoreEq (SortOf (Slot 1)), KindScalableVector)),
                         (CoreLe (NumElems (Slot 1)), 16))>,
    CoreWiden<(ElemOf (Slot 0)), (Ty AArch64_s8)>,
    CoreMore<(Slot 1), (Pow2)>,
    CoreClampMaxNumElements<1, AArch64_s64, 2>,
    CoreClampMaxNumElements<1, AArch64_s32, 4>,
    CoreClampMaxNumElements<1, AArch64_s16, 8>,
    CoreClampMaxNumElements<1, AArch64_s8, 16>,
    CoreClampMaxNumElements<1, AArch64_p0, 2>,
    CoreScalarizeIf<(Slot 1), (CoreGt (BitsOf (Slot 1)), 64)>
  ];
}

def AArch64_INSERT_VECTOR_ELT : LegalizeRuleSet<["G_INSERT_VECTOR_ELT"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_PackedVectorCoreSet)>,
    CoreLegal<(CoreAll AArch64_HasSVE, (CoreInTriples (Slot 0), (Slot 1), (Slot 2), AArch64_INSERT_VECTOR_ELTTripleSet))>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreWiden<(ElemOf (Slot 0)), (MinVectorSize 64)>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 2>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_p0, 2>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>
  ];
}

def AArch64_BUILD_VECTOR : LegalizeRuleSet<["G_BUILD_VECTOR"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_BUILD_VECTORPairSet)>,
    CoreClampMinNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreWiden<(ElemOf (Slot 0)), (Ty AArch64_s8)>,
    CoreWiden<(ElemOf (Slot 0)), (MinVectorSize 64)>,
    CoreWiden<(ElemOf (Slot 0)), (Pow2)>,
    CoreWiden<(Slot 1), (SameAs 0)>
  ];
}

def AArch64_BUILD_VECTOR_TRUNC : LegalizeRuleSet<["G_BUILD_VECTOR_TRUNC"]> {
  let Rules = [
    CoreAlwaysLower
  ];
}

def AArch64_SPLAT_VECTOR : LegalizeRuleSet<["G_SPLAT_VECTOR"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_SPLAT_VECTORPairSet)>,
    CoreLegal<(CoreAll AArch64_HasSVE, (CoreInPairs (Slot 0), (Slot 1), AArch64_SPLAT_VECTORPairSet2))>
  ];
}

def AArch64_SHUFFLE_VECTOR : LegalizeRuleSet<["G_SHUFFLE_VECTOR"]> {
  let Rules = [
    CoreLegal<(CoreAll (CoreEq (Slot 0), (Slot 1)),
                       (CoreIn (Slot 0), AArch64_ShuffleSrcSet))>,
    CoreMoreIf<(Slot 1), (Replace (SameAs 0)),
               (CoreGt (NumElems (Slot 0)), (NumElems (Slot 1)))>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreMoreIf<(Slot 0), (Replace (SameAs 1)),
               (CoreLt (NumElems (Slot 0)), (NumElems (Slot 1)))>,
    CoreWidenPow2OrMinSize<(Slot 0), 8>,
    CoreClampMinNumElements<0, AArch64_s8, 8>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMinNumElements<0, AArch64_s16, 4>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMinNumElements<0, AArch64_s32, 4>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<0, AArch64_s64, 2>,
    CoreClampMaxNumElements<0, AArch64_s64, 2>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
    CoreBitcastIf<(Slot 0), (Replace (EltTypeTo (Slot 0), (IntOfSize 64))),
                  (CoreEq (SortOf (Slot 0)), KindPointerVector)>
  ];
}

def AArch64_CONCAT_VECTORS : LegalizeRuleSet<["G_CONCAT_VECTORS"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_CONCAT_VECTORSPairSet)>,
    CoreCustom<(CoreAll (CoreEq (SortOf (Slot 0)), KindVector),
                        (CoreNot (CoreEq (SortOf (Slot 0)),
                                         KindScalableVector)),
                        (CoreLt (BitsOf (Slot 0)), 8))>,
    CoreBitcastIf<(Slot 0),
                  (Replace (DivLanes (EltSizeTo (Slot 0), (SizeOf (Slot 1))),
                                      (NumElems (Slot 1)))),
                  (CoreAll (CoreEq (SortOf (Slot 0)), KindVector),
                           (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)),
                           (CoreEq (SortOf (Slot 1)), KindVector),
                           (CoreNot (CoreEq (SortOf (Slot 1)), KindScalableVector)),
                           (CoreGe (BitsOf (Slot 0)), 8),
                           (CoreIsPow2 (BitsOf (Slot 0))),
                           (CoreLe (SizeOf (Slot 0)), 128),
                           (CoreLe (SizeOf (Slot 1)), 64))>
  ];
}

def AArch64_EXTRACT_SUBVECTOR : LegalizeRuleSet<["G_EXTRACT_SUBVECTOR"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_EXTRACT_SUBVECTORPairSet)>,
    CoreWiden<(ElemOf (Slot 0)), (Pow2)>,
    CoreClampMaxNumElements<0, AArch64_s8, 16>,
    CoreClampMaxNumElements<0, AArch64_s16, 8>,
    CoreClampMaxNumElements<0, AArch64_s32, 4>,
    CoreClampMinNumElements<1, AArch64_s8, 8>,
    CoreClampMaxNumElements<1, AArch64_s8, 16>,
    CoreClampMinNumElements<1, AArch64_s16, 4>,
    CoreClampMaxNumElements<1, AArch64_s16, 8>,
    CoreClampMinNumElements<1, AArch64_s32, 2>,
    CoreClampMaxNumElements<1, AArch64_s32, 4>,
    CoreAlwaysLower,
    ImmIdx<0>
  ];
}

def AArch64_JUMP_TABLE : LegalizeRuleSet<["G_JUMP_TABLE"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), AArch64_PtrSet)>
  ];
}

def AArch64_BRJT : LegalizeRuleSet<["G_BRJT"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_BRJTPairSet)>
  ];
}

def AArch64_TRAP : LegalizeRuleSet<["G_TRAP", "G_DEBUGTRAP", "G_UBSANTRAP"]> {
  let Rules = [
    CoreAlwaysLegal
  ];
}

def AArch64_DYN_STACKALLOC : LegalizeRuleSet<["G_DYN_STACKALLOC"]> {
  let Rules = [
    CoreAlwaysCustom
  ];
}

def AArch64_STACKSAVE : LegalizeRuleSet<["G_STACKSAVE", "G_STACKRESTORE"]> {
  let Rules = [
    CoreAlwaysLower
  ];
}

// The four memory intrinsics are guarded by hasMOPS(): with the extension the
// operation is legal (or custom), without it the operation is a libcall.  The
// guard is a constant for a given subtarget, so installing both rules and
// letting the guard pick is equivalent to the C++ `if` -- but only if the
// guard is actually written down.  The first transcription dropped the `if`
// and legalized every subtarget.
def AArch64_BZERO : LegalizeRuleSet<["G_BZERO"]> {
  let Rules = [
    CoreUnsupported<(CoreAll AArch64_HasMOPS)>,
    CoreLibcall<(CoreNot AArch64_HasMOPS)>
  ];
}

def AArch64_MEMSET : LegalizeRuleSet<["G_MEMSET"]> {
  let Rules = [
    CoreLegal<(CoreAll AArch64_HasMOPS,
                       (CoreEq (Slot 0), (Ty AArch64_p0)),
                       (CoreEq (Slot 1), (Ty AArch64_s64)),
                       (CoreEq (Slot 2), (Ty AArch64_s64)))>,
    CoreCustom<(CoreAll AArch64_HasMOPS,
                        (CoreEq (Slot 0), (Ty AArch64_p0)),
                        (CoreEq (Slot 1), (Ty AArch64_s8)),
                        (CoreEq (Slot 2), (Ty AArch64_s64)))>,
    CoreLibcall<(CoreNot AArch64_HasMOPS)>,
    // immIdx() returns unsigned, so it has to end the emitted chain.
    ImmIdx<0>
  ];
}

def AArch64_MEMCPY : LegalizeRuleSet<["G_MEMCPY", "G_MEMMOVE"]> {
  let Rules = [
    CoreLegal<(CoreAll AArch64_HasMOPS,
                       (CoreEq (Slot 0), (Ty AArch64_p0)),
                       (CoreEq (Slot 1), (Ty AArch64_p0)),
                       (CoreEq (Slot 2), (Ty AArch64_s64)))>,
    CoreLibcall<(CoreNot AArch64_HasMOPS)>,
    ImmIdx<0>
  ];
}

def AArch64_MEMCPY_INLINE : LegalizeRuleSet<["G_MEMCPY_INLINE"]> {
  let Rules = [
    CoreLegal<(CoreAll AArch64_HasMOPS,
                       (CoreEq (Slot 0), (Ty AArch64_p0)),
                       (CoreEq (Slot 1), (Ty AArch64_p0)),
                       (CoreEq (Slot 2), (Ty AArch64_s64)))>
  ];
}

def AArch64_MEMSET_INLINE : LegalizeRuleSet<["G_MEMSET_INLINE"]> {
  let Rules = [
    CoreLegal<(CoreAll AArch64_HasMOPS,
                       (CoreEq (Slot 0), (Ty AArch64_p0)),
                       (CoreEq (Slot 1), (Ty AArch64_s64)),
                       (CoreEq (Slot 2), (Ty AArch64_s64)))>,
    CoreCustom<(CoreAll AArch64_HasMOPS,
                        (CoreEq (Slot 0), (Ty AArch64_p0)),
                        (CoreEq (Slot 1), (Ty AArch64_s8)),
                        (CoreEq (Slot 2), (Ty AArch64_s64)))>
  ];
}

def AArch64_VECREDUCE_FADD : LegalizeRuleSet<["G_VECREDUCE_FADD"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_VECREDUCE_FADDPairSet)>,
    CoreLegal<(CoreAll AArch64_HasFP16, (CoreInPairs (Slot 0), (Slot 1), AArch64_VECREDUCE_FADDPairSet2))>,
    CoreWidenIf<(Slot 0), (Replace (EltTypeTo (Slot 0), (Ty AArch64_f32))),
                (CoreAny (CoreAll (CoreNot AArch64_HasFP16),
                                  (IsFloat16 (ScalarOf (Slot 0)))),
                         (IsBFloat16 (ScalarOf (Slot 0))))>,
    CoreClampMaxNumElements<1, AArch64_s64, 2>,
    CoreClampMaxNumElements<1, AArch64_s32, 4>,
    CoreClampMaxNumElements<1, AArch64_s16, 8>,
    CoreMore<(Slot 1), (Pow2)>,
    CoreScalarize<(Slot 1)>,
    CoreAlwaysLower
  ];
}

def AArch64_VECREDUCE_FMUL : LegalizeRuleSet<["G_VECREDUCE_FMUL"]> {
  let Rules = [
    CoreWidenIf<(Slot 0), (Replace (EltTypeTo (Slot 0), (Ty AArch64_f32))),
                (CoreAny (CoreAll (CoreNot AArch64_HasFP16),
                                  (IsFloat16 (ScalarOf (Slot 0)))),
                         (IsBFloat16 (ScalarOf (Slot 0))))>,
    CoreClampMaxNumElements<1, AArch64_s64, 2>,
    CoreClampMaxNumElements<1, AArch64_s32, 4>,
    CoreClampMaxNumElements<1, AArch64_s16, 8>,
    CoreClampMaxNumElements<1, AArch64_s32, 2>,
    CoreClampMaxNumElements<1, AArch64_s16, 4>,
    CoreScalarize<(Slot 1)>,
    CoreAlwaysLower
  ];
}

def AArch64_VECREDUCE_SEQ_FADD : LegalizeRuleSet<["G_VECREDUCE_SEQ_FADD", "G_VECREDUCE_SEQ_FMUL"]> {
  let Rules = [
    CoreScalarize<(Slot 2)>,
    CoreAlwaysLower
  ];
}

def AArch64_VECREDUCE_ADD : LegalizeRuleSet<["G_VECREDUCE_ADD"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_VECREDUCE_ADDPairSet)>,
    CoreMore<(Slot 1), (Pow2)>,
    CoreClampMaxNumElements<1, AArch64_s64, 2>,
    CoreClampMaxNumElements<1, AArch64_s32, 4>,
    CoreClampMaxNumElements<1, AArch64_s16, 8>,
    CoreClampMaxNumElements<1, AArch64_s8, 16>,
    CoreWiden<(ElemOf (Slot 1)), (MinVectorSize 64)>,
    CoreScalarize<(Slot 1)>
  ];
}

def AArch64_VECREDUCE_FMIN : LegalizeRuleSet<["G_VECREDUCE_FMIN", "G_VECREDUCE_FMAX", "G_VECREDUCE_FMINIMUM", "G_VECREDUCE_FMAXIMUM"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_VECREDUCE_FMINPairSet)>,
    CoreLegal<(CoreAll AArch64_HasFP16, (CoreInPairs (Slot 0), (Slot 1), AArch64_VECREDUCE_FMINPairSet2))>,
    CoreWidenIf<(Slot 0), (Replace (EltTypeTo (Slot 0), (Ty AArch64_f32))),
                (CoreAny (CoreAll (CoreNot AArch64_HasFP16),
                                  (IsFloat16 (ScalarOf (Slot 0)))),
                         (IsBFloat16 (ScalarOf (Slot 0))))>,
    CoreClampMaxNumElements<1, AArch64_s64, 2>,
    CoreClampMaxNumElements<1, AArch64_s32, 4>,
    CoreClampMaxNumElements<1, AArch64_s16, 8>,
    CoreScalarize<(Slot 1)>,
    CoreAlwaysLower
  ];
}

def AArch64_VECREDUCE_MUL : LegalizeRuleSet<["G_VECREDUCE_MUL"]> {
  let Rules = [
    CoreClampMaxNumElements<1, AArch64_s32, 2>,
    CoreClampMaxNumElements<1, AArch64_s16, 4>,
    CoreClampMaxNumElements<1, AArch64_s8, 8>,
    CoreScalarize<(Slot 1)>,
    CoreAlwaysLower
  ];
}

def AArch64_VECREDUCE_SMIN : LegalizeRuleSet<["G_VECREDUCE_SMIN", "G_VECREDUCE_SMAX", "G_VECREDUCE_UMIN", "G_VECREDUCE_UMAX"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), AArch64_VECREDUCE_SMINPairSet)>,
    CoreMoreIf<(Slot 1), (Pow2),
               (CoreAll (CoreEq (SortOf (Slot 1)), KindVector),
                        (CoreNe (ElemOf (Slot 1)), (Ty AArch64_s8)),
                        (CoreNe (CoreURem (NumElems (Slot 1)), 2), 0))>,
    CoreClampMaxNumElements<1, AArch64_s64, 2>,
    CoreClampMaxNumElements<1, AArch64_s32, 4>,
    CoreClampMaxNumElements<1, AArch64_s16, 8>,
    CoreClampMaxNumElements<1, AArch64_s8, 16>,
    CoreScalarize<(Slot 1)>,
    CoreAlwaysLower
  ];
}

def AArch64_VECREDUCE_OR : LegalizeRuleSet<["G_VECREDUCE_OR", "G_VECREDUCE_AND", "G_VECREDUCE_XOR"]> {
  let Rules = [
    CoreFewerIf<(Slot 1), (Replace (DivLanes (Slot 1), 2)),
                (CoreAll (CoreNot (CoreEq (SortOf (Slot 1)), KindScalar)),
                         (CoreIsPow2 (NumElems (Slot 1))),
                         (CoreGt (SizeOf (Slot 1)), 64))>,
    CoreScalarize<(Slot 1)>,
    CoreAlwaysLower
  ];
}

def AArch64_VECTOR_COMPRESS : LegalizeRuleSet<["G_VECTOR_COMPRESS"]> {
  let Rules = [
    CoreAlwaysLower
  ];
}

def AArch64_GET_FPENV : LegalizeRuleSet<["G_GET_FPENV", "G_SET_FPENV", "G_RESET_FPENV", "G_GET_FPMODE", "G_SET_FPMODE", "G_RESET_FPMODE"]> {
  let Rules = [
    CoreAlwaysLibcall
  ];
}

def AArch64_GET_ROUNDING : LegalizeRuleSet<["G_GET_ROUNDING", "G_SET_ROUNDING"]> {
  let Rules = [
    CoreCustom<(CoreIn (Slot 0), AArch64_S32Set)>
  ];
}

def AArch64_IS_FPCLASS : LegalizeRuleSet<["G_IS_FPCLASS"]> {
  let Rules = [
    CoreAlwaysLower
  ];
}

def AArch64_PREFETCH : LegalizeRuleSet<["G_PREFETCH"]> {
  let Rules = [
    CoreAlwaysCustom
  ];
}

def AArch64_SCMP : LegalizeRuleSet<["G_SCMP", "G_UCMP"]> {
  let Rules = [
    CoreAlwaysLower
  ];
}

def AArch64_INTRINSIC : LegalizeRuleSet<["G_INTRINSIC", "G_INTRINSIC_W_SIDE_EFFECTS"]> {
  let Rules = [
    CoreAlwaysLegal
  ];
}

def AArch64_FENCE : LegalizeRuleSet<["G_FENCE"]> {
  let Rules = [
    CoreAlwaysLegal
  ];
}

def AArch64_INVOKE_REGION_START : LegalizeRuleSet<["G_INVOKE_REGION_START"]> {
  let Rules = [
    CoreAlwaysLegal
  ];
}

def AArch64_SEXTLOAD : LegalizeRuleSet<["G_SEXTLOAD"]> {
  let Rules = [
    CoreLower<(CoreGe (MemOrdering (MemSlot 0)), OrderingUnordered)>,
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), AArch64_SEXTLOADMemDescSet)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreUnsupported<(CoreNot
                         (CoreIsPow2 (SizeInBytesOf (MemTy (MemSlot 0)))))>,
    CoreAlwaysLower
  ];
}

def AArch64_ZEXTLOAD : LegalizeRuleSet<["G_ZEXTLOAD"]> {
  let Rules = [
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), AArch64_ZEXTLOADMemDescSet)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreUnsupported<(CoreNot
                         (CoreIsPow2 (SizeInBytesOf (MemTy (MemSlot 0)))))>,
    CoreAlwaysLower
  ];
}

def AArch64_MERGE_VALUES : LegalizeRuleSet<["G_MERGE_VALUES"]> {
  let Rules = [
    CoreWiden<(Slot 1), (Pow2Min 8)>,
    CoreLower<(CoreAll (CoreEq (SortOf (Slot 0)), KindScalar),
                       (CoreNot (CoreIsPow2 (SizeOf (Slot 0)))),
                       (CoreLt (SizeOf (Slot 0)), 64))>,
    CoreWiden<(Slot 0), (Pow2Min 32)>,
    CoreWiden<(Slot 1), (Ty AArch64_s8)>,
    CoreNarrow<(Slot 1), (Ty AArch64_s64)>,
    CoreWiden<(Slot 0), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s128)>,
    CoreLegal<(CoreAll (CoreAny (CoreEq (SizeOf (Slot 0)), 32),
                               (CoreEq (SizeOf (Slot 0)), 64),
                               (CoreEq (SizeOf (Slot 0)), 128)),
                       (CoreAny (CoreEq (SizeOf (Slot 1)), 8),
                                (CoreEq (SizeOf (Slot 1)), 16),
                                (CoreEq (SizeOf (Slot 1)), 32),
                                (CoreEq (SizeOf (Slot 1)), 64)))>
  ];
}

def AArch64_UNMERGE_VALUES : LegalizeRuleSet<["G_UNMERGE_VALUES"]> {
  let Rules = [
    CoreWiden<(Slot 0), (Pow2Min 8)>,
    CoreLower<(CoreAll (CoreEq (SortOf (Slot 1)), KindScalar),
                       (CoreNot (CoreIsPow2 (SizeOf (Slot 1)))),
                       (CoreLt (SizeOf (Slot 1)), 64))>,
    CoreWiden<(Slot 1), (Pow2Min 32)>,
    CoreWiden<(Slot 0), (Ty AArch64_s8)>,
    CoreNarrow<(Slot 0), (Ty AArch64_s64)>,
    CoreWiden<(Slot 1), (Ty AArch64_s32)>,
    CoreNarrow<(Slot 1), (Ty AArch64_s128)>,
    CoreLegal<(CoreAll (CoreAny (CoreEq (SizeOf (Slot 1)), 32),
                               (CoreEq (SizeOf (Slot 1)), 64),
                               (CoreEq (SizeOf (Slot 1)), 128)),
                       (CoreAny (CoreEq (SizeOf (Slot 0)), 8),
                                (CoreEq (SizeOf (Slot 0)), 16),
                                (CoreEq (SizeOf (Slot 0)), 32),
                                (CoreEq (SizeOf (Slot 0)), 64)))>
  ];
}
