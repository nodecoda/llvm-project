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
// (custom(), bitcastIf(), minScalarOrEltIf(), raw mutation lambdas, immIdx()).
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

def AArch64_NotHasCSSC : LegalizePredicateDag<(not AArch64_HasCSSC)>;
def AArch64_NotUseOutlineAtomics
    : LegalizePredicateDag<(not AArch64_UseOutlineAtomics)>;
def AArch64_NotSmallCodeModel
    : LegalizePredicateDag<(not AArch64_SmallCodeModel)>;
// hasMOPS() gates a whole group of rules, and legalForCartesianProduct() has
// no bool-guard overload in the C++ API, so the gate is written as the
// query-time predicate the rule already is.  A subtarget feature is constant
// per query, so the two forms agree.
def AArch64_Pred_HasMOPS : LegalizeQueryPredicate<
    "[=](const LegalityQuery &) { return AArch64_HasMOPS; }">;
def AArch64_Pred_NotHasMOPS : LegalizeQueryPredicate<
    "[=](const LegalityQuery &) { return !AArch64_HasMOPS; }">;

def AArch64_Pred_MemSetLegal : LegalizeQueryPredicate<
    "[=](const LegalityQuery &Q) { return AArch64_HasMOPS && Q.Types[0] == AArch64_p0 && Q.Types[1] == AArch64_s64 && Q.Types[2] == AArch64_s64; }">;
def AArch64_Pred_MemSetCustom : LegalizeQueryPredicate<
    "[=](const LegalityQuery &Q) { return AArch64_HasMOPS && Q.Types[0] == AArch64_p0 && Q.Types[1] == AArch64_s8 && Q.Types[2] == AArch64_s64; }">;
def AArch64_Pred_MemCpyLegal : LegalizeQueryPredicate<
    "[=](const LegalityQuery &Q) { return AArch64_HasMOPS && Q.Types[0] == AArch64_p0 && Q.Types[1] == AArch64_p0 && Q.Types[2] == AArch64_s64; }">;
def AArch64_Pred_MemSetInlineLegal : LegalizeQueryPredicate<
    "[=](const LegalityQuery &Q) { return AArch64_HasMOPS && Q.Types[0] == AArch64_p0 && Q.Types[1] == AArch64_s64 && Q.Types[2] == AArch64_s64; }">;
def AArch64_Pred_MemSetInlineCustom : LegalizeQueryPredicate<
    "[=](const LegalityQuery &Q) { return AArch64_HasMOPS && Q.Types[0] == AArch64_p0 && Q.Types[1] == AArch64_s8 && Q.Types[2] == AArch64_s64; }">;

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
def AArch64_ScalarAndPtrTypes : LegalizeNamedTypeList<
    [AArch64_s8, AArch64_s16, AArch64_s32, AArch64_s64, AArch64_p0]>;


//===----------------------------------------------------------------------===//
// Predicates
//===----------------------------------------------------------------------===//

