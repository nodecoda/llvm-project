//===- X86LegalizerInfo.td - generated legalizer rules -*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Generated from X86LegalizerInfo.gisel by llvm-dev/tools/gisel_td.py.
// Do not edit: edit the source and re-run the generator.

//===----------------------------------------------------------------------===//
include "llvm/Target/LegalizerInfo.td"
include "llvm/Target/Target.td"
//===----------------------------------------------------------------------===//

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
def HasVPOPCNTDQ : LegalizeFeature<"Subtarget.hasVPOPCNTDQ()">;
def HasLZCNT : LegalizeFeature<"Subtarget.hasLZCNT()">;
def HasBMI : LegalizeFeature<"Subtarget.hasBMI()">;
def UseX87NotSSE1 : LegalizePredicateDag<(all_of UseX87, (not HasSSE1))>;
def X86_p0 : LegalizeNamedType<"LLT::pointer(0, Subtarget.isTarget64BitLP64() ? 64 : 32)">;
def X86_s1 : LegalizeNamedType<"LLT::scalar(1)">;
def X86_s8 : LegalizeNamedType<"LLT::scalar(8)">;
def X86_s16 : LegalizeNamedType<"LLT::scalar(16)">;
def X86_s32 : LegalizeNamedType<"LLT::scalar(32)">;
def X86_s64 : LegalizeNamedType<"LLT::scalar(64)">;
def X86_s80 : LegalizeNamedType<"LLT::scalar(80)">;
def X86_s128 : LegalizeNamedType<"LLT::scalar(128)">;
def X86_sMaxScalar : LegalizeSelectType<[Is64Bit], [X86_s64], X86_s32>;
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
def X86_s8MaxVector : LegalizeSelectType<[HasAVX512, HasAVX], [X86_v64s8, X86_v32s8], X86_v16s8>;
def X86_s16MaxVector : LegalizeSelectType<[HasAVX512, HasAVX], [X86_v32s16, X86_v16s16], X86_v8s16>;
def X86_s32MaxVector : LegalizeSelectType<[HasAVX512, HasAVX], [X86_v16s32, X86_v8s32], X86_v4s32>;
def X86_s64MaxVector : LegalizeSelectType<[HasAVX512, HasAVX], [X86_v8s64, X86_v4s64], X86_v2s64>;
def X86_s64OrS32 : LegalizeSelectType<[HasSSE2], [X86_s64], X86_s32>;
def X86_X87OrS32 : LegalizeSelectType<[UseX87NotSSE1], [X86_s16], X86_s32>;
def X86_CMOVOrS8 : LegalizeSelectType<[HasCMOV], [X86_s16], X86_s8>;
def X86_Vec128 : LegalizeCoreTypeSet<[X86_v16s8, X86_v8s16, X86_v4s32, X86_v2s64]>;
def X86_Vec256 : LegalizeCoreTypeSet<[X86_v32s8, X86_v16s16, X86_v8s32, X86_v4s64]>;
def X86_Vec512 : LegalizeCoreTypeSet<[X86_v64s8, X86_v32s16, X86_v16s32, X86_v8s64]>;
def X86_IntTypes32 : LegalizeCoreTypeSet<[X86_s8, X86_s16, X86_s32, X86_p0]>;
def X86_IntTypes64 : LegalizeCoreTypeSet<[X86_s8, X86_s16, X86_s32, X86_s64, X86_p0]>;
def X86_PtrTypes32 : LegalizeCoreTypeSet<[X86_s1, X86_s8, X86_s16, X86_s32]>;
def X86_PtrTypes64 : LegalizeCoreTypeSet<[X86_s1, X86_s8, X86_s16, X86_s32, X86_s64]>;

def X86_IMPLICIT_DEF : LegalizeRuleSet<["G_IMPLICIT_DEF", "G_PHI", "G_FREEZE", "G_CONSTANT_FOLD_BARRIER"]> {
  let Comments = [
    "implicit/constants",
    "32/64-bits needs support for s64/s128 to handle cases:",
    "s64 = EXTEND (G_IMPLICIT_DEF s32) -> s64 = G_IMPLICIT_DEF",
    "s128 = EXTEND (G_IMPLICIT_DEF s32/s64) -> s128 = G_IMPLICIT_DEF",
  ];
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), [X86_p0, X86_s1, X86_s8, X86_s16, X86_s32, X86_s64])>,
    CoreLegal<(CoreAll UseX87, (CoreIn (Slot 0), X86_s80))>,
    CoreLegal<(CoreAll Is64Bit, (CoreIn (Slot 0), X86_s128))>,
    CoreLegal<(CoreAll HasSSE2, (CoreIn (Slot 0), X86_Vec128))>,
    CoreLegal<(CoreAll HasAVX, (CoreIn (Slot 0), X86_Vec256))>,
    CoreLegal<(CoreAll HasAVX512, (CoreIn (Slot 0), X86_Vec512))>,
    CoreWiden<(ElemOf (Slot 0)), (Pow2Min 8)>,
    CoreWiden<(ElemOf (Slot 0)), (Ty X86_s8)>,
    CoreNarrow<(ElemOf (Slot 0)), (Ty X86_sMaxScalar)>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreMoreIf<(Slot 0), (Replace (VecOf (NumElems (Ty X86_v16s8)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s8)), (CoreLt (NumElems (Slot 0)), (NumElems (Ty X86_v16s8))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (NumElems (Ty X86_s8MaxVector)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s8)), (CoreGt (NumElems (Slot 0)), (NumElems (Ty X86_s8MaxVector))))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf (NumElems (Ty X86_v8s16)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s16)), (CoreLt (NumElems (Slot 0)), (NumElems (Ty X86_v8s16))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (NumElems (Ty X86_s16MaxVector)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s16)), (CoreGt (NumElems (Slot 0)), (NumElems (Ty X86_s16MaxVector))))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf (NumElems (Ty X86_v4s32)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s32)), (CoreLt (NumElems (Slot 0)), (NumElems (Ty X86_v4s32))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (NumElems (Ty X86_s32MaxVector)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s32)), (CoreGt (NumElems (Slot 0)), (NumElems (Ty X86_s32MaxVector))))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf (NumElems (Ty X86_v2s64)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s64)), (CoreLt (NumElems (Slot 0)), (NumElems (Ty X86_v2s64))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (NumElems (Ty X86_s64MaxVector)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s64)), (CoreGt (NumElems (Slot 0)), (NumElems (Ty X86_s64MaxVector))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (CoreSelect (CoreAll Is64Bit, HasAVX512), 8, (CoreSelect (CoreAll Is64Bit, HasAVX), 4, (CoreSelect Is64Bit, 2, (CoreSelect HasAVX512, 16, (CoreSelect HasAVX, 8, 4))))), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_p0)), (CoreGt (NumElems (Slot 0)), (CoreSelect (CoreAll Is64Bit, HasAVX512), 8, (CoreSelect (CoreAll Is64Bit, HasAVX), 4, (CoreSelect Is64Bit, 2, (CoreSelect HasAVX512, 16, (CoreSelect HasAVX, 8, 4)))))))>,
    CoreScalarizeIf<(Slot 0), (CoreGt (BitsOf (Slot 0)), 64)>,
  ];
}