def AArch64_NotSmallCodeModelQuery : LegalizeQueryPredicate<"[=](const LegalityQuery &) { return !AArch64_SmallCodeModel; }">;
def AArch64_Pred_02008ef0 : LegalizeQueryPredicate<"typeInSet(0, {AArch64_s64, AArch64_p0})">;
def AArch64_Pred_0226be53 : LegalizeQueryPredicate<"[AArch64_HasFP16](const LegalityQuery &Query) { return (!AArch64_HasFP16 && Query.Types[0].getScalarType().isFloat16()) || Query.Types[0].getScalarType().isBFloat16(); }">;
def AArch64_Pred_05361bee : LegalizeQueryPredicate<"[](const LegalityQuery &Query) { return Query.Types[0].isScalar(); }">;
def AArch64_Pred_06878545 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { LLT DstTy = Query.Types[0]; LLT SrcTy = Query.Types[1]; if (!DstTy.isFixedVector() || !SrcTy.isFixedVector()) return false; unsigned MoreElts = 1u << Log2_32_Ceil(DstTy.getNumElements()); return SrcTy.getNumElements() * MoreElts % DstTy.getNumElements() != 0; }">;
def AArch64_Pred_080c5b65 : LegalizeQueryPredicate<"typeInSet(1, {AArch64_s64, AArch64_p0})">;
def AArch64_Pred_0dda87c5 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[1].isFixedVector() && Query.Types[1].getNumElements() <= 16; }">;
def AArch64_Pred_11e45701 : LegalizeQueryPredicate<"[](const LegalityQuery &Q) { LLT DstTy = Q.Types[0]; LLT SrcTy = Q.Types[1]; return SrcTy.isVector() && DstTy.isVector() && SrcTy.getScalarSizeInBits() == 16 && DstTy.getScalarSizeInBits() == 64; }">;
def AArch64_Pred_125bf639 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[0].isVector() != Query.Types[1].isVector(); }">;
def AArch64_Pred_12d65c07 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[1].isVector() && Query.Types[1].getElementType() != AArch64_s8 && Query.Types[1].getNumElements() & 1; }">;
def AArch64_Pred_131ecb96 : LegalizeQueryPredicate<"[=](const LegalityQuery &Q) { LLT SrcTy = Q.Types[1]; if (SrcTy.isScalar()) return false; if (!isPowerOf2_32(SrcTy.getNumElements())) return false; return SrcTy.getSizeInBits() > 64; }">;
def AArch64_Pred_15859522 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[0].getNumElements() <= 4; }">;
def AArch64_Pred_1671daba : LegalizeQueryPredicate<"[](const LegalityQuery &Q) { LLT DstTy = Q.Types[0]; LLT SrcTy = Q.Types[1]; return SrcTy.getScalarSizeInBits() == 64 && DstTy.getScalarSizeInBits() == 16; }">;
def AArch64_Pred_1a80c04b : LegalizeQueryPredicate<"typeInSet(0, {AArch64_v8s8, AArch64_v16s8, AArch64_v4s16, AArch64_v8s16, AArch64_v2s32, AArch64_v4s32, AArch64_v2s64, AArch64_v2p0})">;
def AArch64_Pred_1f3dc25d : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[0].isVector(); }">;
def AArch64_Pred_20db2dd3 : LegalizeQueryPredicate<"::any(scalarWiderThan(0, 64), scalarWiderThan(1, 64))">;
def AArch64_Pred_2ac0a068 : LegalizeQueryPredicate<"atomicOrderingAtLeastOrStrongerThan(0, AtomicOrdering::Unordered)">;
def AArch64_Pred_2b954739 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { LLT LdTy = Query.Types[0]; LLT PtrTy = Query.Types[1]; if (!llvm::is_contained({AArch64_v16s8, AArch64_v8s16, AArch64_v4s32, AArch64_v2s64, AArch64_v2p0, AArch64_v8s8, AArch64_v4s16, AArch64_v2s32}, LdTy) && !llvm::is_contained({AArch64_s8, AArch64_s16, AArch64_s32, AArch64_s64, AArch64_p0}, LdTy) && LdTy != AArch64_s128) return false; if (PtrTy != AArch64_p0) return false; return true; }">;
def AArch64_Pred_2bd673a4 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return AArch64_HasRCPC3 && Query.Types[0] == AArch64_s128 && Query.MMODescrs[0].Ordering == AtomicOrdering::Release; }">;
def AArch64_Pred_2d4385e9 : LegalizeQueryPredicate<"all(typeInSet(0, {AArch64_s32, AArch64_s64}), typeIs(1, AArch64_p0))">;
def AArch64_Pred_2e11e704 : LegalizeQueryPredicate<"[=](const LegalityQuery &Q) { const LLT BigTy = Q.Types[1]; return BigTy.isScalar() && !isPowerOf2_32(BigTy.getSizeInBits()) && BigTy.getSizeInBits() < 64; }">;
def AArch64_Pred_2f2fbffa : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[0].getNumElements() <= 2; }">;
def AArch64_Pred_30a9dec1 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[0].getNumElements() <= 16; }">;
def AArch64_Pred_359c5626 : LegalizeQueryPredicate<"typeInSet(0, {AArch64_v2s16, AArch64_v2s8})">;
def AArch64_Pred_38f29bd4 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return (Query.Types[0].getScalarSizeInBits() > Query.Types[1].getScalarSizeInBits() * 2) && Query.Types[0].isVector() && (Query.Types[1].getScalarSizeInBits() == 8 || Query.Types[1].getScalarSizeInBits() == 16); }">;
def AArch64_Pred_4212e231 : LegalizeQueryPredicate<"all(typeInSet(0, {AArch64_s16, AArch64_s32, AArch64_s64, AArch64_p0}), typeInSet(1, {AArch64_s32, AArch64_s64, AArch64_s128, AArch64_p0}), smallerThan(0, 1))">;
def AArch64_Pred_4472211a : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[1].isFixedVector() && Query.Types[1].getNumElements() <= 2; }">;
def AArch64_Pred_458fc228 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[1] == AArch64_v2s16; }">;
def AArch64_Pred_47fbed85 : LegalizeQueryPredicate<"[](const LegalityQuery &Query) { return Query.Types[0].getScalarSizeInBits() == 32 && Query.Types[1].getScalarSizeInBits() == 64; }">;
def AArch64_Pred_51f3473b : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[0].getScalarSizeInBits() <= 64 && Query.Types[0].getScalarSizeInBits() > Query.Types[1].getScalarSizeInBits(); }">;
def AArch64_Pred_5789b262 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { const LLT &Ty = Query.Types[0]; const LLT &SrcTy = Query.Types[1]; return Ty.isVector() && !SrcTy.isPointerVector() && Ty.getElementType() != SrcTy.getElementType(); }">;
def AArch64_Pred_5b3dda4d : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[0].isFixedVector() && Query.Types[1].isFixedVector() && Query.Types[0].getScalarSizeInBits() >= 8 && isPowerOf2_64(Query.Types[0].getScalarSizeInBits()) && Query.Types[0].getSizeInBits() <= 128 && Query.Types[1].getSizeInBits() <= 64; }">;
def AArch64_Pred_5cc26dc8 : LegalizeQueryPredicate<"isPointerVector(0)">;
def AArch64_Pred_6029cf3d : LegalizeQueryPredicate<"typeInSet(1, {AArch64_s128})">;
def AArch64_Pred_61c58b0b : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { unsigned ITySize = Query.Types[0].getScalarSizeInBits(); return (ITySize == 16 || ITySize == 32 || ITySize == 64) && ITySize > Query.Types[1].getScalarSizeInBits(); }">;
def AArch64_Pred_6209b6d1 : LegalizeQueryPredicate<"[=](const LegalityQuery &Q) { switch (Q.Types[1].getSizeInBits()) { case 32: case 64: case 128: break; default: return false; } switch (Q.Types[0].getSizeInBits()) { case 8: case 16: case 32: case 64: return true; default: return false; } }">;
def AArch64_Pred_661419a9 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[1].getScalarSizeInBits() <= 64 && Query.Types[0].getScalarSizeInBits() < Query.Types[1].getScalarSizeInBits(); }">;
def AArch64_Pred_6bbd1890 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[0].isFixedVector() && Query.Types[0].getScalarSizeInBits() < 8; }">;
def AArch64_Pred_6dcb14b4 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { LLT DstTy = Query.Types[0]; LLT SrcTy = Query.Types[1]; return DstTy.isVector() && SrcTy.getSizeInBits() > 128 && DstTy.getScalarSizeInBits() * 2 <= SrcTy.getScalarSizeInBits(); }">;
def AArch64_Pred_7727ed82 : LegalizeQueryPredicate<"[](const LegalityQuery &Query) { return Query.Types[0].getNumElements() > Query.Types[1].getNumElements(); }">;
def AArch64_Pred_79336d5c : LegalizeQueryPredicate<"all(scalarNarrowerThan(0, 32), atomicOrderingAtLeastOrStrongerThan(0, AtomicOrdering::Release))">;
def AArch64_Pred_79e450cf : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { const auto &SrcTy = Query.Types[0]; const auto &AmtTy = Query.Types[1]; return !SrcTy.isVector() && SrcTy.getSizeInBits() == 32 && AmtTy.getSizeInBits() == 32; }">;
def AArch64_Pred_7d8fccce : LegalizeQueryPredicate<"all(typeInSet(0, {AArch64_v4s8}), LegalityPredicate([=](const LegalityQuery &Query) { return Query.Types[0].getSizeInBits() == Query.MMODescrs[0].MemoryTy.getSizeInBits(); }))">;
def AArch64_Pred_84602781 : LegalizeQueryPredicate<"all(isVector(0), isVector(1))">;
def AArch64_Pred_85a847d2 : LegalizeQueryPredicate<"typeInSet(1, {AArch64_s32})">;
def AArch64_Pred_8883238c : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[1].isFixedVector() && Query.Types[1].getNumElements() <= 4; }">;
def AArch64_Pred_8bbda3e4 : LegalizeQueryPredicate<"all(typeIs(0, AArch64_p0), typeIs(1, AArch64_p0))">;
def AArch64_Pred_8bd7def7 : LegalizeQueryPredicate<"all(typeInSet(0, {AArch64_s16, AArch64_s32, AArch64_s64}), LegalityPredicate([=](const LegalityQuery &Q) { LLT LdTy = Q.Types[0]; LLT PtrTy = Q.Types[1]; LLT MemTy = Q.MMODescrs[0].MemoryTy; if (PtrTy != AArch64_p0) return false; if (LdTy == AArch64_s16) return MemTy == AArch64_s8; if (LdTy == AArch64_s32) return MemTy == AArch64_s8 || MemTy == AArch64_s16; if (LdTy == AArch64_s64) return MemTy == AArch64_s8 || MemTy == AArch64_s16 || MemTy == AArch64_s32; return false; }))">;
def AArch64_Pred_91f2ab63 : LegalizeQueryPredicate<"[=](const LegalityQuery &Q) { return Q.Types[0].isScalar() && Q.Types[1].getScalarSizeInBits() < 64; }">;
def AArch64_Pred_92a15f60 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[1].isFixedVector() && Query.Types[1].getNumElements() <= 8; }">;
def AArch64_Pred_92f28716 : LegalizeQueryPredicate<"[](const LegalityQuery &Query) { return Query.Types[0].getScalarSizeInBits() <= 64 && Query.Types[0].getScalarSizeInBits() > Query.Types[1].getScalarSizeInBits(); }">;
def AArch64_Pred_938ab9ea : LegalizeQueryPredicate<"[](const LegalityQuery &Query) { return Query.Types[1].isVector() && Query.Types[1].getScalarSizeInBits() == 64 && Query.Types[0].getScalarSizeInBits() == 16; }">;
def AArch64_Pred_959eb4fe : LegalizeQueryPredicate<"[=](const LegalityQuery &Q) { return (!AArch64_HasFP16 && Q.Types[1].getScalarType().isFloat16()) || Q.Types[1].getScalarType().isBFloat16(); }">;
def AArch64_Pred_971c0f17 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { const LLT &DstTy = Query.Types[0]; const LLT &SrcTy = Query.Types[1]; if (DstTy != SrcTy) return false; return llvm::is_contained( {AArch64_v8s8, AArch64_v16s8, AArch64_v4s16, AArch64_v8s16, AArch64_v2s32, AArch64_v4s32, AArch64_v2s64}, DstTy); }">;
def AArch64_Pred_99ace828 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { unsigned FTySize = Query.Types[1].getScalarSizeInBits(); return (FTySize == 16 || FTySize == 32 || FTySize == 64) && Query.Types[0].getScalarSizeInBits() < FTySize; }">;
def AArch64_Pred_9bc27123 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[0] == AArch64_s128 && Query.MMODescrs[0].Ordering != AtomicOrdering::NotAtomic; }">;
def AArch64_Pred_a10e207c : LegalizeQueryPredicate<"[&](const LegalityQuery &Query) { return Query.Types[0].getScalarType().isBFloat16(); }">;
def AArch64_Pred_a39de19a : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { const LLT &VecTy = Query.Types[1]; return VecTy == AArch64_v8s8 || VecTy == AArch64_v16s8 || VecTy == AArch64_v2s16 || VecTy == AArch64_v4s16 || VecTy == AArch64_v8s16 || VecTy == AArch64_v2s32 || VecTy == AArch64_v4s32 || VecTy == AArch64_v2s64 || VecTy == AArch64_v2p0; }">;
def AArch64_Pred_a4516f30 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return AArch64_HasRCPC3 && Query.Types[0] == AArch64_s128 && Query.MMODescrs[0].Ordering == AtomicOrdering::Acquire; }">;
def AArch64_Pred_b1fd2ec9 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[0].isScalar() && Query.Types[0] != Query.MMODescrs[0].MemoryTy; }">;
def AArch64_Pred_b36dc304 : LegalizeQueryPredicate<"all(typeInSet(0, {AArch64_s8, AArch64_s16, AArch64_s32, AArch64_s64, AArch64_s128}), typeIs(2, AArch64_p0))">;
def AArch64_Pred_b76ecc6d : LegalizeQueryPredicate<"typeInSet(0, {AArch64_v4s8})">;
def AArch64_Pred_bb2dbe0b : LegalizeQueryPredicate<"isVector(0)">;
def AArch64_Pred_bf12db7c : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { const LLT &EltTy = Query.Types[1].getElementType(); if (Query.Types[1].isScalableVector()) return false; return Query.Types[0] != EltTy; }">;
def AArch64_Pred_c20b2585 : LegalizeQueryPredicate<"[=](const LegalityQuery &Q) { switch (Q.Types[0].getSizeInBits()) { case 32: case 64: case 128: break; default: return false; } switch (Q.Types[1].getSizeInBits()) { case 8: case 16: case 32: case 64: return true; default: return false; } }">;
def AArch64_Pred_cb649b90 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[1] == AArch64_f16 && Query.Types[0].getSizeInBits() > 64; }">;
def AArch64_Pred_cc1acbc6 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[1].isPointerVector(); }">;
def AArch64_Pred_ce0a28a5 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { const LLT &ValTy = Query.Types[0]; return ValTy.isPointerVector() && ValTy.getAddressSpace() == 0; }">;
def AArch64_Pred_cfbb9ad6 : LegalizeQueryPredicate<"scalarOrEltWiderThan(0, 32)">;
def AArch64_Pred_d9be76fb : LegalizeQueryPredicate<"scalarOrEltWiderThan(1, 64)">;
def AArch64_Pred_da4664e1 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[0] == AArch64_v4s8; }">;
def AArch64_Pred_e0a3c826 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[0] == AArch64_v2s16; }">;
def AArch64_Pred_e2acf90b : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { unsigned DstSize = Query.Types[0].getSizeInBits(); if (Query.Types[0].isVector()) return false; if (DstSize < 8 || DstSize >= 128 || !isPowerOf2_32(DstSize)) return false; const LLT &SrcTy = Query.Types[1]; unsigned SrcSize = SrcTy.getSizeInBits(); if (SrcSize < 8 || !isPowerOf2_32(SrcSize)) return false; return true; }">;
def AArch64_Pred_e44b3fa8 : LegalizeQueryPredicate<"[=](const LegalityQuery &Q) { const LLT BigTy = Q.Types[0]; return BigTy.isScalar() && !isPowerOf2_32(BigTy.getSizeInBits()) && BigTy.getSizeInBits() < 64; }">;
def AArch64_Pred_e47b6b4a : LegalizeQueryPredicate<"[](const LegalityQuery &Query) { return Query.Types[1].getScalarSizeInBits() <= 64 && Query.Types[0].getScalarSizeInBits() < Query.Types[1].getScalarSizeInBits(); }">;
def AArch64_Pred_e69215a4 : LegalizeQueryPredicate<"[AArch64_HasFP16](const LegalityQuery &Query) { return (!AArch64_HasFP16 && Query.Types[1].getScalarType().isFloat16()) || Query.Types[1].getScalarType().isBFloat16(); }">;
def AArch64_Pred_e83e7106 : LegalizeQueryPredicate<"typeInSet(0, {AArch64_s32})">;
def AArch64_Pred_ed4695d7 : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { return Query.Types[0].isScalar() && Query.Types[0] != Query.MMODescrs[0].MemoryTy && Query.Types[0].getSizeInBits() > 32; }">;
def AArch64_Pred_f0403665 : LegalizeQueryPredicate<"[](const LegalityQuery &Query) { return Query.Types[0].getNumElements() < Query.Types[1].getNumElements(); }">;
def AArch64_Pred_f37f5690 : LegalizeQueryPredicate<"[=](const LegalityQuery &Q) { return true; }">;
def AArch64_Pred_f41b94df : LegalizeQueryPredicate<"[=](const LegalityQuery &Query) { LLT DstTy = Query.Types[0]; LLT SrcTy = Query.Types[1]; return DstTy.isScalar() && SrcTy.isVector() && SrcTy.getScalarSizeInBits() == 1; }">;
def AArch64_Pred_f4c34fb1 : LegalizeQueryPredicate<"scalarOrEltWiderThan(0, 64)">;
def AArch64_Pred_f87a2e39 : LegalizeQueryPredicate<"[=](const LegalityQuery &Q) { LLT SrcTy = Q.Types[0]; return SrcTy.isScalar() && SrcTy.getSizeInBits() < 128; }">;
def AArch64_Pred_fd9b821d : LegalizeQueryPredicate<"[&](const LegalityQuery &Query) { return Query.Types[0].getSizeInBits() != Query.Types[1].getSizeInBits(); }">;
def AArch64_Pred_fe80edda : LegalizeQueryPredicate<"all(typeInSet(0, {AArch64_s32, AArch64_s64, AArch64_p0}), typeInSet(1, {AArch64_s8, AArch64_s16, AArch64_s32}), smallerThan(1, 0))">;
def AArch64_Pred_ff6be52a : LegalizeQueryPredicate<"[=](const LegalityQuery &Q) { return (!AArch64_HasFP16 && Q.Types[0].getScalarType().isFloat16()) || Q.Types[0].getScalarType().isBFloat16(); }">;
def AArch64_SmallCodeModelQuery : LegalizeQueryPredicate<"[=](const LegalityQuery &) { return AArch64_SmallCodeModel; }">;

//===----------------------------------------------------------------------===//
// Rule sets
//===----------------------------------------------------------------------===//

def AArch64_IMPLICIT_DEF : LegalizeRuleSet<["G_IMPLICIT_DEF", "G_FREEZE", "G_CONSTANT_FOLD_BARRIER"]> {
  let Rules = [
    LegalFor<[AArch64_p0, AArch64_s8, AArch64_s16, AArch64_s32, AArch64_s64, AArch64_s128]>,
    LegalFor<[AArch64_v2s8, AArch64_v4s8, AArch64_v8s8, AArch64_v16s8, AArch64_v2s16, AArch64_v4s16, AArch64_v8s16, AArch64_v2s32, AArch64_v4s32, AArch64_v2s64, AArch64_v2p0]>,
    WidenScalarToNextPow2<0>,
    ClampScalar<0, AArch64_s8, AArch64_s64>,
    MoreElementsToNextPow2<0>,
    CoreWiden<(ElemOf (Slot 0)), (MinVectorSize 64)>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampMaxNumElements<0, AArch64_s64, 2>,
    ClampMaxNumElements<0, AArch64_p0, 2>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>
  ];
}

def AArch64_PHI : LegalizeRuleSet<["G_PHI"]> {
  let Rules = [
    LegalFor<[AArch64_p0, AArch64_s16, AArch64_s32, AArch64_s64]>,
    LegalForList<AArch64_PackedVectorAllTypes>,
    WidenScalarToNextPow2<0>,
    MoreElementsToNextPow2<0>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    ClampScalar<0, AArch64_s16, AArch64_s64>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampMaxNumElements<0, AArch64_s64, 2>,
    ClampMaxNumElements<0, AArch64_p0, 2>,
    WidenScalarOrEltToNextPow2OrMinSize<0, 8>
  ];
}