def X86_CONSTANT : LegalizeRuleSet<["G_CONSTANT"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), [X86_p0, X86_s8, X86_s16, X86_s32])>,
    CoreLegal<(CoreAll Is64Bit, (CoreIn (Slot 0), X86_s64))>,
    CoreWiden<(Slot 0), (Pow2Min 8)>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
  ];
}

def X86_LROUND : LegalizeRuleSet<["G_LROUND", "G_LLROUND"]> {
  let Rules = [
    CoreWidenIf<(Slot 1), (Ty X86_s32), (CoreEq (Slot 1), (Ty X86_s16))>,
    CoreAlwaysLibcall,
  ];
}

def X86_FCOS : LegalizeRuleSet<["G_FCOS", "G_FCOSH", "G_FACOS", "G_FSIN", "G_FSINH", "G_FASIN", "G_FTAN", "G_FTANH", "G_FATAN", "G_FATAN2", "G_FPOW", "G_FEXP", "G_FEXP2", "G_FEXP10", "G_FLOG", "G_FLOG2", "G_FLOG10", "G_FPOWI", "G_FSINCOS", "G_FCEIL", "G_FFLOOR"]> {
  let Rules = [
    CoreAlwaysLibcall,
  ];
}

def X86_FNEG : LegalizeRuleSet<["G_FNEG"]> {
  let Rules = [
    CoreLegal<(CoreAll UseX87, (CoreNot HasSSE1), (CoreIn (Slot 0), X86_s32))>,
    CoreLegal<(CoreAll UseX87, (CoreNot HasSSE2), (CoreIn (Slot 0), X86_s64))>,
    CoreLegal<(CoreAll UseX87, (CoreIn (Slot 0), X86_s80))>,
    CoreAlwaysLower,
  ];
}

def X86_FSQRT : LegalizeRuleSet<["G_FSQRT"]> {
  let Rules = [
    CoreLegal<(CoreAll (CoreAny HasSSE1, UseX87), (CoreIn (Slot 0), X86_s32))>,
    CoreLegal<(CoreAll (CoreAny HasSSE2, UseX87), (CoreIn (Slot 0), X86_s64))>,
    CoreLegal<(CoreAll UseX87, (CoreIn (Slot 0), X86_s80))>,
  ];
}

def X86_GET_ROUNDING : LegalizeRuleSet<["G_GET_ROUNDING", "G_SET_ROUNDING"]> {
  let Rules = [
    CoreCustom<(CoreIn (Slot 0), X86_s32)>,
  ];
}

def X86_MERGE_VALUES : LegalizeRuleSet<["G_MERGE_VALUES"]> {
  let Rules = [
    CoreWiden<(Slot 1), (Pow2Min 8)>,
    CoreWiden<(Slot 0), (Pow2Min 16)>,
    CoreWiden<(Slot 1), (Ty X86_s8)>,
    CoreWiden<(Slot 0), (Ty X86_s32)>,
    CoreLegal<(CoreAll (CoreAny (CoreEq (SizeOf (Slot 0)), 16), (CoreEq (SizeOf (Slot 0)), 32), (CoreEq (SizeOf (Slot 0)), 64), (CoreEq (SizeOf (Slot 0)), 128), (CoreEq (SizeOf (Slot 0)), 256), (CoreEq (SizeOf (Slot 0)), 512)), (CoreAny (CoreEq (SizeOf (Slot 1)), 8), (CoreEq (SizeOf (Slot 1)), 16), (CoreEq (SizeOf (Slot 1)), 32), (CoreEq (SizeOf (Slot 1)), 64), (CoreEq (SizeOf (Slot 1)), 128), (CoreEq (SizeOf (Slot 1)), 256)))>,
  ];
}

def X86_UNMERGE_VALUES : LegalizeRuleSet<["G_UNMERGE_VALUES"]> {
  let Rules = [
    CoreWiden<(Slot 0), (Pow2Min 8)>,
    CoreWiden<(Slot 1), (Pow2Min 16)>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreWiden<(Slot 1), (Ty X86_s32)>,
    CoreLegal<(CoreAll (CoreAny (CoreEq (SizeOf (Slot 1)), 16), (CoreEq (SizeOf (Slot 1)), 32), (CoreEq (SizeOf (Slot 1)), 64), (CoreEq (SizeOf (Slot 1)), 128), (CoreEq (SizeOf (Slot 1)), 256), (CoreEq (SizeOf (Slot 1)), 512)), (CoreAny (CoreEq (SizeOf (Slot 0)), 8), (CoreEq (SizeOf (Slot 0)), 16), (CoreEq (SizeOf (Slot 0)), 32), (CoreEq (SizeOf (Slot 0)), 64), (CoreEq (SizeOf (Slot 0)), 128), (CoreEq (SizeOf (Slot 0)), 256)))>,
  ];
}

def X86_UMIN : LegalizeRuleSet<["G_UMIN", "G_UMAX", "G_SMIN", "G_SMAX"]> {
  let Rules = [
    CoreWiden<(Slot 0), (Pow2Min 32)>,
    CoreAlwaysLower,
  ];
}

def X86_ADD : LegalizeRuleSet<["G_ADD", "G_SUB"]> {
  let Comments = [
    "integer addition/subtraction",
  ];
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), [X86_s8, X86_s16, X86_s32])>,
    CoreLegal<(CoreAll Is64Bit, (CoreIn (Slot 0), X86_s64))>,
    CoreLegal<(CoreAll HasSSE2, (CoreIn (Slot 0), X86_Vec128))>,
    CoreLegal<(CoreAll HasAVX2, (CoreIn (Slot 0), X86_Vec256))>,
    CoreLegal<(CoreAll HasAVX512, (CoreIn (Slot 0), [X86_v16s32, X86_v8s64]))>,
    CoreLegal<(CoreAll HasBWI, (CoreIn (Slot 0), [X86_v64s8, X86_v32s16]))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf 16, (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s8)), (CoreLt (NumElems (Slot 0)), 16))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf 8, (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s16)), (CoreLt (NumElems (Slot 0)), 8))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf 4, (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s32)), (CoreLt (NumElems (Slot 0)), 4))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf 2, (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s64)), (CoreLt (NumElems (Slot 0)), 2))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (CoreSelect HasBWI, 64, (CoreSelect HasAVX2, 32, 16)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s8)), (CoreGt (NumElems (Slot 0)), (CoreSelect HasBWI, 64, (CoreSelect HasAVX2, 32, 16))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (CoreSelect HasBWI, 32, (CoreSelect HasAVX2, 16, 8)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s16)), (CoreGt (NumElems (Slot 0)), (CoreSelect HasBWI, 32, (CoreSelect HasAVX2, 16, 8))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (CoreSelect HasAVX512, 16, (CoreSelect HasAVX2, 8, 4)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s32)), (CoreGt (NumElems (Slot 0)), (CoreSelect HasAVX512, 16, (CoreSelect HasAVX2, 8, 4))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (CoreSelect HasAVX512, 8, (CoreSelect HasAVX2, 4, 2)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s64)), (CoreGt (NumElems (Slot 0)), (CoreSelect HasAVX512, 8, (CoreSelect HasAVX2, 4, 2))))>,
    CoreWiden<(Slot 0), (Pow2Min 32)>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
    CoreScalarize<(Slot 0)>,
  ];
}

def X86_UADDE : LegalizeRuleSet<["G_UADDE", "G_UADDO", "G_USUBE", "G_USUBO"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), [[X86_s8, X86_s8], [X86_s16, X86_s8], [X86_s32, X86_s8]])>,
    CoreLegal<(CoreAll Is64Bit, (CoreInPairs (Slot 0), (Slot 1), [[X86_s64, X86_s8]]))>,
    CoreWiden<(Slot 0), (Pow2Min 32)>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
    CoreWiden<(Slot 1), (Ty X86_s8)>,
    CoreNarrow<(Slot 1), (Ty X86_s8)>,
    CoreScalarize<(Slot 0)>,
  ];
}

def X86_MUL : LegalizeRuleSet<["G_MUL"]> {
  let Comments = [
    "integer multiply",
  ];
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), [X86_s8, X86_s16, X86_s32])>,
    CoreLegal<(CoreAll Is64Bit, (CoreIn (Slot 0), X86_s64))>,
    CoreLegal<(CoreAll HasSSE2, (CoreIn (Slot 0), X86_v8s16))>,
    CoreLegal<(CoreAll HasSSE41, (CoreIn (Slot 0), X86_v4s32))>,
    CoreLegal<(CoreAll HasAVX2, (CoreIn (Slot 0), [X86_v16s16, X86_v8s32]))>,
    CoreLegal<(CoreAll HasAVX512, (CoreIn (Slot 0), X86_v16s32))>,
    CoreLegal<(CoreAll HasDQI, (CoreIn (Slot 0), X86_v8s64))>,
    CoreLegal<(CoreAll HasDQI, HasVLX, (CoreIn (Slot 0), [X86_v2s64, X86_v4s64]))>,
    CoreLegal<(CoreAll HasBWI, (CoreIn (Slot 0), X86_v32s16))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf 8, (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s16)), (CoreLt (NumElems (Slot 0)), 8))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf 4, (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s32)), (CoreLt (NumElems (Slot 0)), 4))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf (CoreSelect HasVLX, 2, 8), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s64)), (CoreLt (NumElems (Slot 0)), (CoreSelect HasVLX, 2, 8)))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (CoreSelect HasBWI, 32, (CoreSelect HasAVX2, 16, 8)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s16)), (CoreGt (NumElems (Slot 0)), (CoreSelect HasBWI, 32, (CoreSelect HasAVX2, 16, 8))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (CoreSelect HasAVX512, 16, (CoreSelect HasAVX2, 8, 4)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s32)), (CoreGt (NumElems (Slot 0)), (CoreSelect HasAVX512, 16, (CoreSelect HasAVX2, 8, 4))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf 8, (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s64)), (CoreGt (NumElems (Slot 0)), 8))>,
    CoreWiden<(Slot 0), (Pow2Min 32)>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
    CoreScalarize<(Slot 0)>,
  ];
}

def X86_SMULH : LegalizeRuleSet<["G_SMULH", "G_UMULH"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), [X86_s8, X86_s16, X86_s32])>,
    CoreLegal<(CoreAll Is64Bit, (CoreIn (Slot 0), X86_s64))>,
    CoreWiden<(Slot 0), (Pow2Min 32)>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
    CoreScalarize<(Slot 0)>,
  ];
}

def X86_SDIV : LegalizeRuleSet<["G_SDIV", "G_SREM", "G_UDIV", "G_UREM"]> {
  let Comments = [
    "integer divisions",
  ];
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), [X86_s8, X86_s16, X86_s32])>,
    CoreLegal<(CoreAll Is64Bit, (CoreIn (Slot 0), X86_s64))>,
    CoreLibcall<(CoreIn (Slot 0), X86_s64)>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
  ];
}

def X86_SHL : LegalizeRuleSet<["G_SHL", "G_LSHR", "G_ASHR"]> {
  let Comments = [
    "integer shifts",
  ];
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), [[X86_s8, X86_s8], [X86_s16, X86_s8], [X86_s32, X86_s8]])>,
    CoreLegal<(CoreAll Is64Bit, (CoreInPairs (Slot 0), (Slot 1), [[X86_s64, X86_s8]]))>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
    CoreWiden<(Slot 1), (Ty X86_s8)>,
    CoreNarrow<(Slot 1), (Ty X86_s8)>,
  ];
}

def X86_AND : LegalizeRuleSet<["G_AND", "G_OR", "G_XOR"]> {
  let Comments = [
    "integer logic",
  ];
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), [X86_s8, X86_s16, X86_s32])>,
    CoreLegal<(CoreAll Is64Bit, (CoreIn (Slot 0), X86_s64))>,
    CoreLegal<(CoreAll HasSSE2, (CoreIn (Slot 0), X86_Vec128))>,
    CoreLegal<(CoreAll HasAVX, (CoreIn (Slot 0), X86_Vec256))>,
    CoreLegal<(CoreAll HasAVX512, (CoreIn (Slot 0), X86_Vec512))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf (NumElems (Ty X86_v16s8)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s8)), (CoreLt (NumElems (Slot 0)), (NumElems (Ty X86_v16s8))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (NumElems (Ty X86_s8MaxVector)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s8)), (CoreGt (NumElems (Slot 0)), (NumElems (Ty X86_s8MaxVector))))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf (NumElems (Ty X86_v8s16)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s16)), (CoreLt (NumElems (Slot 0)), (NumElems (Ty X86_v8s16))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (NumElems (Ty X86_s16MaxVector)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s16)), (CoreGt (NumElems (Slot 0)), (NumElems (Ty X86_s16MaxVector))))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf (NumElems (Ty X86_v4s32)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s32)), (CoreLt (NumElems (Slot 0)), (NumElems (Ty X86_v4s32))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (NumElems (Ty X86_s32MaxVector)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s32)), (CoreGt (NumElems (Slot 0)), (NumElems (Ty X86_s32MaxVector))))>,
    CoreMoreIf<(Slot 0), (Replace (VecOf (NumElems (Ty X86_v2s64)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s64)), (CoreLt (NumElems (Slot 0)), (NumElems (Ty X86_v2s64))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (NumElems (Ty X86_s64MaxVector)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s64)), (CoreGt (NumElems (Slot 0)), (NumElems (Ty X86_s64MaxVector))))>,
    CoreWiden<(Slot 0), (Pow2Min 32)>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
    CoreScalarize<(Slot 0)>,
  ];
}