def AArch64_INSERT : LegalizeRuleSet<["G_INSERT"]> {
  let Rules = [
    LegalIf<AArch64_Pred_fe80edda>,
    WidenScalarToNextPow2<0>,
    ClampScalar<0, AArch64_s32, AArch64_s64>,
    WidenScalarToNextPow2<1>,
    MinScalar<1, AArch64_s8>,
    MaxScalarIf<AArch64_Pred_e83e7106, 1, AArch64_s16>,
    MaxScalarIf<AArch64_Pred_02008ef0, 1, AArch64_s32>
  ];
}

def AArch64_EXTRACT : LegalizeRuleSet<["G_EXTRACT"]> {
  let Rules = [
    LegalIf<AArch64_Pred_4212e231>,
    WidenScalarToNextPow2<1>,
    ClampScalar<1, AArch64_s32, AArch64_s128>,
    WidenScalarToNextPow2<0>,
    MinScalar<0, AArch64_s16>,
    MaxScalarIf<AArch64_Pred_85a847d2, 0, AArch64_s16>,
    MaxScalarIf<AArch64_Pred_080c5b65, 0, AArch64_s32>,
    MaxScalarIf<AArch64_Pred_6029cf3d, 0, AArch64_s64>
  ];
}

def AArch64_ADD : LegalizeRuleSet<["G_ADD", "G_SUB", "G_AND", "G_OR", "G_XOR"]> {
  let Rules = [
    LegalFor<[AArch64_i32, AArch64_i64, AArch64_v8i8, AArch64_v16i8, AArch64_v4i16, AArch64_v8i16, AArch64_v2i32, AArch64_v4i32, AArch64_v2i64]>,
    LegalForIf<AArch64_HasSVE, [AArch64_nxv16i8, AArch64_nxv8i16, AArch64_nxv4i32, AArch64_nxv2i64]>,
    WidenScalarToNextPow2<0>,
    ClampScalar<0, AArch64_s32, AArch64_s64>,
    ClampMaxNumElements<0, AArch64_s8, 16>,
    ClampMaxNumElements<0, AArch64_s16, 8>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    MinScalarOrEltIf<AArch64_Pred_2f2fbffa, 0, AArch64_s32>,
    MinScalarOrEltIf<AArch64_Pred_15859522, 0, AArch64_s16>,
    MinScalarOrEltIf<AArch64_Pred_30a9dec1, 0, AArch64_s8>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    MoreElementsToNextPow2<0>
  ];
}

def AArch64_MUL : LegalizeRuleSet<["G_MUL"]> {
  let Rules = [
    LegalFor<[AArch64_i32, AArch64_i64, AArch64_v8i8, AArch64_v16i8, AArch64_v4i16, AArch64_v8i16, AArch64_v2i32, AArch64_v4i32, AArch64_v2i64]>,
    WidenScalarToNextPow2<0>,
    ClampScalar<0, AArch64_s32, AArch64_s64>,
    ClampMaxNumElements<0, AArch64_s8, 16>,
    ClampMaxNumElements<0, AArch64_s16, 8>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    MinScalarOrEltIf<AArch64_Pred_2f2fbffa, 0, AArch64_s32>,
    MinScalarOrEltIf<AArch64_Pred_15859522, 0, AArch64_s16>,
    MinScalarOrEltIf<AArch64_Pred_30a9dec1, 0, AArch64_s8>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    MoreElementsToNextPow2<0>
  ];
}

def AArch64_SHL : LegalizeRuleSet<["G_SHL", "G_ASHR", "G_LSHR"]> {
  let Rules = [
    CustomIf<AArch64_Pred_79e450cf>,
    LegalForPairs<[LegalizeTypePair<AArch64_i32, AArch64_i32>, LegalizeTypePair<AArch64_i32, AArch64_i64>, LegalizeTypePair<AArch64_i64, AArch64_i64>, LegalizeTypePair<AArch64_v8i8, AArch64_v8i8>, LegalizeTypePair<AArch64_v16i8, AArch64_v16i8>, LegalizeTypePair<AArch64_v4i16, AArch64_v4i16>, LegalizeTypePair<AArch64_v8i16, AArch64_v8i16>, LegalizeTypePair<AArch64_v2i32, AArch64_v2i32>, LegalizeTypePair<AArch64_v4i32, AArch64_v4i32>, LegalizeTypePair<AArch64_v2i64, AArch64_v2i64>]>,
    WidenScalarToNextPow2<1>,
    WidenScalarToNextPow2<0>,
    ClampScalar<1, AArch64_s32, AArch64_s64>,
    ClampScalar<0, AArch64_s32, AArch64_s64>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    MoreElementsToNextPow2<0>,
    MinScalarSameAs<1, 0>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    MinScalarEltSameAsIf<AArch64_Pred_bb2dbe0b, 1, 0>,
    MaxScalarEltSameAsIf<AArch64_Pred_bb2dbe0b, 1, 0>
  ];
}

def AArch64_PTR_ADD : LegalizeRuleSet<["G_PTR_ADD"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_p0, AArch64_i64>, LegalizeTypePair<AArch64_v2p0, AArch64_v2i64>]>,
    ClampScalarOrElt<1, AArch64_s64, AArch64_s64>,
    ClampNumElements<0, AArch64_v2p0, AArch64_v2p0>
  ];
}

def AArch64_PTRMASK : LegalizeRuleSet<["G_PTRMASK"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_p0, AArch64_s64>]>
  ];
}

def AArch64_SDIV : LegalizeRuleSet<["G_SDIV", "G_UDIV"]> {
  let Rules = [
    LegalFor<[AArch64_i32, AArch64_i64]>,
    LibcallFor<[AArch64_i128]>,
    ClampScalar<0, AArch64_s32, AArch64_s64>,
    WidenScalarToNextPow2<0>,
    Scalarize<0>
  ];
}

def AArch64_SREM : LegalizeRuleSet<["G_SREM", "G_UREM", "G_SDIVREM", "G_UDIVREM"]> {
  let Rules = [
    LowerFor<[AArch64_i8, AArch64_i16, AArch64_i32, AArch64_i64, AArch64_v2i32, AArch64_v4i32, AArch64_v2i64]>,
    LibcallFor<[AArch64_i128]>,
    WidenScalarOrEltToNextPow2<0>,
    MinScalarOrElt<0, AArch64_s32>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    Scalarize<0>
  ];
}

def AArch64_SMULO : LegalizeRuleSet<["G_SMULO", "G_UMULO"]> {
  let Rules = [
    WidenScalarToNextPow2<0, /*Min = */ 32>,
    ClampScalar<0, AArch64_s32, AArch64_s64>,
    Lower
  ];
}

def AArch64_SMULH : LegalizeRuleSet<["G_SMULH", "G_UMULH"]> {
  let Rules = [
    LegalFor<[AArch64_i64, AArch64_v16i8, AArch64_v8i16, AArch64_v4i32]>,
    ClampMaxNumElements<0, AArch64_s8, 16>,
    ClampMaxNumElements<0, AArch64_s16, 8>,
    ClampMaxNumElements<0, AArch64_s32, 4>,
    Lower
  ];
}

def AArch64_SMULFIX : LegalizeRuleSet<["G_SMULFIX", "G_UMULFIX", "G_SMULFIXSAT", "G_UMULFIXSAT"]> {
  let Rules = [
    Lower
  ];
}

def AArch64_SMIN : LegalizeRuleSet<["G_SMIN", "G_SMAX", "G_UMIN", "G_UMAX"]> {
  let Rules = [
    LegalFor<[AArch64_v8i8, AArch64_v16i8, AArch64_v4i16, AArch64_v8i16, AArch64_v2i32, AArch64_v4i32]>,
    LegalForIf<AArch64_HasCSSC, [AArch64_i32, AArch64_i64]>,
    MinScalar<0, AArch64_s32>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    Lower
  ];
}

def AArch64_ABS : LegalizeRuleSet<["G_ABS"]> {
  let Rules = [
    LegalForIf<AArch64_HasCSSC, [AArch64_i32, AArch64_i64]>,
    LegalFor<[AArch64_v16i8, AArch64_v8i16, AArch64_v4i32, AArch64_v2i64, AArch64_v2p0, AArch64_v8i8, AArch64_v4i16, AArch64_v2i32]>,
    CustomIf<AArch64_Pred_f87a2e39>,
    WidenScalarIf<AArch64_Pred_da4664e1, LegalizeRawMutation<"[=](const LegalityQuery &Query) { return std::make_pair(0, AArch64_v4i16); }">>,
    WidenScalarIf<AArch64_Pred_e0a3c826, LegalizeRawMutation<"[=](const LegalityQuery &Query) { return std::make_pair(0, AArch64_v2i32); }">>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    MoreElementsToNextPow2<0>,
    Lower
  ];
}

def AArch64_ABDS : LegalizeRuleSet<["G_ABDS", "G_ABDU", "G_UAVGFLOOR", "G_UAVGCEIL", "G_SAVGFLOOR", "G_SAVGCEIL"]> {
  let Rules = [
    LegalFor<[AArch64_v8i8, AArch64_v16i8, AArch64_v4i16, AArch64_v8i16, AArch64_v2i32, AArch64_v4i32]>,
    Lower
  ];
}

def AArch64_SADDE : LegalizeRuleSet<["G_SADDE", "G_SSUBE", "G_UADDE", "G_USUBE", "G_SADDO", "G_SSUBO", "G_UADDO", "G_USUBO"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_i32, AArch64_i32>, LegalizeTypePair<AArch64_i64, AArch64_i32>]>,
    ClampScalar<0, AArch64_s32, AArch64_s64>,
    ClampScalar<1, AArch64_s32, AArch64_s64>,
    WidenScalarToNextPow2<0>,
    Lower
  ];
}

def AArch64_FSHL : LegalizeRuleSet<["G_FSHL", "G_FSHR"]> {
  let Rules = [
    CustomForPairs<[LegalizeTypePair<AArch64_i32, AArch64_i32>, LegalizeTypePair<AArch64_i32, AArch64_i64>, LegalizeTypePair<AArch64_i64, AArch64_i64>]>,
    Lower
  ];
}

def AArch64_ROTR : LegalizeRuleSet<["G_ROTR"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_i32, AArch64_i64>, LegalizeTypePair<AArch64_i64, AArch64_i64>]>,
    CustomIf<AArch64_Pred_91f2ab63>,
    Lower
  ];
}

def AArch64_ROTL : LegalizeRuleSet<["G_ROTL"]> {
  let Rules = [
    Lower
  ];
}

def AArch64_SBFX : LegalizeRuleSet<["G_SBFX", "G_UBFX"]> {
  let Rules = [
    CustomForPairs<[LegalizeTypePair<AArch64_s32, AArch64_s32>, LegalizeTypePair<AArch64_s64, AArch64_s64>]>
  ];
}

def AArch64_CTPOP : LegalizeRuleSet<["G_CTPOP"]> {
  let Rules = [
    LegalForPairsIf<AArch64_HasCSSC, [LegalizeTypePair<AArch64_i32, AArch64_i32>, LegalizeTypePair<AArch64_i64, AArch64_i64>]>,
    LegalForPairs<[LegalizeTypePair<AArch64_v8i8, AArch64_v8i8>, LegalizeTypePair<AArch64_v16i8, AArch64_v16i8>]>,
    CustomForPairsIf<AArch64_NotHasCSSC, [LegalizeTypePair<AArch64_s32, AArch64_s32>, LegalizeTypePair<AArch64_s64, AArch64_s64>]>,
    CustomForPairs<[LegalizeTypePair<AArch64_s128, AArch64_s128>, LegalizeTypePair<AArch64_v4s16, AArch64_v4s16>, LegalizeTypePair<AArch64_v8s16, AArch64_v8s16>, LegalizeTypePair<AArch64_v2s32, AArch64_v2s32>, LegalizeTypePair<AArch64_v4s32, AArch64_v4s32>, LegalizeTypePair<AArch64_v2s64, AArch64_v2s64>]>,
    ClampScalar<0, AArch64_s32, AArch64_s128>,
    WidenScalarToNextPow2<0>,
    WidenScalarOrEltToNextPow2OrMinSize<0, 8>,
    MinScalarEltSameAsIf<AArch64_Pred_f37f5690, 1, 0>,
    MaxScalarEltSameAsIf<AArch64_Pred_f37f5690, 1, 0>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    MoreElementsToNextPow2<0>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>
  ];
}