def X86_ICMP : LegalizeRuleSet<["G_ICMP"]> {
  let Rules = [
    CoreLegal<(CoreAll Is64Bit, (CoreIn (Slot 0), X86_s8), (CoreIn (Slot 1), X86_IntTypes64))>,
    CoreLegal<(CoreAll (CoreNot Is64Bit), (CoreIn (Slot 0), X86_s8), (CoreIn (Slot 1), X86_IntTypes32))>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_s8)>,
    CoreWiden<(Slot 1), (Pow2Min 8)>,
    CoreWiden<(Slot 1), (Ty X86_s8)>,
    CoreNarrow<(Slot 1), (Ty X86_sMaxScalar)>,
  ];
}

def X86_BSWAP : LegalizeRuleSet<["G_BSWAP"]> {
  let Comments = [
    "bswap",
  ];
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), X86_s32)>,
    CoreLegal<(CoreAll Is64Bit, (CoreIn (Slot 0), X86_s64))>,
    CoreWiden<(Slot 0), (Pow2Min 32)>,
    CoreWiden<(Slot 0), (Ty X86_s32)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
  ];
}

def X86_CTPOP : LegalizeRuleSet<["G_CTPOP"]> {
  let Comments = [
    "popcount",
  ];
  let Rules = [
    CoreLegal<(CoreAll HasPOPCNT, (CoreInPairs (Slot 0), (Slot 1), [[X86_s16, X86_s16], [X86_s32, X86_s32]]))>,
    CoreLegal<(CoreAll HasPOPCNT, Is64Bit, (CoreInPairs (Slot 0), (Slot 1), [[X86_s64, X86_s64]]))>,
    CoreLegal<(CoreAll HasVPOPCNTDQ, (CoreInPairs (Slot 0), (Slot 1), [[X86_v16s32, X86_v16s32], [X86_v8s64, X86_v8s64]]))>,
    CoreLegal<(CoreAll HasVPOPCNTDQ, HasVLX, (CoreInPairs (Slot 0), (Slot 1), [[X86_v4s32, X86_v4s32], [X86_v8s32, X86_v8s32], [X86_v2s64, X86_v2s64], [X86_v4s64, X86_v4s64]]))>,
    CoreWiden<(Slot 1), (Pow2Min 16)>,
    CoreWiden<(Slot 1), (Ty X86_s16)>,
    CoreNarrow<(Slot 1), (Ty X86_sMaxScalar)>,
    CoreWiden<(Slot 0), (SameAs 1)>,
    CoreNarrow<(Slot 0), (SameAs 1)>,
  ];
}

def X86_CTLZ : LegalizeRuleSet<["G_CTLZ"]> {
  let Comments = [
    "count leading zeros (LZCNT)",
  ];
  let Rules = [
    CoreLegal<(CoreAll HasLZCNT, (CoreInPairs (Slot 0), (Slot 1), [[X86_s16, X86_s16], [X86_s32, X86_s32]]))>,
    CoreLegal<(CoreAll HasLZCNT, Is64Bit, (CoreInPairs (Slot 0), (Slot 1), [[X86_s64, X86_s64]]))>,
    CoreWiden<(Slot 1), (Pow2Min 16)>,
    CoreWiden<(Slot 1), (Ty X86_s16)>,
    CoreNarrow<(Slot 1), (Ty X86_sMaxScalar)>,
    CoreWiden<(Slot 0), (SameAs 1)>,
    CoreNarrow<(Slot 0), (SameAs 1)>,
  ];
}

def X86_CTTZ_ZERO_POISON : LegalizeRuleSet<["G_CTTZ_ZERO_POISON"]> {
  let Comments = [
    "count trailing zeros",
  ];
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), [[X86_s16, X86_s16], [X86_s32, X86_s32]])>,
    CoreLegal<(CoreAll Is64Bit, (CoreInPairs (Slot 0), (Slot 1), [[X86_s64, X86_s64]]))>,
    CoreWiden<(Slot 1), (Pow2Min 16)>,
    CoreWiden<(Slot 1), (Ty X86_s16)>,
    CoreNarrow<(Slot 1), (Ty X86_sMaxScalar)>,
    CoreWiden<(Slot 0), (SameAs 1)>,
    CoreNarrow<(Slot 0), (SameAs 1)>,
  ];
}

def X86_CTTZ : LegalizeRuleSet<["G_CTTZ"]> {
  let Rules = [
    CoreLegal<(CoreAll HasBMI, (CoreInPairs (Slot 0), (Slot 1), [[X86_s16, X86_s16], [X86_s32, X86_s32]]))>,
    CoreLegal<(CoreAll HasBMI, Is64Bit, (CoreInPairs (Slot 0), (Slot 1), [[X86_s64, X86_s64]]))>,
    CoreWiden<(Slot 1), (Pow2Min 16)>,
    CoreWiden<(Slot 1), (Ty X86_s16)>,
    CoreNarrow<(Slot 1), (Ty X86_sMaxScalar)>,
    CoreWiden<(Slot 0), (SameAs 1)>,
    CoreNarrow<(Slot 0), (SameAs 1)>,
  ];
}

def X86_BR : LegalizeRuleSet<["G_BR"]> {
  let Rules = [
    CoreAlwaysLegal,
  ];
}

def X86_BRCOND : LegalizeRuleSet<["G_BRCOND"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), X86_s1)>,
  ];
}

def X86_PTRTOINT : LegalizeRuleSet<["G_PTRTOINT"]> {
  let Rules = [
    CoreLegal<(CoreAll Is64Bit, (CoreIn (Slot 0), X86_PtrTypes64), (CoreIn (Slot 1), X86_p0))>,
    CoreLegal<(CoreAll (CoreNot Is64Bit), (CoreIn (Slot 0), X86_PtrTypes32), (CoreIn (Slot 1), X86_p0))>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
    CoreWiden<(Slot 0), (Pow2Min 8)>,
  ];
}

def X86_INTTOPTR : LegalizeRuleSet<["G_INTTOPTR"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), [[X86_p0, X86_sMaxScalar]])>,
  ];
}

def X86_CONSTANT_POOL : LegalizeRuleSet<["G_CONSTANT_POOL"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), X86_p0)>,
  ];
}

def X86_PTR_ADD : LegalizeRuleSet<["G_PTR_ADD"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), [[X86_p0, X86_s32]])>,
    CoreLegal<(CoreAll Is64Bit, (CoreInPairs (Slot 0), (Slot 1), [[X86_p0, X86_s64]]))>,
    CoreWiden<(Slot 1), (Pow2Min 32)>,
    CoreWiden<(Slot 1), (Ty X86_s32)>,
    CoreNarrow<(Slot 1), (Ty X86_sMaxScalar)>,
  ];
}

def X86_FRAME_INDEX : LegalizeRuleSet<["G_FRAME_INDEX"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), X86_p0)>,
  ];
}

def X86_GLOBAL_VALUE : LegalizeRuleSet<["G_GLOBAL_VALUE"]> {
  let Rules = [
    CoreCustom<(CoreIn (Slot 0), X86_p0)>,
  ];
}

def X86_LOAD : LegalizeRuleSet<["G_LOAD"]> {
  let Comments = [
    "load/store: add more corner cases",
  ];
  let Rules = [
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_s8, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s16, X86_p0, X86_s16, 1>, LegalizeMemDesc<X86_s32, X86_p0, X86_s32, 1>, LegalizeMemDesc<X86_s80, X86_p0, X86_s80, 1>, LegalizeMemDesc<X86_p0, X86_p0, X86_p0, 1>, LegalizeMemDesc<X86_v4s8, X86_p0, X86_v4s8, 1>])>,
    CoreLegal<(CoreAll Is64Bit, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_s64, X86_p0, X86_s64, 1>, LegalizeMemDesc<X86_v2s32, X86_p0, X86_v2s32, 1>]))>,
    CoreLegal<(CoreAll HasSSE1, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_v4s32, X86_p0, X86_v4s32, 1>]))>,
    CoreLegal<(CoreAll HasSSE2, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_v16s8, X86_p0, X86_v16s8, 1>, LegalizeMemDesc<X86_v8s16, X86_p0, X86_v8s16, 1>, LegalizeMemDesc<X86_v2s64, X86_p0, X86_v2s64, 1>, LegalizeMemDesc<X86_v2p0, X86_p0, X86_v2p0, 1>]))>,
    CoreLegal<(CoreAll HasAVX, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_v32s8, X86_p0, X86_v32s8, 1>, LegalizeMemDesc<X86_v16s16, X86_p0, X86_v16s16, 1>, LegalizeMemDesc<X86_v8s32, X86_p0, X86_v8s32, 1>, LegalizeMemDesc<X86_v4s64, X86_p0, X86_v4s64, 1>, LegalizeMemDesc<X86_v4p0, X86_p0, X86_v4p0, 1>]))>,
    CoreLegal<(CoreAll HasAVX512, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_v64s8, X86_p0, X86_v64s8, 1>, LegalizeMemDesc<X86_v32s16, X86_p0, X86_v32s16, 1>, LegalizeMemDesc<X86_v16s32, X86_p0, X86_v16s32, 1>, LegalizeMemDesc<X86_v8s64, X86_p0, X86_v8s64, 1>]))>,
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_s8, X86_p0, X86_s1, 1>, LegalizeMemDesc<X86_s16, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s32, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s32, X86_p0, X86_s16, 1>])>,
    CoreLegal<(CoreAll Is64Bit, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_s64, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s64, X86_p0, X86_s16, 1>, LegalizeMemDesc<X86_s64, X86_p0, X86_s32, 1>]))>,
    CoreWiden<(Slot 0), (Pow2Min 8)>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
    CoreScalarize<(Slot 0)>,
  ];
}

def X86_STORE : LegalizeRuleSet<["G_STORE"]> {
  let Rules = [
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_s8, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s16, X86_p0, X86_s16, 1>, LegalizeMemDesc<X86_s32, X86_p0, X86_s32, 1>, LegalizeMemDesc<X86_s80, X86_p0, X86_s80, 1>, LegalizeMemDesc<X86_p0, X86_p0, X86_p0, 1>, LegalizeMemDesc<X86_v4s8, X86_p0, X86_v4s8, 1>])>,
    CoreLegal<(CoreAll Is64Bit, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_s64, X86_p0, X86_s64, 1>, LegalizeMemDesc<X86_v2s32, X86_p0, X86_v2s32, 1>]))>,
    CoreLegal<(CoreAll HasSSE1, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_v4s32, X86_p0, X86_v4s32, 1>]))>,
    CoreLegal<(CoreAll HasSSE2, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_v16s8, X86_p0, X86_v16s8, 1>, LegalizeMemDesc<X86_v8s16, X86_p0, X86_v8s16, 1>, LegalizeMemDesc<X86_v2s64, X86_p0, X86_v2s64, 1>, LegalizeMemDesc<X86_v2p0, X86_p0, X86_v2p0, 1>]))>,
    CoreLegal<(CoreAll HasAVX, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_v32s8, X86_p0, X86_v32s8, 1>, LegalizeMemDesc<X86_v16s16, X86_p0, X86_v16s16, 1>, LegalizeMemDesc<X86_v8s32, X86_p0, X86_v8s32, 1>, LegalizeMemDesc<X86_v4s64, X86_p0, X86_v4s64, 1>, LegalizeMemDesc<X86_v4p0, X86_p0, X86_v4p0, 1>]))>,
    CoreLegal<(CoreAll HasAVX512, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_v64s8, X86_p0, X86_v64s8, 1>, LegalizeMemDesc<X86_v32s16, X86_p0, X86_v32s16, 1>, LegalizeMemDesc<X86_v16s32, X86_p0, X86_v16s32, 1>, LegalizeMemDesc<X86_v8s64, X86_p0, X86_v8s64, 1>]))>,
    CoreCustom<(CoreNe (Slot 0), (MemTy (MemSlot 0)))>,
    CoreWiden<(Slot 0), (Pow2Min 8)>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
    CoreScalarize<(Slot 0)>,
  ];
}

def X86_SEXTLOAD : LegalizeRuleSet<["G_SEXTLOAD"]> {
  let Rules = [
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_s16, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s32, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s32, X86_p0, X86_s16, 1>])>,
    CoreLegal<(CoreAll Is64Bit, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_s64, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s64, X86_p0, X86_s16, 1>, LegalizeMemDesc<X86_s64, X86_p0, X86_s32, 1>]))>,
  ];
}

def X86_ZEXTLOAD : LegalizeRuleSet<["G_ZEXTLOAD"]> {
  let Rules = [
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_s16, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s32, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s32, X86_p0, X86_s16, 1>])>,
    CoreLegal<(CoreAll Is64Bit, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_s64, X86_p0, X86_s8, 1>, LegalizeMemDesc<X86_s64, X86_p0, X86_s16, 1>, LegalizeMemDesc<X86_s64, X86_p0, X86_s32, 1>]))>,
  ];
}