def AArch64_CTLZ : LegalizeRuleSet<["G_CTLZ", "G_CTLS"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_i32, AArch64_i32>, LegalizeTypePair<AArch64_i64, AArch64_i64>, LegalizeTypePair<AArch64_v8i8, AArch64_v8i8>, LegalizeTypePair<AArch64_v16i8, AArch64_v16i8>, LegalizeTypePair<AArch64_v4i16, AArch64_v4i16>, LegalizeTypePair<AArch64_v8i16, AArch64_v8i16>, LegalizeTypePair<AArch64_v2i32, AArch64_v2i32>, LegalizeTypePair<AArch64_v4i32, AArch64_v4i32>]>,
    WidenScalarToNextPow2<1, /*Min=*/32>,
    ClampScalar<1, AArch64_s32, AArch64_s64>,
    WidenScalarOrEltToNextPow2OrMinSize<1, 8>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    MoreElementsToNextPow2<0>,
    ScalarizeIf<AArch64_Pred_cfbb9ad6, 0>,
    ScalarSameSizeAs<0, 1>
  ];
}

def AArch64_INSERT_SUBVECTOR : LegalizeRuleSet<["G_INSERT_SUBVECTOR"]> {
  let Rules = [
    Lower
  ];
}

def AArch64_CTLZ_ZERO_POISON : LegalizeRuleSet<["G_CTLZ_ZERO_POISON"]> {
  let Rules = [
    Lower
  ];
}

def AArch64_CTTZ : LegalizeRuleSet<["G_CTTZ"]> {
  let Rules = [
    LowerIf<AArch64_Pred_bb2dbe0b>,
    WidenScalarToNextPow2<1, /*Min=*/32>,
    ClampScalar<1, AArch64_s32, AArch64_s64>,
    ScalarSameSizeAs<0, 1>,
    LegalForIf<AArch64_HasCSSC, [AArch64_s32, AArch64_s64]>,
    CustomForIf<AArch64_NotHasCSSC, [AArch64_s32, AArch64_s64]>
  ];
}

def AArch64_CTTZ_ZERO_POISON : LegalizeRuleSet<["G_CTTZ_ZERO_POISON"]> {
  let Rules = [
    Lower
  ];
}

def AArch64_BITREVERSE : LegalizeRuleSet<["G_BITREVERSE"]> {
  let Rules = [
    LegalFor<[AArch64_i32, AArch64_i64, AArch64_v8i8, AArch64_v16i8]>,
    WidenScalarToNextPow2<0, /*Min = */ 32>,
    WidenScalarOrEltToNextPow2OrMinSize<0, 8>,
    ClampScalar<0, AArch64_s32, AArch64_s64>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    MoreElementsToNextPow2<0>,
    Lower
  ];
}

def AArch64_CLMUL : LegalizeRuleSet<["G_CLMUL"]> {
  let Rules = [
    LegalFor<[AArch64_v8i8, AArch64_v16i8]>
  ];
}

def AArch64_BSWAP : LegalizeRuleSet<["G_BSWAP"]> {
  let Rules = [
    LegalFor<[AArch64_i32, AArch64_i64, AArch64_v4i16, AArch64_v8i16, AArch64_v2i32, AArch64_v4i32, AArch64_v2i64]>,
    WidenScalarOrEltToNextPow2<0, 16>,
    ClampScalar<0, AArch64_s32, AArch64_s64>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    MoreElementsToNextPow2<0>
  ];
}

def AArch64_UADDSAT : LegalizeRuleSet<["G_UADDSAT", "G_SADDSAT", "G_USUBSAT", "G_SSUBSAT"]> {
  let Rules = [
    LegalFor<[AArch64_v8i8, AArch64_v16i8, AArch64_v4i16, AArch64_v8i16, AArch64_v2i32, AArch64_v4i32, AArch64_v2i64]>,
    LegalForIf<AArch64_HasSVE, [AArch64_nxv16i8, AArch64_nxv8i16, AArch64_nxv4i32, AArch64_nxv2i64]>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampMaxNumElements<0, AArch64_s64, 2>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    MoreElementsToNextPow2<0>,
    Lower
  ];
}

def AArch64_FADD : LegalizeRuleSet<["G_FADD", "G_FSUB", "G_FMUL", "G_FDIV", "G_FMA", "G_FSQRT", "G_FMAXNUM", "G_FMINNUM", "G_FMAXIMUM", "G_FMINIMUM", "G_FCEIL", "G_FFLOOR", "G_FRINT", "G_FNEARBYINT", "G_INTRINSIC_TRUNC", "G_INTRINSIC_ROUND", "G_INTRINSIC_ROUNDEVEN"]> {
  let Rules = [
    LegalFor<[AArch64_f32, AArch64_f64, AArch64_v2f32, AArch64_v4f32, AArch64_v2f64]>,
    LegalForIf<AArch64_HasFP16, [AArch64_f16, AArch64_v4f16, AArch64_v8f16]>,
    LibcallFor<[AArch64_f128]>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    WidenScalarIf<AArch64_Pred_ff6be52a, LegalizeRawMutation<"changeElementTo(0, AArch64_f32)">>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    MoreElementsToNextPow2<0>
  ];
}

def AArch64_FABS : LegalizeRuleSet<["G_FABS", "G_FNEG"]> {
  let Rules = [
    LegalFor<[AArch64_f32, AArch64_f64, AArch64_v2f32, AArch64_v4f32, AArch64_v2f64]>,
    LegalForIf<AArch64_HasFP16, [AArch64_f16, AArch64_bf16, AArch64_v4f16, AArch64_v4bf16, AArch64_v8f16, AArch64_v8bf16]>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    LowerIf<AArch64_Pred_f4c34fb1>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    MoreElementsToNextPow2<0>,
    LowerFor<[AArch64_f16, AArch64_bf16, AArch64_v4f16, AArch64_v4bf16, AArch64_v8f16, AArch64_v8bf16]>
  ];
}

def AArch64_FREM : LegalizeRuleSet<["G_FREM", "G_FCOS", "G_FSIN", "G_FPOW", "G_FLOG", "G_FLOG2", "G_FLOG10", "G_FTAN", "G_FEXP", "G_FEXP2", "G_FEXP10", "G_FACOS", "G_FASIN", "G_FATAN", "G_FATAN2", "G_FCOSH", "G_FSINH", "G_FTANH", "G_FMODF"]> {
  let Rules = [
    LibcallFor<[AArch64_f32, AArch64_f64, AArch64_f128]>,
    WidenScalarFor<LegalizeInlineTypeList<[AArch64_f16, AArch64_bf16]>, LegalizeRawMutation<"changeElementTo(0, AArch64_f32)">>,
    Scalarize<0>
  ];
}

def AArch64_FPOWI : LegalizeRuleSet<["G_FPOWI", "G_FLDEXP"]> {
  let Rules = [
    LibcallForPairs<[LegalizeTypePair<AArch64_f32, AArch64_i32>, LegalizeTypePair<AArch64_f64, AArch64_i32>, LegalizeTypePair<AArch64_f128, AArch64_i32>]>,
    WidenScalarFor<LegalizeInlineTypeList<[AArch64_f16, AArch64_bf16]>, LegalizeRawMutation<"changeElementTo(0, AArch64_f32)">>,
    Scalarize<0>
  ];
}

def AArch64_LROUND : LegalizeRuleSet<["G_LROUND", "G_INTRINSIC_LRINT"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_i32, AArch64_f32>, LegalizeTypePair<AArch64_i32, AArch64_f64>, LegalizeTypePair<AArch64_i64, AArch64_f32>, LegalizeTypePair<AArch64_i64, AArch64_f64>]>,
    LegalForPairsIf<AArch64_HasFP16, [LegalizeTypePair<AArch64_i32, AArch64_f16>, LegalizeTypePair<AArch64_i64, AArch64_f16>]>,
    MinScalar<1, AArch64_s32>,
    LibcallForPairs<[LegalizeTypePair<AArch64_s64, AArch64_s128>]>,
    Lower
  ];
}

def AArch64_LLROUND : LegalizeRuleSet<["G_LLROUND", "G_INTRINSIC_LLRINT"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_i64, AArch64_f32>, LegalizeTypePair<AArch64_i64, AArch64_f64>]>,
    LegalForPairsIf<AArch64_HasFP16, [LegalizeTypePair<AArch64_i64, AArch64_f16>]>,
    MinScalar<0, AArch64_s64>,
    MinScalar<1, AArch64_s32>,
    LibcallForPairs<[LegalizeTypePair<AArch64_s64, AArch64_s128>]>,
    Lower
  ];
}

def AArch64_FCOPYSIGN : LegalizeRuleSet<["G_FCOPYSIGN"]> {
  let Rules = [
    MoreElementsIf<AArch64_Pred_05361bee, LegalizeRawMutation<"[=](const LegalityQuery &Query) { const LLT Ty = Query.Types[0]; return std::pair(0, LLT::fixed_vector(Ty == AArch64_s16 ? 4 : 2, Ty)); }">>,
    Lower
  ];
}

def AArch64_FMAD : LegalizeRuleSet<["G_FMAD"]> {
  let Rules = [
    Lower
  ];
}

def AArch64_LOAD : LegalizeRuleSet<["G_LOAD"]> {
  let Rules = [
    CustomIf<AArch64_Pred_a4516f30>,
    CustomIf<AArch64_Pred_9bc27123>,
    LegalForMemDesc<[LegalizeMemDesc<AArch64_s8, AArch64_p0, AArch64_s8, 8>, LegalizeMemDesc<AArch64_s16, AArch64_p0, AArch64_s16, 8>, LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s32, 8>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_p0, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_s128, AArch64_p0, AArch64_s128, 8>, LegalizeMemDesc<AArch64_v8s8, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_v16s8, AArch64_p0, AArch64_s128, 8>, LegalizeMemDesc<AArch64_v4s16, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_v8s16, AArch64_p0, AArch64_s128, 8>, LegalizeMemDesc<AArch64_v2s32, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_v4s32, AArch64_p0, AArch64_s128, 8>, LegalizeMemDesc<AArch64_v2s64, AArch64_p0, AArch64_s128, 8>]>,
    LegalForMemDesc<[LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s8, 8>, LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s16, 8>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s32, 8>]>,
    LegalForMemDesc<[LegalizeMemDesc<AArch64_nxv16s8, AArch64_p0, AArch64_nxv16s8, 8>, LegalizeMemDesc<AArch64_nxv8s16, AArch64_p0, AArch64_nxv8s16, 8>, LegalizeMemDesc<AArch64_nxv4s32, AArch64_p0, AArch64_nxv4s32, 8>, LegalizeMemDesc<AArch64_nxv2s64, AArch64_p0, AArch64_nxv2s64, 8>]>,
    WidenScalarToNextPow2<0, /* MinSize = */ 8>,
    ClampMaxNumElements<0, AArch64_s8, 16>,
    ClampMaxNumElements<0, AArch64_s16, 8>,
    ClampMaxNumElements<0, AArch64_s32, 4>,
    ClampMaxNumElements<0, AArch64_s64, 2>,
    ClampMaxNumElements<0, AArch64_p0, 2>,
    LowerIfMemSizeNotByteSizePow2,
    ClampScalar<0, AArch64_s8, AArch64_s64>,
    NarrowScalarIf<AArch64_Pred_ed4695d7, LegalizeRawMutation<"changeTo(0, AArch64_i32)">>,
    BitcastIf<AArch64_Pred_b76ecc6d, LegalizeRawMutation<"[=](const LegalityQuery &Query) { const LLT VecTy = Query.Types[0]; return std::pair(0, LLT::integer(VecTy.getSizeInBits())); }">>,
    CustomIf<AArch64_Pred_ce0a28a5>,
    ScalarizeIf<AArch64_Pred_359c5626, 0>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>
  ];
}