def X86_FPEXTLOAD : LegalizeRuleSet<["G_FPEXTLOAD"]> {
  let Rules = [
    CoreLegal<(CoreAll UseX87, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_s80, X86_p0, X86_s32, 1>, LegalizeMemDesc<X86_s80, X86_p0, X86_s64, 1>, LegalizeMemDesc<X86_s64, X86_p0, X86_s32, 1>]))>,
  ];
}

def X86_FPTRUNCSTORE : LegalizeRuleSet<["G_FPTRUNCSTORE"]> {
  let Rules = [
    CoreLegal<(CoreAll UseX87, (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), [LegalizeMemDesc<X86_s80, X86_p0, X86_s32, 1>, LegalizeMemDesc<X86_s80, X86_p0, X86_s64, 1>, LegalizeMemDesc<X86_s64, X86_p0, X86_s32, 1>]))>,
  ];
}

def X86_ANYEXT : LegalizeRuleSet<["G_ANYEXT"]> {
  let Comments = [
    "sext, zext, and anyext",
  ];
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), [X86_s8, X86_s16, X86_s32, X86_s128])>,
    CoreLegal<(CoreAll Is64Bit, (CoreIn (Slot 0), X86_s64))>,
    CoreWiden<(Slot 0), (Pow2Min 8)>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
    CoreWiden<(Slot 1), (Pow2Min 8)>,
    CoreWiden<(Slot 1), (Ty X86_s8)>,
    CoreNarrow<(Slot 1), (Ty X86_sMaxScalar)>,
    CoreScalarize<(Slot 0)>,
  ];
}

def X86_SEXT : LegalizeRuleSet<["G_SEXT", "G_ZEXT"]> {
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), [X86_s8, X86_s16, X86_s32])>,
    CoreLegal<(CoreAll Is64Bit, (CoreIn (Slot 0), X86_s64))>,
    CoreWiden<(Slot 0), (Pow2Min 8)>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
    CoreWiden<(Slot 1), (Pow2Min 8)>,
    CoreWiden<(Slot 1), (Ty X86_s8)>,
    CoreNarrow<(Slot 1), (Ty X86_sMaxScalar)>,
    CoreScalarize<(Slot 0)>,
  ];
}

def X86_TRUNC : LegalizeRuleSet<["G_TRUNC"]> {
  let Rules = [
    CoreLegal<(CoreAll (CoreIn (Slot 0), X86_PtrTypes64), (CoreIn (Slot 1), [X86_s8, X86_s16, X86_s32, X86_s64, X86_s128]))>,
  ];
}

def X86_SEXT_INREG : LegalizeRuleSet<["G_SEXT_INREG"]> {
  let Rules = [
    CoreAlwaysLower,
  ];
}

def X86_FCONSTANT : LegalizeRuleSet<["G_FCONSTANT"]> {
  let Comments = [
    "fp constants",
  ];
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), [X86_s32, X86_s64])>,
    CoreLegal<(CoreAll UseX87, (CoreIn (Slot 0), X86_s80))>,
  ];
}

def X86_FADD : LegalizeRuleSet<["G_FADD", "G_FSUB", "G_FMUL", "G_FDIV"]> {
  let Comments = [
    "fp arithmetic",
  ];
  let Rules = [
    CoreLegal<(CoreIn (Slot 0), [X86_s32, X86_s64])>,
    CoreLegal<(CoreAll HasSSE1, (CoreIn (Slot 0), X86_v4s32))>,
    CoreLegal<(CoreAll HasSSE2, (CoreIn (Slot 0), X86_v2s64))>,
    CoreLegal<(CoreAll HasAVX, (CoreIn (Slot 0), [X86_v8s32, X86_v4s64]))>,
    CoreLegal<(CoreAll HasAVX512, (CoreIn (Slot 0), [X86_v16s32, X86_v8s64]))>,
    CoreLegal<(CoreAll UseX87, (CoreIn (Slot 0), X86_s80))>,
  ];
}

def X86_FABS : LegalizeRuleSet<["G_FABS"]> {
  let Rules = [
    CoreLegal<(CoreAll UseX87, (CoreIn (Slot 0), X86_s80))>,
    CoreLegal<(CoreAll UseX87, (CoreNot Is64Bit), (CoreIn (Slot 0), X86_s64))>,
    CoreAlwaysLower,
  ];
}

def X86_FCMP : LegalizeRuleSet<["G_FCMP"]> {
  let Comments = [
    "fp comparison",
  ];
  let Rules = [
    CoreLegal<(CoreAll (CoreAny HasSSE1, UseX87), (CoreIn (Slot 0), [X86_s8, X86_s32]))>,
    CoreLegal<(CoreAll (CoreAny HasSSE2, UseX87), (CoreIn (Slot 0), [X86_s8, X86_s64]))>,
    CoreLegal<(CoreAll UseX87, (CoreIn (Slot 0), [X86_s8, X86_s80]))>,
    CoreWiden<(Slot 0), (Ty X86_s8)>,
    CoreNarrow<(Slot 0), (Ty X86_s8)>,
    CoreWiden<(Slot 1), (Ty X86_s32)>,
    CoreNarrow<(Slot 1), (Ty X86_s64OrS32)>,
    CoreWiden<(Slot 1), (Pow2)>,
  ];
}

def X86_FPEXT : LegalizeRuleSet<["G_FPEXT"]> {
  let Comments = [
    "fp conversions",
  ];
  let Rules = [
    CoreLegal<(CoreAll HasSSE2, (CoreInPairs (Slot 0), (Slot 1), [[X86_s64, X86_s32]]))>,
    CoreLegal<(CoreAll HasAVX, (CoreInPairs (Slot 0), (Slot 1), [[X86_v4s64, X86_v4s32]]))>,
    CoreLegal<(CoreAll HasAVX512, (CoreInPairs (Slot 0), (Slot 1), [[X86_v8s64, X86_v8s32]]))>,
    CoreLower<(CoreAll UseX87, (CoreInPairs (Slot 0), (Slot 1), [[X86_s64, X86_s32], [X86_s80, X86_s32], [X86_s80, X86_s64]]))>,
    CoreAlwaysLibcall,
  ];
}

def X86_FPTRUNC : LegalizeRuleSet<["G_FPTRUNC"]> {
  let Rules = [
    CoreLegal<(CoreAll HasSSE2, (CoreInPairs (Slot 0), (Slot 1), [[X86_s32, X86_s64]]))>,
    CoreLegal<(CoreAll HasAVX, (CoreInPairs (Slot 0), (Slot 1), [[X86_v4s32, X86_v4s64]]))>,
    CoreLegal<(CoreAll HasAVX512, (CoreInPairs (Slot 0), (Slot 1), [[X86_v8s32, X86_v8s64]]))>,
    CoreLower<(CoreAll UseX87, (CoreInPairs (Slot 0), (Slot 1), [[X86_s32, X86_s64], [X86_s32, X86_s80], [X86_s64, X86_s80]]))>,
  ];
}

def X86_SITOFP : LegalizeRuleSet<["G_SITOFP"]> {
  let Rules = [
    CoreLegal<(CoreAll HasSSE1, (CoreInPairs (Slot 0), (Slot 1), [[X86_s32, X86_s32]]))>,
    CoreLegal<(CoreAll HasSSE1, Is64Bit, (CoreInPairs (Slot 0), (Slot 1), [[X86_s32, X86_s64]]))>,
    CoreLegal<(CoreAll HasSSE2, (CoreInPairs (Slot 0), (Slot 1), [[X86_s64, X86_s32]]))>,
    CoreLegal<(CoreAll HasSSE2, Is64Bit, (CoreInPairs (Slot 0), (Slot 1), [[X86_s64, X86_s64]]))>,
    CoreWiden<(Slot 1), (Ty X86_X87OrS32)>,
    CoreNarrow<(Slot 1), (Ty X86_sMaxScalar)>,
    CoreWiden<(Slot 1), (Pow2)>,
    CoreCustom<(CoreAll UseX87, (CoreAll (CoreIn (Slot 0), [X86_s32, X86_s64, X86_s80]), (CoreIn (Slot 1), [X86_s16, X86_s32, X86_s64])))>,
    CoreWiden<(Slot 0), (Ty X86_s32)>,
    CoreNarrow<(Slot 0), (Ty X86_s64OrS32)>,
    CoreWiden<(Slot 0), (Pow2)>,
  ];
}

def X86_FPTOSI : LegalizeRuleSet<["G_FPTOSI"]> {
  let Rules = [
    CoreLegal<(CoreAll HasSSE1, (CoreInPairs (Slot 0), (Slot 1), [[X86_s32, X86_s32]]))>,
    CoreLegal<(CoreAll HasSSE1, Is64Bit, (CoreInPairs (Slot 0), (Slot 1), [[X86_s64, X86_s32]]))>,
    CoreLegal<(CoreAll HasSSE2, (CoreInPairs (Slot 0), (Slot 1), [[X86_s32, X86_s64]]))>,
    CoreLegal<(CoreAll HasSSE2, Is64Bit, (CoreInPairs (Slot 0), (Slot 1), [[X86_s64, X86_s64]]))>,
    CoreWiden<(Slot 0), (Ty X86_X87OrS32)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreCustom<(CoreAll UseX87, (CoreAll (CoreIn (Slot 0), [X86_s16, X86_s32, X86_s64]), (CoreIn (Slot 1), [X86_s32, X86_s64, X86_s80])))>,
    CoreWiden<(Slot 1), (Ty X86_s32)>,
    CoreNarrow<(Slot 1), (Ty X86_s64OrS32)>,
    CoreWiden<(Slot 1), (Pow2)>,
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
    CoreLegal<(CoreAll HasAVX512, (CoreInPairs (Slot 0), (Slot 1), [[X86_s32, X86_s32], [X86_s32, X86_s64], [X86_s64, X86_s32], [X86_s64, X86_s64]]))>,
    CoreCustom<(CoreAll (CoreNot HasAVX512), (CoreAny (CoreAll HasSSE1, (CoreEq (Slot 0), (Ty X86_s32))), (CoreAll HasSSE2, (CoreEq (Slot 0), (Ty X86_s64)))), (CoreLt (BitsOf (Slot 1)), (CoreSelect Is64Bit, 64, 32)))>,
    CoreLower<(CoreAll (CoreNot HasAVX512), (CoreAny (CoreAll HasSSE1, (CoreEq (Slot 0), (Ty X86_s32))), (CoreAll HasSSE2, (CoreEq (Slot 0), (Ty X86_s64)))), Is64Bit, (CoreEq (Slot 1), (Ty X86_s64)))>,
    CoreWiden<(Slot 0), (Ty X86_s32)>,
    CoreNarrow<(Slot 0), (Ty X86_s64OrS32)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 1), (Ty X86_s32)>,
    CoreNarrow<(Slot 1), (Ty X86_sMaxScalar)>,
    CoreWiden<(Slot 1), (Pow2)>,
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
    CoreLegal<(CoreAll HasAVX512, (CoreInPairs (Slot 0), (Slot 1), [[X86_s32, X86_s32], [X86_s32, X86_s64], [X86_s64, X86_s32], [X86_s64, X86_s64]]))>,
    CoreCustom<(CoreAll (CoreNot HasAVX512), (CoreAny (CoreAll HasSSE1, (CoreEq (Slot 1), (Ty X86_s32))), (CoreAll HasSSE2, (CoreEq (Slot 1), (Ty X86_s64)))), (CoreLt (BitsOf (Slot 0)), (CoreSelect Is64Bit, 64, 32)))>,
    CoreLower<(CoreAll (CoreNot HasAVX512), (CoreAny (CoreAll HasSSE1, (CoreEq (Slot 1), (Ty X86_s32))), (CoreAll HasSSE2, (CoreEq (Slot 1), (Ty X86_s64)))), Is64Bit, (CoreEq (Slot 0), (Ty X86_s64)))>,
    CoreWiden<(Slot 0), (Ty X86_s32)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 1), (Ty X86_s32)>,
    CoreNarrow<(Slot 1), (Ty X86_s64OrS32)>,
    CoreWiden<(Slot 1), (Pow2)>,
  ];
}

def X86_BUILD_VECTOR : LegalizeRuleSet<["G_BUILD_VECTOR"]> {
  let Comments = [
    "vector ops",
    "G_BUILD_VECTOR cannot be legalized by adding elements: the added",
    "elements are undef, and narrowing the widened vector back to the",
    "original type recreates the instruction being legalized, so the",
    "legalizer would re-enqueue the same query forever.  Only clamp the",
    "maximum number of elements; a narrow build vector is reported as",
    "unsupported (and falls back to SelectionDAG) instead of looping.",
    "Lowering non-constant narrow build vectors is tracked by #143099.",
  ];
  let Rules = [
    CoreCustom<(CoreAny (CoreAll HasSSE1, (CoreEq (Slot 0), (Ty X86_v4s32))), (CoreAll HasSSE2, (CoreIn (Slot 0), [X86_v2s64, X86_v8s16, X86_v16s8])), (CoreAll HasAVX, (CoreIn (Slot 0), [X86_v4s64, X86_v8s32, X86_v16s16, X86_v32s8])), (CoreAll HasAVX512, (CoreIn (Slot 0), [X86_v8s64, X86_v16s32, X86_v32s16, X86_v64s8])))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (CoreSelect HasAVX512, 64, (CoreSelect HasAVX, 32, 16)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s8)), (CoreGt (NumElems (Slot 0)), (CoreSelect HasAVX512, 64, (CoreSelect HasAVX, 32, 16))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (CoreSelect HasAVX512, 32, (CoreSelect HasAVX, 16, 8)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s16)), (CoreGt (NumElems (Slot 0)), (CoreSelect HasAVX512, 32, (CoreSelect HasAVX, 16, 8))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (CoreSelect HasAVX512, 16, (CoreSelect HasAVX, 8, 4)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s32)), (CoreGt (NumElems (Slot 0)), (CoreSelect HasAVX512, 16, (CoreSelect HasAVX, 8, 4))))>,
    CoreFewerIf<(Slot 0), (Replace (ScalarOrVecOf (CoreSelect HasAVX512, 8, (CoreSelect HasAVX, 4, 2)), (ElemOf (Slot 0)))), (CoreAll (CoreEq (SortOf (Slot 0)), KindVector), (CoreNot (CoreEq (SortOf (Slot 0)), KindScalableVector)), (CoreEq (ElemOf (Slot 0)), (Ty X86_s64)), (CoreGt (NumElems (Slot 0)), (CoreSelect HasAVX512, 8, (CoreSelect HasAVX, 4, 2))))>,
  ];
}