def AArch64_STORE : LegalizeRuleSet<["G_STORE"]> {
  let Rules = [
    CustomIf<AArch64_Pred_2bd673a4>,
    CustomIf<AArch64_Pred_9bc27123>,
    WidenScalarIf<AArch64_Pred_79336d5c, LegalizeRawMutation<"changeElementSizeTo(0, AArch64_s32)">>,
    LegalForMemDesc<[LegalizeMemDesc<AArch64_s8, AArch64_p0, AArch64_s8, 8>, LegalizeMemDesc<AArch64_s16, AArch64_p0, AArch64_s8, 8>, LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s8, 8>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s8, 8>, LegalizeMemDesc<AArch64_s16, AArch64_p0, AArch64_s16, 8>, LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s16, 8>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s16, 8>, LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s8, 8>, LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s16, 8>, LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s32, 8>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s32, 8>, LegalizeMemDesc<AArch64_p0, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_s128, AArch64_p0, AArch64_s128, 8>, LegalizeMemDesc<AArch64_v16s8, AArch64_p0, AArch64_s128, 8>, LegalizeMemDesc<AArch64_v8s8, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_v4s16, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_v8s16, AArch64_p0, AArch64_s128, 8>, LegalizeMemDesc<AArch64_v2s32, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_v4s32, AArch64_p0, AArch64_s128, 8>, LegalizeMemDesc<AArch64_v2s64, AArch64_p0, AArch64_s128, 8>]>,
    LegalForMemDesc<[LegalizeMemDesc<AArch64_nxv16s8, AArch64_p0, AArch64_nxv16s8, 8>, LegalizeMemDesc<AArch64_nxv8s16, AArch64_p0, AArch64_nxv8s16, 8>, LegalizeMemDesc<AArch64_nxv4s32, AArch64_p0, AArch64_nxv4s32, 8>, LegalizeMemDesc<AArch64_nxv2s64, AArch64_p0, AArch64_nxv2s64, 8>]>,
    ClampScalar<0, AArch64_s8, AArch64_s64>,
    MinScalarOrElt<0, AArch64_s8>,
    LowerIf<AArch64_Pred_b1fd2ec9>,
    ClampMaxNumElements<0, AArch64_s8, 16>,
    ClampMaxNumElements<0, AArch64_s16, 8>,
    ClampMaxNumElements<0, AArch64_s32, 4>,
    ClampMaxNumElements<0, AArch64_s64, 2>,
    ClampMaxNumElements<0, AArch64_p0, 2>,
    LowerIfMemSizeNotPow2,
    BitcastIf<AArch64_Pred_7d8fccce, LegalizeRawMutation<"[=](const LegalityQuery &Query) { const LLT VecTy = Query.Types[0]; return std::pair(0, LLT::integer(VecTy.getSizeInBits())); }">>,
    CustomIf<AArch64_Pred_ce0a28a5>,
    ScalarizeIf<AArch64_Pred_359c5626, 0>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    Lower
  ];
}

def AArch64_INDEXED_STORE : LegalizeRuleSet<["G_INDEXED_STORE"]> {
  let Rules = [
    LegalForMemDesc<[LegalizeMemDesc<AArch64_p0, AArch64_s8, AArch64_s8, 8>, LegalizeMemDesc<AArch64_p0, AArch64_s16, AArch64_s16, 8>, LegalizeMemDesc<AArch64_p0, AArch64_s32, AArch64_s8, 8>, LegalizeMemDesc<AArch64_p0, AArch64_s32, AArch64_s16, 8>, LegalizeMemDesc<AArch64_p0, AArch64_s32, AArch64_s32, 8>, LegalizeMemDesc<AArch64_p0, AArch64_s64, AArch64_s64, 8>, LegalizeMemDesc<AArch64_p0, AArch64_p0, AArch64_p0, 8>, LegalizeMemDesc<AArch64_p0, AArch64_v8s8, AArch64_v8s8, 8>, LegalizeMemDesc<AArch64_p0, AArch64_v16s8, AArch64_v16s8, 8>, LegalizeMemDesc<AArch64_p0, AArch64_v4s16, AArch64_v4s16, 8>, LegalizeMemDesc<AArch64_p0, AArch64_v8s16, AArch64_v8s16, 8>, LegalizeMemDesc<AArch64_p0, AArch64_v2s32, AArch64_v2s32, 8>, LegalizeMemDesc<AArch64_p0, AArch64_v4s32, AArch64_v4s32, 8>, LegalizeMemDesc<AArch64_p0, AArch64_v2s64, AArch64_v2s64, 8>, LegalizeMemDesc<AArch64_p0, AArch64_v2p0, AArch64_v2p0, 8>, LegalizeMemDesc<AArch64_p0, AArch64_s128, AArch64_s128, 8>]>,
    Unsupported
  ];
}

def AArch64_INDEXED_LOAD : LegalizeRuleSet<["G_INDEXED_LOAD"]> {
  let Rules = [
    UnsupportedIf<AArch64_Pred_2ac0a068>,
    LegalIf<AArch64_Pred_2b954739>,
    Unsupported
  ];
}

def AArch64_INDEXED_SEXTLOAD : LegalizeRuleSet<["G_INDEXED_SEXTLOAD", "G_INDEXED_ZEXTLOAD"]> {
  let Rules = [
    UnsupportedIf<AArch64_Pred_2ac0a068>,
    LegalIf<AArch64_Pred_8bd7def7>,
    Unsupported
  ];
}

def AArch64_CONSTANT : LegalizeRuleSet<["G_CONSTANT"]> {
  let Rules = [
    LegalFor<[AArch64_p0, AArch64_s8, AArch64_s16, AArch64_s32, AArch64_s64]>,
    WidenScalarToNextPow2<0>,
    ClampScalar<0, AArch64_s8, AArch64_s64>
  ];
}

def AArch64_FCONSTANT : LegalizeRuleSet<["G_FCONSTANT"]> {
  let Rules = [
    LegalFor<[AArch64_s16, AArch64_s32, AArch64_s64, AArch64_s128]>
  ];
}

def AArch64_ICMP : LegalizeRuleSet<["G_ICMP"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_i32, AArch64_i32>, LegalizeTypePair<AArch64_i32, AArch64_i64>, LegalizeTypePair<AArch64_i32, AArch64_p0>]>,
    WidenScalarOrEltToNextPow2<1>,
    MinScalarOrElt<1, AArch64_s8>,
    ClampScalar<1, AArch64_s32, AArch64_s64>,
    ClampScalar<0, AArch64_s32, AArch64_s32>,
    ScalarizeIf<AArch64_Pred_d9be76fb, 1>,
    MinScalarEltSameAsIf<AArch64_Pred_5789b262, 0, 1>,
    MinScalarOrEltIf<AArch64_Pred_458fc228, 1, AArch64_s32>,
    MinScalarOrEltIf<AArch64_Pred_cc1acbc6, 0, AArch64_s64>,
    MoreElementsToNextPow2<1>,
    ClampNumElements<1, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<1, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<1, AArch64_v2s32, AArch64_v4s32>,
    ClampNumElements<1, AArch64_v2s64, AArch64_v2s64>,
    ClampNumElements<1, AArch64_v2p0, AArch64_v2p0>,
    CustomIf<AArch64_Pred_bb2dbe0b>
  ];
}

def AArch64_FCMP : LegalizeRuleSet<["G_FCMP"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_i32, AArch64_f32>, LegalizeTypePair<AArch64_i32, AArch64_f64>, LegalizeTypePair<AArch64_v4i32, AArch64_v4f32>, LegalizeTypePair<AArch64_v2i32, AArch64_v2f32>, LegalizeTypePair<AArch64_v2i64, AArch64_v2f64>]>,
    LegalForPairsIf<AArch64_HasFP16, [LegalizeTypePair<AArch64_i32, AArch64_f16>, LegalizeTypePair<AArch64_v4i16, AArch64_v4f16>, LegalizeTypePair<AArch64_v8i16, AArch64_v8f16>]>,
    WidenScalarOrEltToNextPow2<1>,
    ClampScalar<0, AArch64_s32, AArch64_s32>,
    WidenScalarIf<AArch64_Pred_959eb4fe, LegalizeRawMutation<"changeElementTo(1, AArch64_f32)">>,
    ScalarizeIf<AArch64_Pred_d9be76fb, 1>,
    MinScalarEltSameAsIf<AArch64_Pred_5789b262, 0, 1>,
    ClampNumElements<1, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<1, AArch64_v2s32, AArch64_v4s32>,
    ClampMaxNumElements<1, AArch64_s64, 2>,
    MoreElementsToNextPow2<1>,
    LibcallForPairs<[LegalizeTypePair<AArch64_s32, AArch64_s128>]>
  ];
}

def AArch64_ZEXT : LegalizeRuleSet<["G_ZEXT", "G_SEXT", "G_ANYEXT"]> {
  let Rules = [
    LegalIf<AArch64_Pred_e2acf90b>,
    LegalForPairs<[LegalizeTypePair<AArch64_v8s16, AArch64_v8s8>, LegalizeTypePair<AArch64_v4s32, AArch64_v4s16>, LegalizeTypePair<AArch64_v2s64, AArch64_v2s32>]>,
    ClampScalar<0, AArch64_s64, AArch64_s64>,
    MoreElementsToNextPow2<0>,
    ClampMaxNumElements<1, AArch64_s8, 8>,
    ClampMaxNumElements<1, AArch64_s16, 4>,
    ClampMaxNumElements<1, AArch64_s32, 2>,
    LowerIf<AArch64_Pred_38f29bd4>,
    ClampMinNumElements<1, AArch64_s8, 8>,
    ClampMinNumElements<1, AArch64_s16, 4>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>
  ];
}

def AArch64_TRUNC : LegalizeRuleSet<["G_TRUNC"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_v8s8, AArch64_v8s16>, LegalizeTypePair<AArch64_v4s16, AArch64_v4s32>, LegalizeTypePair<AArch64_v2s32, AArch64_v2s64>]>,
    MoreElementsToNextPow2<0>,
    ClampMaxNumElements<0, AArch64_s8, 8>,
    ClampMaxNumElements<0, AArch64_s16, 4>,
    ClampMaxNumElements<0, AArch64_s32, 2>,
    MinScalarOrEltIf<AArch64_Pred_1f3dc25d, 0, AArch64_s8>,
    LowerIf<AArch64_Pred_6dcb14b4>,
    ClampMinNumElements<0, AArch64_s8, 8>,
    ClampMinNumElements<0, AArch64_s16, 4>,
    AlwaysLegal
  ];
}

def AArch64_TRUNC_SSAT_S : LegalizeRuleSet<["G_TRUNC_SSAT_S", "G_TRUNC_SSAT_U", "G_TRUNC_USAT_U"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_v8i8, AArch64_v8i16>, LegalizeTypePair<AArch64_v4i16, AArch64_v4i32>, LegalizeTypePair<AArch64_v2i32, AArch64_v2i64>]>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v8s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v4s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v2s32>,
    Lower
  ];
}

def AArch64_SEXT_INREG : LegalizeRuleSet<["G_SEXT_INREG"]> {
  let Rules = [
    LegalFor<[AArch64_i32, AArch64_i64, AArch64_v8i8, AArch64_v16i8, AArch64_v4i16, AArch64_v8i16, AArch64_v2i32, AArch64_v4i32, AArch64_v2i64]>,
    MaxScalar<0, AArch64_s64>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampMaxNumElements<0, AArch64_s64, 2>,
    Lower
  ];
}