def X86_EXTRACT : LegalizeRuleSet<["G_EXTRACT"]> {
  let Rules = [
    CoreLegal<(CoreAll HasAVX, (CoreInPairs (Slot 0), (Slot 1), [[X86_v16s8, X86_v32s8], [X86_v8s16, X86_v16s16], [X86_v4s32, X86_v8s32], [X86_v2s64, X86_v4s64]]))>,
    CoreLegal<(CoreAll HasAVX512, (CoreInPairs (Slot 0), (Slot 1), [[X86_v16s8, X86_v64s8], [X86_v32s8, X86_v64s8], [X86_v8s16, X86_v32s16], [X86_v16s16, X86_v32s16], [X86_v4s32, X86_v16s32], [X86_v8s32, X86_v16s32], [X86_v2s64, X86_v8s64], [X86_v4s64, X86_v8s64]]))>,
    CoreAlwaysUnsupported,
  ];
}

def X86_INSERT : LegalizeRuleSet<["G_INSERT"]> {
  let Rules = [
    CoreLegal<(CoreAll HasAVX, (CoreInPairs (Slot 0), (Slot 1), [[X86_v32s8, X86_v16s8], [X86_v16s16, X86_v8s16], [X86_v8s32, X86_v4s32], [X86_v4s64, X86_v2s64]]))>,
    CoreLegal<(CoreAll HasAVX512, (CoreInPairs (Slot 0), (Slot 1), [[X86_v64s8, X86_v16s8], [X86_v64s8, X86_v32s8], [X86_v32s16, X86_v8s16], [X86_v32s16, X86_v16s16], [X86_v16s32, X86_v4s32], [X86_v16s32, X86_v8s32], [X86_v8s64, X86_v2s64], [X86_v8s64, X86_v4s64]]))>,
    CoreAlwaysUnsupported,
  ];
}

def X86_CONCAT_VECTORS : LegalizeRuleSet<["G_CONCAT_VECTORS"]> {
  let Comments = [
    "todo: only permit dst types up to max legal vector register size?",
  ];
  let Rules = [
    CoreLegal<(CoreAll HasSSE1, (CoreInPairs (Slot 0), (Slot 1), [[X86_v32s8, X86_v16s8], [X86_v16s16, X86_v8s16], [X86_v8s32, X86_v4s32], [X86_v4s64, X86_v2s64]]))>,
    CoreLegal<(CoreAll HasAVX, (CoreInPairs (Slot 0), (Slot 1), [[X86_v64s8, X86_v16s8], [X86_v64s8, X86_v32s8], [X86_v32s16, X86_v8s16], [X86_v32s16, X86_v16s16], [X86_v16s32, X86_v4s32], [X86_v16s32, X86_v8s32], [X86_v8s64, X86_v2s64], [X86_v8s64, X86_v4s64]]))>,
  ];
}

def X86_SELECT : LegalizeRuleSet<["G_SELECT"]> {
  let Comments = [
    "todo: vectors and address spaces",
  ];
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), [[X86_s16, X86_s32], [X86_s32, X86_s32], [X86_p0, X86_s32]])>,
    CoreLegal<(CoreAll (CoreNot HasCMOV), (CoreInPairs (Slot 0), (Slot 1), [[X86_s8, X86_s32]]))>,
    CoreLegal<(CoreAll Is64Bit, (CoreInPairs (Slot 0), (Slot 1), [[X86_s64, X86_s32]]))>,
    CoreLegal<(CoreAll UseX87, (CoreInPairs (Slot 0), (Slot 1), [[X86_s80, X86_s32]]))>,
    CoreWiden<(Slot 1), (Ty X86_s32)>,
    CoreNarrow<(Slot 1), (Ty X86_s32)>,
    CoreWiden<(Slot 0), (Pow2Min 8)>,
    CoreWiden<(Slot 0), (Ty X86_CMOVOrS8)>,
    CoreNarrow<(Slot 0), (Ty X86_sMaxScalar)>,
  ];
}

def X86_MEMCPY : LegalizeRuleSet<["G_MEMCPY", "G_MEMMOVE", "G_MEMSET"]> {
  let Comments = [
    "memory intrinsics",
  ];
  let Rules = [
    CoreAlwaysLibcall,
  ];
}

def X86_DYN_STACKALLOC : LegalizeRuleSet<["G_DYN_STACKALLOC", "G_STACKSAVE", "G_STACKRESTORE"]> {
  let Rules = [
    CoreAlwaysLower,
  ];
}

def X86_IS_FPCLASS : LegalizeRuleSet<["G_IS_FPCLASS"]> {
  let Comments = [
    "fp intrinsics",
    "fpclass for i686 is disabled for llvm issue #171992",
  ];
  let Rules = [
    CoreLower<(CoreAll Is64Bit, (CoreInPairs (Slot 0), (Slot 1), [[X86_s1, X86_s32], [X86_s1, X86_s64], [X86_s1, X86_s80]]))>,
  ];
}

def X86_INTRINSIC_ROUNDEVEN : LegalizeRuleSet<["G_INTRINSIC_ROUNDEVEN", "G_INTRINSIC_TRUNC"]> {
  let Rules = [
    CoreScalarize<(Slot 0)>,
    CoreWiden<(Slot 0), (Ty X86_s32)>,
    CoreAlwaysLibcall,
  ];
}

def X86_INTRINSIC : LegalizeRuleSet<["G_INTRINSIC", "G_INTRINSIC_W_SIDE_EFFECTS"]> {
  let Rules = [
    CoreAlwaysLegal,
  ];
}

def X86_TRAP : LegalizeRuleSet<["G_TRAP", "G_DEBUGTRAP", "G_UBSANTRAP"]> {
  let Rules = [
    CoreAlwaysLegal,
  ];
}

def X86_INVOKE_REGION_START : LegalizeRuleSet<["G_INVOKE_REGION_START"]> {
  let Rules = [
    CoreAlwaysLegal,
  ];
}