def AArch64_FPTRUNC : LegalizeRuleSet<["G_FPTRUNC"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_f16, AArch64_f32>, LegalizeTypePair<AArch64_f16, AArch64_f64>, LegalizeTypePair<AArch64_f32, AArch64_f64>, LegalizeTypePair<AArch64_v4f16, AArch64_v4f32>, LegalizeTypePair<AArch64_v2f32, AArch64_v2f64>]>,
    LegalForPairsIf<AArch64_HasBF16, [LegalizeTypePair<AArch64_bf16, AArch64_f32>, LegalizeTypePair<AArch64_v4bf16, AArch64_v4f32>]>,
    LibcallForPairs<[LegalizeTypePair<AArch64_f16, AArch64_f128>, LegalizeTypePair<AArch64_f32, AArch64_f128>, LegalizeTypePair<AArch64_f64, AArch64_f128>]>,
    MoreElementsToNextPow2<1>,
    CustomIf<AArch64_Pred_1671daba>,
    LowerForPairs<[LegalizeTypePair<AArch64_bf16, AArch64_f32>, LegalizeTypePair<AArch64_v4bf16, AArch64_v4f32>]>,
    ClampNumElements<1, AArch64_v4s32, AArch64_v4s32>,
    ClampNumElements<1, AArch64_v2s64, AArch64_v2s64>,
    Scalarize<0>
  ];
}

def AArch64_FPEXT : LegalizeRuleSet<["G_FPEXT"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_f32, AArch64_f16>, LegalizeTypePair<AArch64_f64, AArch64_f16>, LegalizeTypePair<AArch64_f32, AArch64_bf16>, LegalizeTypePair<AArch64_f64, AArch64_f32>, LegalizeTypePair<AArch64_v4f32, AArch64_v4f16>, LegalizeTypePair<AArch64_v4f32, AArch64_v4bf16>, LegalizeTypePair<AArch64_v2f64, AArch64_v2f32>]>,
    LibcallForPairs<[LegalizeTypePair<AArch64_f128, AArch64_f64>, LegalizeTypePair<AArch64_f128, AArch64_f32>, LegalizeTypePair<AArch64_f128, AArch64_f16>]>,
    MoreElementsToNextPow2<0>,
    WidenScalarIf<AArch64_Pred_11e45701, LegalizeRawMutation<"changeElementTo(1, AArch64_f32)">>,
    ClampNumElements<0, AArch64_v4s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    Scalarize<0>
  ];
}

def AArch64_FPTOSI : LegalizeRuleSet<["G_FPTOSI", "G_FPTOUI"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_i32, AArch64_f32>, LegalizeTypePair<AArch64_i64, AArch64_f32>, LegalizeTypePair<AArch64_i32, AArch64_f64>, LegalizeTypePair<AArch64_i64, AArch64_f64>, LegalizeTypePair<AArch64_v2i32, AArch64_v2f32>, LegalizeTypePair<AArch64_v4i32, AArch64_v4f32>, LegalizeTypePair<AArch64_v2i64, AArch64_v2f64>]>,
    LegalForPairsIf<AArch64_HasFP16, [LegalizeTypePair<AArch64_i32, AArch64_f16>, LegalizeTypePair<AArch64_i64, AArch64_f16>, LegalizeTypePair<AArch64_v4i16, AArch64_v4f16>, LegalizeTypePair<AArch64_v8i16, AArch64_v8f16>]>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    ScalarizeIf<AArch64_Pred_d9be76fb, 1>,
    NarrowScalarIf<AArch64_Pred_cb649b90, LegalizeRawMutation<"changeTo(0, AArch64_i64)">>,
    MoreElementsToNextPow2<0>,
    WidenScalarOrEltToNextPow2OrMinSize<0>,
    MinScalar<0, AArch64_s32>,
    WidenScalarIf<AArch64_Pred_e69215a4, LegalizeRawMutation<"changeElementTo(1, AArch64_f32)">>,
    WidenScalarIf<AArch64_Pred_51f3473b, LegalizeRawMutation<"LegalizeMutations::changeElementSizeTo(1, 0)">>,
    WidenScalarIf<AArch64_Pred_661419a9, LegalizeRawMutation<"LegalizeMutations::changeElementSizeTo(0, 1)">>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampMaxNumElements<0, AArch64_s64, 2>,
    LibcallForPairs<[LegalizeTypePair<AArch64_i32, AArch64_f128>, LegalizeTypePair<AArch64_i64, AArch64_f128>, LegalizeTypePair<AArch64_i128, AArch64_f128>, LegalizeTypePair<AArch64_i128, AArch64_f32>, LegalizeTypePair<AArch64_i128, AArch64_f64>]>
  ];
}

def AArch64_FPTOSI_SAT : LegalizeRuleSet<["G_FPTOSI_SAT", "G_FPTOUI_SAT"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_i32, AArch64_f32>, LegalizeTypePair<AArch64_i64, AArch64_f32>, LegalizeTypePair<AArch64_i32, AArch64_f64>, LegalizeTypePair<AArch64_i64, AArch64_f64>, LegalizeTypePair<AArch64_v2i32, AArch64_v2f32>, LegalizeTypePair<AArch64_v4i32, AArch64_v4f32>, LegalizeTypePair<AArch64_v2i64, AArch64_v2f64>]>,
    LegalForPairsIf<AArch64_HasFP16, [LegalizeTypePair<AArch64_i16, AArch64_f16>, LegalizeTypePair<AArch64_i32, AArch64_f16>, LegalizeTypePair<AArch64_i64, AArch64_f16>, LegalizeTypePair<AArch64_v4i16, AArch64_v4f16>, LegalizeTypePair<AArch64_v8i16, AArch64_v8f16>]>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    ScalarizeIf<AArch64_Pred_d9be76fb, 1>,
    NarrowScalarIf<AArch64_Pred_cb649b90, LegalizeRawMutation<"changeTo(0, AArch64_i64)">>,
    LowerIf<AArch64_Pred_20db2dd3>,
    MoreElementsToNextPow2<0>,
    WidenScalarToNextPow2<0, /*MinSize=*/32>,
    MinScalar<0, AArch64_s32>,
    WidenScalarIf<AArch64_Pred_e69215a4, LegalizeRawMutation<"changeElementTo(1, AArch64_f32)">>,
    WidenScalarIf<AArch64_Pred_61c58b0b, LegalizeRawMutation<"LegalizeMutations::changeElementSizeTo(1, 0)">>,
    WidenScalarIf<AArch64_Pred_99ace828, LegalizeRawMutation<"LegalizeMutations::changeElementSizeTo(0, 1)">>,
    WidenScalarOrEltToNextPow2<0>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampMaxNumElements<0, AArch64_s64, 2>
  ];
}

def AArch64_SITOFP : LegalizeRuleSet<["G_SITOFP", "G_UITOFP"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_f32, AArch64_i32>, LegalizeTypePair<AArch64_f64, AArch64_i32>, LegalizeTypePair<AArch64_f32, AArch64_i64>, LegalizeTypePair<AArch64_f64, AArch64_i64>, LegalizeTypePair<AArch64_v2f32, AArch64_v2i32>, LegalizeTypePair<AArch64_v4f32, AArch64_v4i32>, LegalizeTypePair<AArch64_v2f64, AArch64_v2i64>]>,
    LegalForPairsIf<AArch64_HasFP16, [LegalizeTypePair<AArch64_f16, AArch64_i32>, LegalizeTypePair<AArch64_f16, AArch64_i64>, LegalizeTypePair<AArch64_v4f16, AArch64_v4i16>, LegalizeTypePair<AArch64_v8f16, AArch64_v8i16>]>,
    UnsupportedIf<AArch64_Pred_a10e207c>,
    ScalarizeIf<AArch64_Pred_d9be76fb, 1>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    MoreElementsToNextPow2<1>,
    WidenScalarOrEltToNextPow2OrMinSize<1>,
    MinScalar<1, AArch64_f32>,
    LowerIf<AArch64_Pred_938ab9ea>,
    CoreWiden<(Slot 0), (CoreSelect AArch64_HasFP16, 16, 32)>,
    ScalarizeIf<AArch64_Pred_47fbed85, 0>,
    WidenScalarIf<AArch64_Pred_e47b6b4a, LegalizeRawMutation<"LegalizeMutations::changeElementSizeTo(0, 1)">>,
    WidenScalarIf<AArch64_Pred_92f28716, LegalizeRawMutation<"LegalizeMutations::changeElementSizeTo(1, 0)">>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampMaxNumElements<0, AArch64_s64, 2>,
    LibcallForPairs<[LegalizeTypePair<AArch64_f16, AArch64_i128>, LegalizeTypePair<AArch64_f32, AArch64_i128>, LegalizeTypePair<AArch64_f64, AArch64_i128>, LegalizeTypePair<AArch64_f128, AArch64_i128>, LegalizeTypePair<AArch64_f128, AArch64_i32>, LegalizeTypePair<AArch64_f128, AArch64_i64>]>
  ];
}

def AArch64_BR : LegalizeRuleSet<["G_BR"]> {
  let Rules = [
    AlwaysLegal
  ];
}

def AArch64_BRCOND : LegalizeRuleSet<["G_BRCOND"]> {
  let Rules = [
    LegalFor<[AArch64_s32]>,
    ClampScalar<0, AArch64_s32, AArch64_s32>
  ];
}

def AArch64_BRINDIRECT : LegalizeRuleSet<["G_BRINDIRECT"]> {
  let Rules = [
    LegalFor<[AArch64_p0]>
  ];
}

def AArch64_SELECT : LegalizeRuleSet<["G_SELECT"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_s32, AArch64_s32>, LegalizeTypePair<AArch64_s64, AArch64_s32>, LegalizeTypePair<AArch64_p0, AArch64_s32>]>,
    WidenScalarToNextPow2<0>,
    ClampScalar<0, AArch64_s32, AArch64_s64>,
    ClampScalar<1, AArch64_s32, AArch64_s32>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    MinScalarEltSameAsIf<AArch64_Pred_84602781, 1, 0>,
    LowerIf<AArch64_Pred_bb2dbe0b>
  ];
}

def AArch64_FRAME_INDEX : LegalizeRuleSet<["G_FRAME_INDEX"]> {
  let Rules = [
    LegalFor<[AArch64_p0]>
  ];
}

def AArch64_GLOBAL_VALUE : LegalizeRuleSet<["G_GLOBAL_VALUE"]> {
  let Rules = [
    CustomIf<AArch64_SmallCodeModelQuery>,
    LegalIf<AArch64_NotSmallCodeModelQuery>
  ];
}

def AArch64_PTRAUTH_GLOBAL_VALUE : LegalizeRuleSet<["G_PTRAUTH_GLOBAL_VALUE"]> {
  let Rules = [
    LegalIf<AArch64_Pred_8bbda3e4>
  ];
}

def AArch64_PTRTOINT : LegalizeRuleSet<["G_PTRTOINT"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_i64, AArch64_p0>, LegalizeTypePair<AArch64_v2i64, AArch64_v2p0>]>,
    WidenScalarToNextPow2<0, 64>,
    ClampScalar<0, AArch64_s64, AArch64_s64>,
    ClampMaxNumElements<0, AArch64_s64, 2>
  ];
}

def AArch64_INTTOPTR : LegalizeRuleSet<["G_INTTOPTR"]> {
  let Rules = [
    UnsupportedIf<AArch64_Pred_fd9b821d>,
    LegalForPairs<[LegalizeTypePair<AArch64_p0, AArch64_i64>, LegalizeTypePair<AArch64_v2p0, AArch64_v2i64>]>,
    ClampMaxNumElements<1, AArch64_s64, 2>
  ];
}

def AArch64_BITCAST : LegalizeRuleSet<["G_BITCAST"]> {
  let Rules = [
    LegalForCartesianProduct1<LegalizeInlineTypeList<[AArch64_s16]>>,
    LegalForCartesianProduct1<LegalizeInlineTypeList<[AArch64_s32, AArch64_v2s16, AArch64_v4s8]>>,
    LegalForCartesianProduct1<LegalizeInlineTypeList<[AArch64_s64, AArch64_v8s8, AArch64_v4s16, AArch64_v2s32]>>,
    LegalForCartesianProduct1<LegalizeInlineTypeList<[AArch64_s128, AArch64_v16s8, AArch64_v8s16, AArch64_v4s32, AArch64_v2s64, AArch64_v2p0]>>,
    CustomIf<AArch64_Pred_f41b94df>,
    LowerIf<AArch64_Pred_125bf639>,
    LowerIf<AArch64_Pred_06878545>,
    MoreElementsToNextPow2<0>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampMaxNumElements<0, AArch64_s64, 2>,
    Lower
  ];
}

def AArch64_VASTART : LegalizeRuleSet<["G_VASTART"]> {
  let Rules = [
    LegalFor<[AArch64_p0]>
  ];
}

def AArch64_VAARG : LegalizeRuleSet<["G_VAARG"]> {
  let Rules = [
    CustomForCartesianProduct<LegalizeInlineTypeList<[AArch64_s8, AArch64_s16, AArch64_s32, AArch64_s64, AArch64_p0]>, LegalizeInlineTypeList<[AArch64_p0]>>,
    ClampScalar<0, AArch64_s8, AArch64_s64>,
    WidenScalarToNextPow2<0, /*Min*/ 8>
  ];
}

def AArch64_ATOMIC_CMPXCHG_WITH_SUCCESS : LegalizeRuleSet<["G_ATOMIC_CMPXCHG_WITH_SUCCESS"]> {
  let Rules = [
    LowerIf<AArch64_Pred_b36dc304>
  ];
}

def AArch64_ATOMIC_CMPXCHG : LegalizeRuleSet<["G_ATOMIC_CMPXCHG"]> {
  let Rules = [
    LegalForPairsIf<AArch64_NotUseOutlineAtomics, [LegalizeTypePair<AArch64_s32, AArch64_p0>, LegalizeTypePair<AArch64_s64, AArch64_p0>]>,
    CustomForPairsIf<AArch64_NotUseOutlineAtomics, [LegalizeTypePair<AArch64_s128, AArch64_p0>]>,
    LibcallForPairsIf<AArch64_UseOutlineAtomics, [LegalizeTypePair<AArch64_s8, AArch64_p0>, LegalizeTypePair<AArch64_s16, AArch64_p0>, LegalizeTypePair<AArch64_s32, AArch64_p0>, LegalizeTypePair<AArch64_s64, AArch64_p0>, LegalizeTypePair<AArch64_s128, AArch64_p0>]>,
    ClampScalar<0, AArch64_s32, AArch64_s64>
  ];
}

def AArch64_ATOMICRMW_XCHG : LegalizeRuleSet<["G_ATOMICRMW_XCHG", "G_ATOMICRMW_ADD", "G_ATOMICRMW_SUB", "G_ATOMICRMW_AND", "G_ATOMICRMW_OR", "G_ATOMICRMW_XOR"]> {
  let Rules = [
    LegalForPairsIf<AArch64_NotUseOutlineAtomics, [LegalizeTypePair<AArch64_s32, AArch64_p0>, LegalizeTypePair<AArch64_s64, AArch64_p0>]>,
    LibcallForPairsIf<AArch64_UseOutlineAtomics, [LegalizeTypePair<AArch64_s8, AArch64_p0>, LegalizeTypePair<AArch64_s16, AArch64_p0>, LegalizeTypePair<AArch64_s32, AArch64_p0>, LegalizeTypePair<AArch64_s64, AArch64_p0>]>,
    ClampScalar<0, AArch64_s32, AArch64_s64>
  ];
}

def AArch64_ATOMICRMW_MIN : LegalizeRuleSet<["G_ATOMICRMW_MIN", "G_ATOMICRMW_MAX", "G_ATOMICRMW_UMIN", "G_ATOMICRMW_UMAX"]> {
  let Rules = [
    LegalIf<AArch64_Pred_2d4385e9>,
    ClampScalar<0, AArch64_s32, AArch64_s64>
  ];
}

def AArch64_BLOCK_ADDR : LegalizeRuleSet<["G_BLOCK_ADDR"]> {
  let Rules = [
    LegalFor<[AArch64_p0]>
  ];
}

def AArch64_EXTRACT_VECTOR_ELT : LegalizeRuleSet<["G_EXTRACT_VECTOR_ELT"]> {
  let Rules = [
    LegalForTriplesIf<AArch64_HasSVE, [LegalizeTypeTriple<AArch64_s16, AArch64_nxv16s8, AArch64_s64>, LegalizeTypeTriple<AArch64_s16, AArch64_nxv8s16, AArch64_s64>, LegalizeTypeTriple<AArch64_s32, AArch64_nxv4s32, AArch64_s64>, LegalizeTypeTriple<AArch64_s64, AArch64_nxv2s64, AArch64_s64>]>,
    UnsupportedIf<AArch64_Pred_bf12db7c>,
    MinScalar<2, AArch64_s64>,
    CustomIf<AArch64_Pred_a39de19a>,
    MinScalarOrEltIf<AArch64_Pred_4472211a, 0, AArch64_s64>,
    MinScalarOrEltIf<AArch64_Pred_8883238c, 0, AArch64_s32>,
    MinScalarOrEltIf<AArch64_Pred_92a15f60, 0, AArch64_s16>,
    MinScalarOrEltIf<AArch64_Pred_0dda87c5, 0, AArch64_s8>,
    MinScalarOrElt<0, AArch64_s8>,
    MoreElementsToNextPow2<1>,
    ClampMaxNumElements<1, AArch64_s64, 2>,
    ClampMaxNumElements<1, AArch64_s32, 4>,
    ClampMaxNumElements<1, AArch64_s16, 8>,
    ClampMaxNumElements<1, AArch64_s8, 16>,
    ClampMaxNumElements<1, AArch64_p0, 2>,
    ScalarizeIf<AArch64_Pred_d9be76fb, 1>
  ];
}

def AArch64_INSERT_VECTOR_ELT : LegalizeRuleSet<["G_INSERT_VECTOR_ELT"]> {
  let Rules = [
    LegalIf<AArch64_Pred_1a80c04b>,
    LegalForTriplesIf<AArch64_HasSVE, [LegalizeTypeTriple<AArch64_nxv16s8, AArch64_s32, AArch64_s64>, LegalizeTypeTriple<AArch64_nxv8s16, AArch64_s32, AArch64_s64>, LegalizeTypeTriple<AArch64_nxv4s32, AArch64_s32, AArch64_s64>, LegalizeTypeTriple<AArch64_nxv2s64, AArch64_s64, AArch64_s64>]>,
    MoreElementsToNextPow2<0>,
    CoreWiden<(ElemOf (Slot 0)), (MinVectorSize 64)>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampMaxNumElements<0, AArch64_s64, 2>,
    ClampMaxNumElements<0, AArch64_p0, 2>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>
  ];
}

def AArch64_BUILD_VECTOR : LegalizeRuleSet<["G_BUILD_VECTOR"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_v8s8, AArch64_s8>, LegalizeTypePair<AArch64_v16s8, AArch64_s8>, LegalizeTypePair<AArch64_v4s16, AArch64_s16>, LegalizeTypePair<AArch64_v8s16, AArch64_s16>, LegalizeTypePair<AArch64_v2s32, AArch64_s32>, LegalizeTypePair<AArch64_v4s32, AArch64_s32>, LegalizeTypePair<AArch64_v2s64, AArch64_s64>, LegalizeTypePair<AArch64_v2p0, AArch64_p0>]>,
    ClampNumElements<0, AArch64_v4s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    MinScalarOrElt<0, AArch64_s8>,
    CoreWiden<(ElemOf (Slot 0)), (MinVectorSize 64)>,
    WidenScalarOrEltToNextPow2<0>,
    MinScalarSameAs<1, 0>
  ];
}

def AArch64_BUILD_VECTOR_TRUNC : LegalizeRuleSet<["G_BUILD_VECTOR_TRUNC"]> {
  let Rules = [
    Lower
  ];
}

def AArch64_SPLAT_VECTOR : LegalizeRuleSet<["G_SPLAT_VECTOR"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_v8s8, AArch64_s8>, LegalizeTypePair<AArch64_v8s8, AArch64_s32>, LegalizeTypePair<AArch64_v16s8, AArch64_s8>, LegalizeTypePair<AArch64_v16s8, AArch64_s32>, LegalizeTypePair<AArch64_v4s16, AArch64_s16>, LegalizeTypePair<AArch64_v4s16, AArch64_s32>, LegalizeTypePair<AArch64_v8s16, AArch64_s16>, LegalizeTypePair<AArch64_v8s16, AArch64_s32>, LegalizeTypePair<AArch64_v2s32, AArch64_s32>, LegalizeTypePair<AArch64_v4s32, AArch64_s32>, LegalizeTypePair<AArch64_v2s64, AArch64_s64>, LegalizeTypePair<AArch64_v2p0, AArch64_p0>]>,
    LegalForPairsIf<AArch64_HasSVE, [LegalizeTypePair<AArch64_nxv4s32, AArch64_s32>, LegalizeTypePair<AArch64_nxv2s64, AArch64_s64>]>
  ];
}

def AArch64_SHUFFLE_VECTOR : LegalizeRuleSet<["G_SHUFFLE_VECTOR"]> {
  let Rules = [
    LegalIf<AArch64_Pred_971c0f17>,
    MoreElementsIf<AArch64_Pred_7727ed82, LegalizeRawMutation<"changeTo(1, 0)">>,
    MoreElementsToNextPow2<0>,
    MoreElementsIf<AArch64_Pred_f0403665, LegalizeRawMutation<"changeTo(0, 1)">>,
    WidenScalarOrEltToNextPow2OrMinSize<0, 8>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v4s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    ScalarizeIf<AArch64_Pred_f4c34fb1, 0>,
    BitcastIf<AArch64_Pred_5cc26dc8, LegalizeRawMutation<"[=](const LegalityQuery &Query) { const LLT DstTy = Query.Types[0]; return std::pair( 0, LLT::vector(DstTy.getElementCount(), LLT::integer(64))); }">>
  ];
}

def AArch64_CONCAT_VECTORS : LegalizeRuleSet<["G_CONCAT_VECTORS"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_v16s8, AArch64_v8s8>, LegalizeTypePair<AArch64_v8s16, AArch64_v4s16>, LegalizeTypePair<AArch64_v4s32, AArch64_v2s32>]>,
    CustomIf<AArch64_Pred_6bbd1890>,
    BitcastIf<AArch64_Pred_5b3dda4d, LegalizeRawMutation<"[=](const LegalityQuery &Query) { const LLT DstTy = Query.Types[0]; const LLT SrcTy = Query.Types[1]; return std::pair( 0, DstTy.changeElementSize(SrcTy.getSizeInBits()) .changeElementCount( DstTy.getElementCount().divideCoefficientBy( SrcTy.getNumElements()))); }">>
  ];
}

def AArch64_EXTRACT_SUBVECTOR : LegalizeRuleSet<["G_EXTRACT_SUBVECTOR"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_v8s8, AArch64_v16s8>, LegalizeTypePair<AArch64_v4s16, AArch64_v8s16>, LegalizeTypePair<AArch64_v2s32, AArch64_v4s32>]>,
    WidenScalarOrEltToNextPow2<0>,
    ClampMaxNumElements<0, AArch64_s8, 16>,
    ClampMaxNumElements<0, AArch64_s16, 8>,
    ClampMaxNumElements<0, AArch64_s32, 4>,
    ClampNumElements<1, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<1, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<1, AArch64_v2s32, AArch64_v4s32>,
    Lower,
    ImmIdx<0>
  ];
}

def AArch64_JUMP_TABLE : LegalizeRuleSet<["G_JUMP_TABLE"]> {
  let Rules = [
    LegalFor<[AArch64_p0]>
  ];
}

def AArch64_BRJT : LegalizeRuleSet<["G_BRJT"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_p0, AArch64_s64>]>
  ];
}

def AArch64_TRAP : LegalizeRuleSet<["G_TRAP", "G_DEBUGTRAP", "G_UBSANTRAP"]> {
  let Rules = [
    AlwaysLegal
  ];
}

def AArch64_DYN_STACKALLOC : LegalizeRuleSet<["G_DYN_STACKALLOC"]> {
  let Rules = [
    Custom
  ];
}

def AArch64_STACKSAVE : LegalizeRuleSet<["G_STACKSAVE", "G_STACKRESTORE"]> {
  let Rules = [
    Lower
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
    UnsupportedIf<AArch64_Pred_HasMOPS>,
    LibcallIf<AArch64_Pred_NotHasMOPS>
  ];
}

def AArch64_MEMSET : LegalizeRuleSet<["G_MEMSET"]> {
  let Rules = [
    LegalIf<AArch64_Pred_MemSetLegal>,
    CustomIf<AArch64_Pred_MemSetCustom>,
    LibcallIf<AArch64_Pred_NotHasMOPS>,
    // immIdx() returns unsigned, so it has to end the emitted chain.
    ImmIdx<0>
  ];
}

def AArch64_MEMCPY : LegalizeRuleSet<["G_MEMCPY", "G_MEMMOVE"]> {
  let Rules = [
    LegalIf<AArch64_Pred_MemCpyLegal>,
    LibcallIf<AArch64_Pred_NotHasMOPS>,
    ImmIdx<0>
  ];
}

def AArch64_MEMCPY_INLINE : LegalizeRuleSet<["G_MEMCPY_INLINE"]> {
  let Rules = [
    LegalIf<AArch64_Pred_MemCpyLegal>
  ];
}

def AArch64_MEMSET_INLINE : LegalizeRuleSet<["G_MEMSET_INLINE"]> {
  let Rules = [
    LegalIf<AArch64_Pred_MemSetInlineLegal>,
    CustomIf<AArch64_Pred_MemSetInlineCustom>
  ];
}

def AArch64_VECREDUCE_FADD : LegalizeRuleSet<["G_VECREDUCE_FADD"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_f32, AArch64_v2f32>, LegalizeTypePair<AArch64_f32, AArch64_v4f32>, LegalizeTypePair<AArch64_f64, AArch64_v2f64>]>,
    LegalForPairsIf<AArch64_HasFP16, [LegalizeTypePair<AArch64_f16, AArch64_v4f16>, LegalizeTypePair<AArch64_f16, AArch64_v8f16>]>,
    WidenScalarIf<AArch64_Pred_0226be53, LegalizeRawMutation<"changeElementTo(0, AArch64_f32)">>,
    ClampMaxNumElements<1, AArch64_s64, 2>,
    ClampMaxNumElements<1, AArch64_s32, 4>,
    ClampMaxNumElements<1, AArch64_s16, 8>,
    MoreElementsToNextPow2<1>,
    Scalarize<1>,
    Lower
  ];
}

def AArch64_VECREDUCE_FMUL : LegalizeRuleSet<["G_VECREDUCE_FMUL"]> {
  let Rules = [
    WidenScalarIf<AArch64_Pred_0226be53, LegalizeRawMutation<"changeElementTo(0, AArch64_f32)">>,
    ClampMaxNumElements<1, AArch64_s64, 2>,
    ClampMaxNumElements<1, AArch64_s32, 4>,
    ClampMaxNumElements<1, AArch64_s16, 8>,
    ClampMaxNumElements<1, AArch64_s32, 2>,
    ClampMaxNumElements<1, AArch64_s16, 4>,
    Scalarize<1>,
    Lower
  ];
}

def AArch64_VECREDUCE_SEQ_FADD : LegalizeRuleSet<["G_VECREDUCE_SEQ_FADD", "G_VECREDUCE_SEQ_FMUL"]> {
  let Rules = [
    Scalarize<2>,
    Lower
  ];
}

def AArch64_VECREDUCE_ADD : LegalizeRuleSet<["G_VECREDUCE_ADD"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_i8, AArch64_v8i8>, LegalizeTypePair<AArch64_i8, AArch64_v16i8>, LegalizeTypePair<AArch64_i16, AArch64_v4i16>, LegalizeTypePair<AArch64_i16, AArch64_v8i16>, LegalizeTypePair<AArch64_i32, AArch64_v2i32>, LegalizeTypePair<AArch64_i32, AArch64_v4i32>, LegalizeTypePair<AArch64_i64, AArch64_v2i64>]>,
    MoreElementsToNextPow2<1>,
    ClampMaxNumElements<1, AArch64_s64, 2>,
    ClampMaxNumElements<1, AArch64_s32, 4>,
    ClampMaxNumElements<1, AArch64_s16, 8>,
    ClampMaxNumElements<1, AArch64_s8, 16>,
    CoreWiden<(ElemOf (Slot 1)), (MinVectorSize 64)>,
    Scalarize<1>
  ];
}

def AArch64_VECREDUCE_FMIN : LegalizeRuleSet<["G_VECREDUCE_FMIN", "G_VECREDUCE_FMAX", "G_VECREDUCE_FMINIMUM", "G_VECREDUCE_FMAXIMUM"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_f32, AArch64_v2f32>, LegalizeTypePair<AArch64_f32, AArch64_v4f32>, LegalizeTypePair<AArch64_f64, AArch64_v2f64>]>,
    LegalForPairsIf<AArch64_HasFP16, [LegalizeTypePair<AArch64_f16, AArch64_v4f16>, LegalizeTypePair<AArch64_f16, AArch64_v8f16>]>,
    WidenScalarIf<AArch64_Pred_0226be53, LegalizeRawMutation<"changeElementTo(0, AArch64_f32)">>,
    ClampMaxNumElements<1, AArch64_s64, 2>,
    ClampMaxNumElements<1, AArch64_s32, 4>,
    ClampMaxNumElements<1, AArch64_s16, 8>,
    Scalarize<1>,
    Lower
  ];
}

def AArch64_VECREDUCE_MUL : LegalizeRuleSet<["G_VECREDUCE_MUL"]> {
  let Rules = [
    ClampMaxNumElements<1, AArch64_s32, 2>,
    ClampMaxNumElements<1, AArch64_s16, 4>,
    ClampMaxNumElements<1, AArch64_s8, 8>,
    Scalarize<1>,
    Lower
  ];
}

def AArch64_VECREDUCE_SMIN : LegalizeRuleSet<["G_VECREDUCE_SMIN", "G_VECREDUCE_SMAX", "G_VECREDUCE_UMIN", "G_VECREDUCE_UMAX"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_i8, AArch64_v8i8>, LegalizeTypePair<AArch64_i8, AArch64_v16i8>, LegalizeTypePair<AArch64_i16, AArch64_v4i16>, LegalizeTypePair<AArch64_i16, AArch64_v8i16>, LegalizeTypePair<AArch64_i32, AArch64_v2i32>, LegalizeTypePair<AArch64_i32, AArch64_v4i32>]>,
    MoreElementsIf<AArch64_Pred_12d65c07, LegalizeRawMutation<"LegalizeMutations::moreElementsToNextPow2(1)">>,
    ClampMaxNumElements<1, AArch64_s64, 2>,
    ClampMaxNumElements<1, AArch64_s32, 4>,
    ClampMaxNumElements<1, AArch64_s16, 8>,
    ClampMaxNumElements<1, AArch64_s8, 16>,
    Scalarize<1>,
    Lower
  ];
}

def AArch64_VECREDUCE_OR : LegalizeRuleSet<["G_VECREDUCE_OR", "G_VECREDUCE_AND", "G_VECREDUCE_XOR"]> {
  let Rules = [
    FewerElementsIf<AArch64_Pred_131ecb96, LegalizeRawMutation<"[=](const LegalityQuery &Q) { LLT SrcTy = Q.Types[1]; return std::make_pair(1, SrcTy.divide(2)); }">>,
    Scalarize<1>,
    Lower
  ];
}

def AArch64_VECTOR_COMPRESS : LegalizeRuleSet<["G_VECTOR_COMPRESS"]> {
  let Rules = [
    Lower
  ];
}

def AArch64_GET_FPENV : LegalizeRuleSet<["G_GET_FPENV", "G_SET_FPENV", "G_RESET_FPENV", "G_GET_FPMODE", "G_SET_FPMODE", "G_RESET_FPMODE"]> {
  let Rules = [
    Libcall
  ];
}

def AArch64_GET_ROUNDING : LegalizeRuleSet<["G_GET_ROUNDING", "G_SET_ROUNDING"]> {
  let Rules = [
    CustomFor<[AArch64_s32]>
  ];
}

def AArch64_IS_FPCLASS : LegalizeRuleSet<["G_IS_FPCLASS"]> {
  let Rules = [
    Lower
  ];
}

def AArch64_PREFETCH : LegalizeRuleSet<["G_PREFETCH"]> {
  let Rules = [
    Custom
  ];
}

def AArch64_SCMP : LegalizeRuleSet<["G_SCMP", "G_UCMP"]> {
  let Rules = [
    Lower
  ];
}

def AArch64_INTRINSIC : LegalizeRuleSet<["G_INTRINSIC", "G_INTRINSIC_W_SIDE_EFFECTS"]> {
  let Rules = [
    AlwaysLegal
  ];
}

def AArch64_FENCE : LegalizeRuleSet<["G_FENCE"]> {
  let Rules = [
    AlwaysLegal
  ];
}

def AArch64_INVOKE_REGION_START : LegalizeRuleSet<["G_INVOKE_REGION_START"]> {
  let Rules = [
    AlwaysLegal
  ];
}

def AArch64_SEXTLOAD : LegalizeRuleSet<["G_SEXTLOAD"]> {
  let Rules = [
    LowerIf<AArch64_Pred_2ac0a068>,
    LegalForMemDesc<[LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s8, 8>, LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s16, 8>, LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s32, 8>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s8, 2>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s16, 2>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s32, 4>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_p0, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_v2s32, AArch64_p0, AArch64_s64, 8>]>,
    WidenScalarToNextPow2<0>,
    ClampScalar<0, AArch64_s32, AArch64_s64>,
    UnsupportedIfMemSizeNotPow2,
    Lower
  ];
}

def AArch64_ZEXTLOAD : LegalizeRuleSet<["G_ZEXTLOAD"]> {
  let Rules = [
    LegalForMemDesc<[LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s8, 8>, LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s16, 8>, LegalizeMemDesc<AArch64_s32, AArch64_p0, AArch64_s32, 8>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s8, 2>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s16, 2>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s32, 4>, LegalizeMemDesc<AArch64_s64, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_p0, AArch64_p0, AArch64_s64, 8>, LegalizeMemDesc<AArch64_v2s32, AArch64_p0, AArch64_s64, 8>]>,
    WidenScalarToNextPow2<0>,
    ClampScalar<0, AArch64_s32, AArch64_s64>,
    UnsupportedIfMemSizeNotPow2,
    Lower
  ];
}

def AArch64_MERGE_VALUES : LegalizeRuleSet<["G_MERGE_VALUES"]> {
  let Rules = [
    WidenScalarToNextPow2<1, 8>,
    LowerIf<AArch64_Pred_e44b3fa8>,
    WidenScalarToNextPow2<0, 32>,
    ClampScalar<1, AArch64_s8, AArch64_s64>,
    ClampScalar<0, AArch64_s32, AArch64_s128>,
    LegalIf<AArch64_Pred_c20b2585>
  ];
}

def AArch64_UNMERGE_VALUES : LegalizeRuleSet<["G_UNMERGE_VALUES"]> {
  let Rules = [
    WidenScalarToNextPow2<0, 8>,
    LowerIf<AArch64_Pred_2e11e704>,
    WidenScalarToNextPow2<1, 32>,
    ClampScalar<0, AArch64_s8, AArch64_s64>,
    ClampScalar<1, AArch64_s32, AArch64_s128>,
    LegalIf<AArch64_Pred_6209b6d1>
  ];
}
