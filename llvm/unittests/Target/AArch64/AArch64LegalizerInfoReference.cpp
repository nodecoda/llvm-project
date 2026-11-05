//===-- AArch64LegalizerInfoReference.cpp -----------------------*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Frozen migration reference; see AArch64LegalizerInfoReference.h.
//
//===----------------------------------------------------------------------===//

#include "AArch64LegalizerInfoReference.h"
#include "AArch64Subtarget.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/GlobalISel/GenericMachineInstrs.h"
#include "llvm/CodeGen/GlobalISel/LegalizerHelper.h"
#include "llvm/CodeGen/GlobalISel/MIPatternMatch.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/MathExtras.h"
#include <initializer_list>

using namespace llvm;
using namespace LegalizeActions;
using namespace LegalizeMutations;
using namespace LegalityPredicates;
using namespace MIPatternMatch;

AArch64LegalizerInfoReference::AArch64LegalizerInfoReference(
    const AArch64Subtarget &ST) {
  using namespace TargetOpcode;
  const LLT p0 = LLT::pointer(0, 64);
  const LLT s8 = LLT::scalar(8);
  const LLT s16 = LLT::scalar(16);
  const LLT s32 = LLT::scalar(32);
  const LLT s64 = LLT::scalar(64);
  const LLT s128 = LLT::scalar(128);
  const LLT v16s8 = LLT::fixed_vector(16, 8);
  const LLT v8s8 = LLT::fixed_vector(8, 8);
  const LLT v4s8 = LLT::fixed_vector(4, 8);
  const LLT v2s8 = LLT::fixed_vector(2, 8);
  const LLT v8s16 = LLT::fixed_vector(8, 16);
  const LLT v4s16 = LLT::fixed_vector(4, 16);
  const LLT v2s16 = LLT::fixed_vector(2, 16);
  const LLT v2s32 = LLT::fixed_vector(2, 32);
  const LLT v4s32 = LLT::fixed_vector(4, 32);
  const LLT v2s64 = LLT::fixed_vector(2, 64);
  const LLT v2p0 = LLT::fixed_vector(2, p0);

  const LLT nxv16s8 = LLT::scalable_vector(16, s8);
  const LLT nxv8s16 = LLT::scalable_vector(8, s16);
  const LLT nxv4s32 = LLT::scalable_vector(4, s32);
  const LLT nxv2s64 = LLT::scalable_vector(2, s64);

  const LLT bf16 = LLT::bfloat16();
  const LLT v4bf16 = LLT::fixed_vector(4, bf16);
  const LLT v8bf16 = LLT::fixed_vector(8, bf16);

  const LLT f16 = LLT::float16();
  const LLT v4f16 = LLT::fixed_vector(4, f16);
  const LLT v8f16 = LLT::fixed_vector(8, f16);

  const LLT f32 = LLT::float32();
  const LLT v2f32 = LLT::fixed_vector(2, f32);
  const LLT v4f32 = LLT::fixed_vector(4, f32);

  const LLT f64 = LLT::float64();
  const LLT v2f64 = LLT::fixed_vector(2, f64);

  const LLT f128 = LLT::float128();

  const LLT i8 = LLT::integer(8);
  const LLT v8i8 = LLT::fixed_vector(8, i8);
  const LLT v16i8 = LLT::fixed_vector(16, i8);

  const LLT i16 = LLT::integer(16);
  const LLT v8i16 = LLT::fixed_vector(8, i16);
  const LLT v4i16 = LLT::fixed_vector(4, i16);

  const LLT i32 = LLT::integer(32);
  const LLT v2i32 = LLT::fixed_vector(2, i32);
  const LLT v4i32 = LLT::fixed_vector(4, i32);

  const LLT i64 = LLT::integer(64);
  const LLT v2i64 = LLT::fixed_vector(2, i64);

  const LLT i128 = LLT::integer(128);

  const LLT nxv16i8 = LLT::scalable_vector(16, i8);
  const LLT nxv8i16 = LLT::scalable_vector(8, i16);
  const LLT nxv4i32 = LLT::scalable_vector(4, i32);
  const LLT nxv2i64 = LLT::scalable_vector(2, i64);

  std::initializer_list<LLT> PackedVectorAllTypeList = {/* Begin 128bit types */
                                                        v16s8, v8s16, v4s32,
                                                        v2s64, v2p0,
                                                        /* End 128bit types */
                                                        /* Begin 64bit types */
                                                        v8s8, v4s16, v2s32};
  std::initializer_list<LLT> ScalarAndPtrTypesList = {s8, s16, s32, s64, p0};
  SmallVector<LLT, 8> PackedVectorAllTypesVec(PackedVectorAllTypeList);
  SmallVector<LLT, 8> ScalarAndPtrTypesVec(ScalarAndPtrTypesList);

  const TargetMachine &TM = ST.getTargetLowering()->getTargetMachine();

  // FIXME: support subtargets which have neon/fp-armv8 disabled.
  if (!ST.hasNEON() || !ST.hasFPARMv8())
    return;

  // Some instructions only support s16 if the subtarget has full 16-bit FP
  // support.
  const bool HasFP16 = ST.hasFullFP16();
  const bool HasCSSC = ST.hasCSSC();
  const bool HasRCPC3 = ST.hasRCPC3();
  const bool HasSVE = ST.hasSVE();

  getActionDefinitionsBuilder(
      {G_IMPLICIT_DEF, G_FREEZE, G_CONSTANT_FOLD_BARRIER})
      .legalFor({p0, s8, s16, s32, s64, s128})
      .legalFor({v2s8, v4s8, v8s8, v16s8, v2s16, v4s16, v8s16, v2s32, v4s32,
                 v2s64, v2p0})
      .widenScalarToNextPow2(0)
      .clampScalar(0, s8, s64)
      .moreElementsToNextPow2(0)
      .widenVectorEltsToVectorMinSize(0, 64)
      .clampNumElements(0, v8s8, v16s8)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampMaxNumElements(0, s64, 2)
      .clampMaxNumElements(0, p0, 2)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0);

  getActionDefinitionsBuilder(G_PHI)
      .legalFor({p0, s16, s32, s64})
      .legalFor(PackedVectorAllTypeList)
      .widenScalarToNextPow2(0)
      .moreElementsToNextPow2(0)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .clampScalar(0, s16, s64)
      .clampNumElements(0, v8s8, v16s8)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampMaxNumElements(0, s64, 2)
      .clampMaxNumElements(0, p0, 2)
      .widenScalarOrEltToNextPow2OrMinSize(0, 8);

  getActionDefinitionsBuilder(G_INSERT)
      .legalIf(all(typeInSet(0, {s32, s64, p0}), typeInSet(1, {s8, s16, s32}),
                   smallerThan(1, 0)))
      .widenScalarToNextPow2(0)
      .clampScalar(0, s32, s64)
      .widenScalarToNextPow2(1)
      .minScalar(1, s8)
      .maxScalarIf(typeInSet(0, {s32}), 1, s16)
      .maxScalarIf(typeInSet(0, {s64, p0}), 1, s32);

  getActionDefinitionsBuilder(G_EXTRACT)
      .legalIf(all(typeInSet(0, {s16, s32, s64, p0}),
                   typeInSet(1, {s32, s64, s128, p0}), smallerThan(0, 1)))
      .widenScalarToNextPow2(1)
      .clampScalar(1, s32, s128)
      .widenScalarToNextPow2(0)
      .minScalar(0, s16)
      .maxScalarIf(typeInSet(1, {s32}), 0, s16)
      .maxScalarIf(typeInSet(1, {s64, p0}), 0, s32)
      .maxScalarIf(typeInSet(1, {s128}), 0, s64);

  getActionDefinitionsBuilder({G_ADD, G_SUB, G_AND, G_OR, G_XOR})
      .legalFor({i32, i64, v8i8, v16i8, v4i16, v8i16, v2i32, v4i32, v2i64})
      .legalFor(HasSVE, {nxv16i8, nxv8i16, nxv4i32, nxv2i64})
      .widenScalarToNextPow2(0)
      .clampScalar(0, s32, s64)
      .clampMaxNumElements(0, s8, 16)
      .clampMaxNumElements(0, s16, 8)
      .clampNumElements(0, v2s32, v4s32)
      .clampNumElements(0, v2s64, v2s64)
      .minScalarOrEltIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[0].getNumElements() <= 2;
          },
          0, s32)
      .minScalarOrEltIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[0].getNumElements() <= 4;
          },
          0, s16)
      .minScalarOrEltIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[0].getNumElements() <= 16;
          },
          0, s8)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .moreElementsToNextPow2(0);

  getActionDefinitionsBuilder(G_MUL)
      .legalFor({i32, i64, v8i8, v16i8, v4i16, v8i16, v2i32, v4i32, v2i64})
      .widenScalarToNextPow2(0)
      .clampScalar(0, s32, s64)
      .clampMaxNumElements(0, s8, 16)
      .clampMaxNumElements(0, s16, 8)
      .clampNumElements(0, v2s32, v4s32)
      .clampNumElements(0, v2s64, v2s64)
      .minScalarOrEltIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[0].getNumElements() <= 2;
          },
          0, s32)
      .minScalarOrEltIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[0].getNumElements() <= 4;
          },
          0, s16)
      .minScalarOrEltIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[0].getNumElements() <= 16;
          },
          0, s8)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .moreElementsToNextPow2(0);

  getActionDefinitionsBuilder({G_SHL, G_ASHR, G_LSHR})
      .customIf([=](const LegalityQuery &Query) {
        const auto &SrcTy = Query.Types[0];
        const auto &AmtTy = Query.Types[1];
        return !SrcTy.isVector() && SrcTy.getSizeInBits() == 32 &&
               AmtTy.getSizeInBits() == 32;
      })
      .legalFor({
          {i32, i32},
          {i32, i64},
          {i64, i64},
          {v8i8, v8i8},
          {v16i8, v16i8},
          {v4i16, v4i16},
          {v8i16, v8i16},
          {v2i32, v2i32},
          {v4i32, v4i32},
          {v2i64, v2i64},
      })
      .widenScalarToNextPow2(1)
      .widenScalarToNextPow2(0)
      .clampScalar(1, s32, s64)
      .clampScalar(0, s32, s64)
      .clampNumElements(0, v8s8, v16s8)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampNumElements(0, v2s64, v2s64)
      .moreElementsToNextPow2(0)
      .minScalarSameAs(1, 0)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .minScalarEltSameAsIf(isVector(0), 1, 0)
      .maxScalarEltSameAsIf(isVector(0), 1, 0);

  getActionDefinitionsBuilder(G_PTR_ADD)
      .legalFor({{p0, i64}, {v2p0, v2i64}})
      .clampScalarOrElt(1, s64, s64)
      .clampNumElements(0, v2p0, v2p0);

  getActionDefinitionsBuilder(G_PTRMASK).legalFor({{p0, s64}});

  getActionDefinitionsBuilder({G_SDIV, G_UDIV})
      .legalFor({i32, i64})
      .libcallFor({i128})
      .clampScalar(0, s32, s64)
      .widenScalarToNextPow2(0)
      .scalarize(0);

  getActionDefinitionsBuilder({G_SREM, G_UREM, G_SDIVREM, G_UDIVREM})
      .lowerFor({i8, i16, i32, i64, v2i32, v4i32, v2i64})
      .libcallFor({i128})
      .widenScalarOrEltToNextPow2(0)
      .minScalarOrElt(0, s32)
      .clampNumElements(0, v2s32, v4s32)
      .clampNumElements(0, v2s64, v2s64)
      .scalarize(0);

  getActionDefinitionsBuilder({G_SMULO, G_UMULO})
      .widenScalarToNextPow2(0, /*Min = */ 32)
      .clampScalar(0, s32, s64)
      .lower();

  getActionDefinitionsBuilder({G_SMULH, G_UMULH})
      .legalFor({i64, v16i8, v8i16, v4i32})
      .clampMaxNumElements(0, s8, 16)
      .clampMaxNumElements(0, s16, 8)
      .clampMaxNumElements(0, s32, 4)
      .lower();

  getActionDefinitionsBuilder(
      {G_SMULFIX, G_UMULFIX, G_SMULFIXSAT, G_UMULFIXSAT})
      .lower();

  getActionDefinitionsBuilder({G_SMIN, G_SMAX, G_UMIN, G_UMAX})
      .legalFor({v8i8, v16i8, v4i16, v8i16, v2i32, v4i32})
      .legalFor(HasCSSC, {i32, i64})
      .minScalar(HasCSSC, 0, s32)
      .clampNumElements(0, v8s8, v16s8)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .lower();

  // FIXME: Legal vector types are only legal with NEON.
  getActionDefinitionsBuilder(G_ABS)
      .legalFor(HasCSSC, {i32, i64})
      .legalFor({v16i8, v8i16, v4i32, v2i64, v2p0, v8i8, v4i16, v2i32})
      .customIf([=](const LegalityQuery &Q) {
        // TODO: Fix suboptimal codegen for 128+ bit types.
        LLT SrcTy = Q.Types[0];
        return SrcTy.isScalar() && SrcTy.getSizeInBits() < 128;
      })
      .widenScalarIf(
          [=](const LegalityQuery &Query) { return Query.Types[0] == v4s8; },
          [=](const LegalityQuery &Query) { return std::make_pair(0, v4i16); })
      .widenScalarIf(
          [=](const LegalityQuery &Query) { return Query.Types[0] == v2s16; },
          [=](const LegalityQuery &Query) { return std::make_pair(0, v2i32); })
      .clampNumElements(0, v8s8, v16s8)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampNumElements(0, v2s64, v2s64)
      .moreElementsToNextPow2(0)
      .lower();

  getActionDefinitionsBuilder(
      {G_ABDS, G_ABDU, G_UAVGFLOOR, G_UAVGCEIL, G_SAVGFLOOR, G_SAVGCEIL})
      .legalFor({v8i8, v16i8, v4i16, v8i16, v2i32, v4i32})
      .lower();

  getActionDefinitionsBuilder(
      {G_SADDE, G_SSUBE, G_UADDE, G_USUBE, G_SADDO, G_SSUBO, G_UADDO, G_USUBO})
      .legalFor({{i32, i32}, {i64, i32}})
      .clampScalar(0, s32, s64)
      .clampScalar(1, s32, s64)
      .widenScalarToNextPow2(0)
      .lower();

  getActionDefinitionsBuilder({G_FSHL, G_FSHR})
      .customFor({{i32, i32}, {i32, i64}, {i64, i64}})
      .lower();

  getActionDefinitionsBuilder(G_ROTR)
      .legalFor({{i32, i64}, {i64, i64}})
      .customIf([=](const LegalityQuery &Q) {
        return Q.Types[0].isScalar() && Q.Types[1].getScalarSizeInBits() < 64;
      })
      .lower();
  getActionDefinitionsBuilder(G_ROTL).lower();

  getActionDefinitionsBuilder({G_SBFX, G_UBFX})
      .customFor({{s32, s32}, {s64, s64}});

  auto always = [=](const LegalityQuery &Q) { return true; };
  getActionDefinitionsBuilder(G_CTPOP)
      .legalFor(HasCSSC, {{i32, i32}, {i64, i64}})
      .legalFor({{v8i8, v8i8}, {v16i8, v16i8}})
      .customFor(!HasCSSC, {{s32, s32}, {s64, s64}})
      .customFor({{s128, s128},
                  {v4s16, v4s16},
                  {v8s16, v8s16},
                  {v2s32, v2s32},
                  {v4s32, v4s32},
                  {v2s64, v2s64}})
      .clampScalar(0, s32, s128)
      .widenScalarToNextPow2(0)
      .widenScalarOrEltToNextPow2OrMinSize(0, 8)
      .minScalarEltSameAsIf(always, 1, 0)
      .maxScalarEltSameAsIf(always, 1, 0)
      .clampNumElements(0, v8s8, v16s8)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampNumElements(0, v2s64, v2s64)
      .moreElementsToNextPow2(0)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0);

  getActionDefinitionsBuilder({G_CTLZ, G_CTLS})
      .legalFor({{i32, i32},
                 {i64, i64},
                 {v8i8, v8i8},
                 {v16i8, v16i8},
                 {v4i16, v4i16},
                 {v8i16, v8i16},
                 {v2i32, v2i32},
                 {v4i32, v4i32}})
      .widenScalarToNextPow2(1, /*Min=*/32)
      .clampScalar(1, s32, s64)
      .widenScalarOrEltToNextPow2OrMinSize(1, /*Min=*/8)
      .clampNumElements(0, v8s8, v16s8)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .moreElementsToNextPow2(0)
      .scalarizeIf(scalarOrEltWiderThan(0, 32), 0)
      .scalarSameSizeAs(0, 1);

  getActionDefinitionsBuilder(G_INSERT_SUBVECTOR).lower();

  getActionDefinitionsBuilder(G_CTLZ_ZERO_POISON).lower();

  getActionDefinitionsBuilder(G_CTTZ)
      .lowerIf(isVector(0))
      .widenScalarToNextPow2(1, /*Min=*/32)
      .clampScalar(1, s32, s64)
      .scalarSameSizeAs(0, 1)
      .legalFor(HasCSSC, {s32, s64})
      .customFor(!HasCSSC, {s32, s64});

  getActionDefinitionsBuilder(G_CTTZ_ZERO_POISON).lower();

  getActionDefinitionsBuilder(G_BITREVERSE)
      .legalFor({i32, i64, v8i8, v16i8})
      .widenScalarToNextPow2(0, /*Min = */ 32)
      .widenScalarOrEltToNextPow2OrMinSize(0, 8)
      .clampScalar(0, s32, s64)
      .clampNumElements(0, v8s8, v16s8)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampNumElements(0, v2s64, v2s64)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .moreElementsToNextPow2(0)
      .lower();

  getActionDefinitionsBuilder(G_CLMUL).legalFor({v8i8, v16i8});

  getActionDefinitionsBuilder(G_BSWAP)
      .legalFor({i32, i64, v4i16, v8i16, v2i32, v4i32, v2i64})
      .widenScalarOrEltToNextPow2(0, 16)
      .clampScalar(0, s32, s64)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampNumElements(0, v2s64, v2s64)
      .moreElementsToNextPow2(0);

  getActionDefinitionsBuilder({G_UADDSAT, G_SADDSAT, G_USUBSAT, G_SSUBSAT})
      .legalFor({v8i8, v16i8, v4i16, v8i16, v2i32, v4i32, v2i64})
      .legalFor(HasSVE, {nxv16i8, nxv8i16, nxv4i32, nxv2i64})
      .clampNumElements(0, v8s8, v16s8)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampMaxNumElements(0, s64, 2)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .moreElementsToNextPow2(0)
      .lower();

  getActionDefinitionsBuilder(
      {G_FADD, G_FSUB, G_FMUL, G_FDIV, G_FMA, G_FSQRT, G_FMAXNUM, G_FMINNUM,
       G_FMAXIMUM, G_FMINIMUM, G_FCEIL, G_FFLOOR, G_FRINT, G_FNEARBYINT,
       G_INTRINSIC_TRUNC, G_INTRINSIC_ROUND, G_INTRINSIC_ROUNDEVEN})
      .legalFor({f32, f64, v2f32, v4f32, v2f64})
      .legalFor(HasFP16, {f16, v4f16, v8f16})
      .libcallFor({f128})
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .widenScalarIf(
          [=](const LegalityQuery &Q) {
            return (!HasFP16 && Q.Types[0].getScalarType().isFloat16()) ||
                   Q.Types[0].getScalarType().isBFloat16();
          },
          changeElementTo(0, f32))
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampNumElements(0, v2s64, v2s64)
      .moreElementsToNextPow2(0);

  getActionDefinitionsBuilder({G_FABS, G_FNEG})
      .legalFor({f32, f64, v2f32, v4f32, v2f64})
      .legalFor(HasFP16, {f16, bf16, v4f16, v4bf16, v8f16, v8bf16})
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .lowerIf(scalarOrEltWiderThan(0, 64))
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampNumElements(0, v2s64, v2s64)
      .moreElementsToNextPow2(0)
      .lowerFor({f16, bf16, v4f16, v4bf16, v8f16, v8bf16});

  getActionDefinitionsBuilder({G_FREM, G_FCOS, G_FSIN, G_FPOW, G_FLOG, G_FLOG2,
                               G_FLOG10, G_FTAN, G_FEXP, G_FEXP2, G_FEXP10,
                               G_FACOS, G_FASIN, G_FATAN, G_FATAN2, G_FCOSH,
                               G_FSINH, G_FTANH, G_FMODF})
      .libcallFor({f32, f64, f128})
      .widenScalarFor({f16, bf16}, changeElementTo(0, f32))
      .scalarize(0);
  getActionDefinitionsBuilder({G_FPOWI, G_FLDEXP})
      .libcallFor({{f32, i32}, {f64, i32}, {f128, i32}})
      .widenScalarFor({f16, bf16}, changeElementTo(0, f32))
      .scalarize(0);

  getActionDefinitionsBuilder({G_LROUND, G_INTRINSIC_LRINT})
      .legalFor({{i32, f32}, {i32, f64}, {i64, f32}, {i64, f64}})
      .legalFor(HasFP16, {{i32, f16}, {i64, f16}})
      .minScalar(1, s32)
      .libcallFor({{s64, s128}})
      .lower();
  getActionDefinitionsBuilder({G_LLROUND, G_INTRINSIC_LLRINT})
      .legalFor({{i64, f32}, {i64, f64}})
      .legalFor(HasFP16, {{i64, f16}})
      .minScalar(0, s64)
      .minScalar(1, s32)
      .libcallFor({{s64, s128}})
      .lower();

  // TODO: Custom legalization for mismatched types.
  getActionDefinitionsBuilder(G_FCOPYSIGN)
      .moreElementsIf(
          [](const LegalityQuery &Query) { return Query.Types[0].isScalar(); },
          [=](const LegalityQuery &Query) {
            const LLT Ty = Query.Types[0];
            return std::pair(0, LLT::fixed_vector(Ty == s16 ? 4 : 2, Ty));
          })
      .lower();

  getActionDefinitionsBuilder(G_FMAD).lower();

  for (unsigned Op : {G_SEXTLOAD, G_ZEXTLOAD}) {
    auto &Actions = getActionDefinitionsBuilder(Op);

    if (Op == G_SEXTLOAD)
      Actions.lowerIf(
          atomicOrderingAtLeastOrStrongerThan(0, AtomicOrdering::Unordered));

    // Atomics have zero extending behavior.
    Actions
        .legalForTypesWithMemDesc({{s32, p0, s8, 8},
                                   {s32, p0, s16, 8},
                                   {s32, p0, s32, 8},
                                   {s64, p0, s8, 2},
                                   {s64, p0, s16, 2},
                                   {s64, p0, s32, 4},
                                   {s64, p0, s64, 8},
                                   {p0, p0, s64, 8},
                                   {v2s32, p0, s64, 8}})
        .widenScalarToNextPow2(0)
        .clampScalar(0, s32, s64)
        // TODO: We could support sum-of-pow2's but the lowering code doesn't
        // know
        //       how to do that yet.
        .unsupportedIfMemSizeNotPow2()
        // Lower anything left over into G_*EXT and G_LOAD
        .lower();
  }

  auto IsPtrVecPred = [=](const LegalityQuery &Query) {
    const LLT &ValTy = Query.Types[0];
    return ValTy.isPointerVector() && ValTy.getAddressSpace() == 0;
  };

  getActionDefinitionsBuilder(G_LOAD)
      .customIf([=](const LegalityQuery &Query) {
        return HasRCPC3 && Query.Types[0] == s128 &&
               Query.MMODescrs[0].Ordering == AtomicOrdering::Acquire;
      })
      .customIf([=](const LegalityQuery &Query) {
        return Query.Types[0] == s128 &&
               Query.MMODescrs[0].Ordering != AtomicOrdering::NotAtomic;
      })
      .legalForTypesWithMemDesc({{s8, p0, s8, 8},
                                 {s16, p0, s16, 8},
                                 {s32, p0, s32, 8},
                                 {s64, p0, s64, 8},
                                 {p0, p0, s64, 8},
                                 {s128, p0, s128, 8},
                                 {v8s8, p0, s64, 8},
                                 {v16s8, p0, s128, 8},
                                 {v4s16, p0, s64, 8},
                                 {v8s16, p0, s128, 8},
                                 {v2s32, p0, s64, 8},
                                 {v4s32, p0, s128, 8},
                                 {v2s64, p0, s128, 8}})
      // These extends are also legal
      .legalForTypesWithMemDesc(
          {{s32, p0, s8, 8}, {s32, p0, s16, 8}, {s64, p0, s32, 8}})
      .legalForTypesWithMemDesc({
          // SVE vscale x 128 bit base sizes
          {nxv16s8, p0, nxv16s8, 8},
          {nxv8s16, p0, nxv8s16, 8},
          {nxv4s32, p0, nxv4s32, 8},
          {nxv2s64, p0, nxv2s64, 8},
      })
      .widenScalarToNextPow2(0, /* MinSize = */ 8)
      .clampMaxNumElements(0, s8, 16)
      .clampMaxNumElements(0, s16, 8)
      .clampMaxNumElements(0, s32, 4)
      .clampMaxNumElements(0, s64, 2)
      .clampMaxNumElements(0, p0, 2)
      .lowerIfMemSizeNotByteSizePow2()
      .clampScalar(0, s8, s64)
      .narrowScalarIf(
          [=](const LegalityQuery &Query) {
            // Clamp extending load results to 32-bits.
            return Query.Types[0].isScalar() &&
                   Query.Types[0] != Query.MMODescrs[0].MemoryTy &&
                   Query.Types[0].getSizeInBits() > 32;
          },
          changeTo(0, i32))
      // TODO: Use BITCAST for v2i8, v2i16 after G_TRUNC gets sorted out
      .bitcastIf(typeInSet(0, {v4s8}),
                 [=](const LegalityQuery &Query) {
                   const LLT VecTy = Query.Types[0];
                   return std::pair(0, LLT::integer(VecTy.getSizeInBits()));
                 })
      .customIf(IsPtrVecPred)
      .scalarizeIf(typeInSet(0, {v2s16, v2s8}), 0)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0);

  getActionDefinitionsBuilder(G_STORE)
      .customIf([=](const LegalityQuery &Query) {
        return HasRCPC3 && Query.Types[0] == s128 &&
               Query.MMODescrs[0].Ordering == AtomicOrdering::Release;
      })
      .customIf([=](const LegalityQuery &Query) {
        return Query.Types[0] == s128 &&
               Query.MMODescrs[0].Ordering != AtomicOrdering::NotAtomic;
      })
      .widenScalarIf(
          all(scalarNarrowerThan(0, 32),
              atomicOrderingAtLeastOrStrongerThan(0, AtomicOrdering::Release)),
          changeElementSizeTo(0, s32))
      .legalForTypesWithMemDesc(
          {{s8, p0, s8, 8},     {s16, p0, s8, 8},  // truncstorei8 from s16
           {s32, p0, s8, 8},                       // truncstorei8 from s32
           {s64, p0, s8, 8},                       // truncstorei8 from s64
           {s16, p0, s16, 8},   {s32, p0, s16, 8}, // truncstorei16 from s32
           {s64, p0, s16, 8},                      // truncstorei16 from s64
           {s32, p0, s8, 8},    {s32, p0, s16, 8},    {s32, p0, s32, 8},
           {s64, p0, s64, 8},   {s64, p0, s32, 8}, // truncstorei32 from s64
           {p0, p0, s64, 8},    {s128, p0, s128, 8},  {v16s8, p0, s128, 8},
           {v8s8, p0, s64, 8},  {v4s16, p0, s64, 8},  {v8s16, p0, s128, 8},
           {v2s32, p0, s64, 8}, {v4s32, p0, s128, 8}, {v2s64, p0, s128, 8}})
      .legalForTypesWithMemDesc({
          // SVE vscale x 128 bit base sizes
          // TODO: Add nxv2p0. Consider bitcastIf.
          //       See #92130
          // https://github.com/llvm/llvm-project/pull/92130#discussion_r1616888461
          {nxv16s8, p0, nxv16s8, 8},
          {nxv8s16, p0, nxv8s16, 8},
          {nxv4s32, p0, nxv4s32, 8},
          {nxv2s64, p0, nxv2s64, 8},
      })
      .clampScalar(0, s8, s64)
      .minScalarOrElt(0, s8)
      .lowerIf([=](const LegalityQuery &Query) {
        return Query.Types[0].isScalar() &&
               Query.Types[0] != Query.MMODescrs[0].MemoryTy;
      })
      // Maximum: sN * k = 128
      .clampMaxNumElements(0, s8, 16)
      .clampMaxNumElements(0, s16, 8)
      .clampMaxNumElements(0, s32, 4)
      .clampMaxNumElements(0, s64, 2)
      .clampMaxNumElements(0, p0, 2)
      .lowerIfMemSizeNotPow2()
      // TODO: Use BITCAST for v2i8, v2i16 after G_TRUNC gets sorted out
      .bitcastIf(all(typeInSet(0, {v4s8}),
                     LegalityPredicate([=](const LegalityQuery &Query) {
                       return Query.Types[0].getSizeInBits() ==
                              Query.MMODescrs[0].MemoryTy.getSizeInBits();
                     })),
                 [=](const LegalityQuery &Query) {
                   const LLT VecTy = Query.Types[0];
                   return std::pair(0, LLT::integer(VecTy.getSizeInBits()));
                 })
      .customIf(IsPtrVecPred)
      .scalarizeIf(typeInSet(0, {v2s16, v2s8}), 0)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .lower();

  getActionDefinitionsBuilder(G_INDEXED_STORE)
      // Idx 0 == Ptr, Idx 1 == Val
      // TODO: we can implement legalizations but as of now these are
      // generated in a very specific way.
      .legalForTypesWithMemDesc({
          {p0, s8, s8, 8},
          {p0, s16, s16, 8},
          {p0, s32, s8, 8},
          {p0, s32, s16, 8},
          {p0, s32, s32, 8},
          {p0, s64, s64, 8},
          {p0, p0, p0, 8},
          {p0, v8s8, v8s8, 8},
          {p0, v16s8, v16s8, 8},
          {p0, v4s16, v4s16, 8},
          {p0, v8s16, v8s16, 8},
          {p0, v2s32, v2s32, 8},
          {p0, v4s32, v4s32, 8},
          {p0, v2s64, v2s64, 8},
          {p0, v2p0, v2p0, 8},
          {p0, s128, s128, 8},
      })
      .unsupported();

  auto IndexedLoadBasicPred = [=](const LegalityQuery &Query) {
    LLT LdTy = Query.Types[0];
    LLT PtrTy = Query.Types[1];
    if (!llvm::is_contained(PackedVectorAllTypesVec, LdTy) &&
        !llvm::is_contained(ScalarAndPtrTypesVec, LdTy) && LdTy != s128)
      return false;
    if (PtrTy != p0)
      return false;
    return true;
  };
  getActionDefinitionsBuilder(G_INDEXED_LOAD)
      .unsupportedIf(
          atomicOrderingAtLeastOrStrongerThan(0, AtomicOrdering::Unordered))
      .legalIf(IndexedLoadBasicPred)
      .unsupported();
  getActionDefinitionsBuilder({G_INDEXED_SEXTLOAD, G_INDEXED_ZEXTLOAD})
      .unsupportedIf(
          atomicOrderingAtLeastOrStrongerThan(0, AtomicOrdering::Unordered))
      .legalIf(all(typeInSet(0, {s16, s32, s64}),
                   LegalityPredicate([=](const LegalityQuery &Q) {
                     LLT LdTy = Q.Types[0];
                     LLT PtrTy = Q.Types[1];
                     LLT MemTy = Q.MMODescrs[0].MemoryTy;
                     if (PtrTy != p0)
                       return false;
                     if (LdTy == s16)
                       return MemTy == s8;
                     if (LdTy == s32)
                       return MemTy == s8 || MemTy == s16;
                     if (LdTy == s64)
                       return MemTy == s8 || MemTy == s16 || MemTy == s32;
                     return false;
                   })))
      .unsupported();

  // Constants
  getActionDefinitionsBuilder(G_CONSTANT)
      .legalFor({p0, s8, s16, s32, s64})
      .widenScalarToNextPow2(0)
      .clampScalar(0, s8, s64);
  getActionDefinitionsBuilder(G_FCONSTANT).legalFor({s16, s32, s64, s128});

  // FIXME: fix moreElementsToNextPow2
  getActionDefinitionsBuilder(G_ICMP)
      .legalFor({{i32, i32}, {i32, i64}, {i32, p0}})
      .widenScalarOrEltToNextPow2(1)
      .minScalarOrElt(1, s8)
      .clampScalar(1, s32, s64)
      .clampScalar(0, s32, s32)
      .scalarizeIf(scalarOrEltWiderThan(1, 64), 1)
      .minScalarEltSameAsIf(
          [=](const LegalityQuery &Query) {
            const LLT &Ty = Query.Types[0];
            const LLT &SrcTy = Query.Types[1];
            return Ty.isVector() && !SrcTy.isPointerVector() &&
                   Ty.getElementType() != SrcTy.getElementType();
          },
          0, 1)
      .minScalarOrEltIf(
          [=](const LegalityQuery &Query) { return Query.Types[1] == v2s16; },
          1, s32)
      .minScalarOrEltIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[1].isPointerVector();
          },
          0, s64)
      .moreElementsToNextPow2(1)
      .clampNumElements(1, v8s8, v16s8)
      .clampNumElements(1, v4s16, v8s16)
      .clampNumElements(1, v2s32, v4s32)
      .clampNumElements(1, v2s64, v2s64)
      .clampNumElements(1, v2p0, v2p0)
      .customIf(isVector(0));

  getActionDefinitionsBuilder(G_FCMP)
      .legalFor({{i32, f32},
                 {i32, f64},
                 {v4i32, v4f32},
                 {v2i32, v2f32},
                 {v2i64, v2f64}})
      .legalFor(HasFP16, {{i32, f16}, {v4i16, v4f16}, {v8i16, v8f16}})
      .widenScalarOrEltToNextPow2(1)
      .clampScalar(0, s32, s32)
      .widenScalarIf(
          [=](const LegalityQuery &Q) {
            return (!HasFP16 && Q.Types[1].getScalarType().isFloat16()) ||
                   Q.Types[1].getScalarType().isBFloat16();
          },
          changeElementTo(1, f32))
      .scalarizeIf(scalarOrEltWiderThan(1, 64), 1)
      .minScalarEltSameAsIf(
          [=](const LegalityQuery &Query) {
            const LLT &Ty = Query.Types[0];
            const LLT &SrcTy = Query.Types[1];
            return Ty.isVector() && !SrcTy.isPointerVector() &&
                   Ty.getElementType() != SrcTy.getElementType();
          },
          0, 1)
      .clampNumElements(1, v4s16, v8s16)
      .clampNumElements(1, v2s32, v4s32)
      .clampMaxNumElements(1, s64, 2)
      .moreElementsToNextPow2(1)
      .libcallFor({{s32, s128}});

  // Extensions
  auto ExtLegalFunc = [=](const LegalityQuery &Query) {
    unsigned DstSize = Query.Types[0].getSizeInBits();

    // Handle legal vectors using legalFor
    if (Query.Types[0].isVector())
      return false;

    if (DstSize < 8 || DstSize >= 128 || !isPowerOf2_32(DstSize))
      return false; // Extending to a scalar s128 needs narrowing.

    const LLT &SrcTy = Query.Types[1];

    // Make sure we fit in a register otherwise. Don't bother checking that
    // the source type is below 128 bits. We shouldn't be allowing anything
    // through which is wider than the destination in the first place.
    unsigned SrcSize = SrcTy.getSizeInBits();
    if (SrcSize < 8 || !isPowerOf2_32(SrcSize))
      return false;

    return true;
  };
  getActionDefinitionsBuilder({G_ZEXT, G_SEXT, G_ANYEXT})
      .legalIf(ExtLegalFunc)
      .legalFor({{v8s16, v8s8}, {v4s32, v4s16}, {v2s64, v2s32}})
      .clampScalar(0, s64, s64) // Just for s128, others are handled above.
      .moreElementsToNextPow2(0)
      .clampMaxNumElements(1, s8, 8)
      .clampMaxNumElements(1, s16, 4)
      .clampMaxNumElements(1, s32, 2)
      // Tries to convert a large EXTEND into two smaller EXTENDs
      .lowerIf([=](const LegalityQuery &Query) {
        return (Query.Types[0].getScalarSizeInBits() >
                Query.Types[1].getScalarSizeInBits() * 2) &&
               Query.Types[0].isVector() &&
               (Query.Types[1].getScalarSizeInBits() == 8 ||
                Query.Types[1].getScalarSizeInBits() == 16);
      })
      .clampMinNumElements(1, s8, 8)
      .clampMinNumElements(1, s16, 4)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0);

  getActionDefinitionsBuilder(G_TRUNC)
      .legalFor({{v8s8, v8s16}, {v4s16, v4s32}, {v2s32, v2s64}})
      .moreElementsToNextPow2(0)
      .clampMaxNumElements(0, s8, 8)
      .clampMaxNumElements(0, s16, 4)
      .clampMaxNumElements(0, s32, 2)
      .minScalarOrEltIf(
          [=](const LegalityQuery &Query) { return Query.Types[0].isVector(); },
          0, s8)
      .lowerIf([=](const LegalityQuery &Query) {
        LLT DstTy = Query.Types[0];
        LLT SrcTy = Query.Types[1];
        return DstTy.isVector() && SrcTy.getSizeInBits() > 128 &&
               DstTy.getScalarSizeInBits() * 2 <= SrcTy.getScalarSizeInBits();
      })
      .clampMinNumElements(0, s8, 8)
      .clampMinNumElements(0, s16, 4)
      .alwaysLegal();

  getActionDefinitionsBuilder({G_TRUNC_SSAT_S, G_TRUNC_SSAT_U, G_TRUNC_USAT_U})
      .legalFor({{v8i8, v8i16}, {v4i16, v4i32}, {v2i32, v2i64}})
      .clampNumElements(0, v8s8, v8s8)
      .clampNumElements(0, v4s16, v4s16)
      .clampNumElements(0, v2s32, v2s32)
      .lower();

  getActionDefinitionsBuilder(G_SEXT_INREG)
      .legalFor({i32, i64, v8i8, v16i8, v4i16, v8i16, v2i32, v4i32, v2i64})
      .maxScalar(0, s64)
      .clampNumElements(0, v8s8, v16s8)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampMaxNumElements(0, s64, 2)
      .lower();

  // FP conversions
  getActionDefinitionsBuilder(G_FPTRUNC)
      .legalFor(
          {{f16, f32}, {f16, f64}, {f32, f64}, {v4f16, v4f32}, {v2f32, v2f64}})
      .legalFor(ST.hasBF16(), {{bf16, f32}, {v4bf16, v4f32}})
      .libcallFor({{f16, f128}, {f32, f128}, {f64, f128}})
      .moreElementsToNextPow2(1)
      .customIf([](const LegalityQuery &Q) {
        LLT DstTy = Q.Types[0];
        LLT SrcTy = Q.Types[1];
        return SrcTy.getScalarSizeInBits() == 64 &&
               DstTy.getScalarSizeInBits() == 16;
      })
      .lowerFor({{bf16, f32}, {v4bf16, v4f32}})
      // Clamp based on input
      .clampNumElements(1, v4s32, v4s32)
      .clampNumElements(1, v2s64, v2s64)
      .scalarize(0);

  getActionDefinitionsBuilder(G_FPEXT)
      .legalFor({{f32, f16},
                 {f64, f16},
                 {f32, bf16},
                 {f64, f32},
                 {v4f32, v4f16},
                 {v4f32, v4bf16},
                 {v2f64, v2f32}})
      .libcallFor({{f128, f64}, {f128, f32}, {f128, f16}})
      .moreElementsToNextPow2(0)
      .widenScalarIf(
          [](const LegalityQuery &Q) {
            LLT DstTy = Q.Types[0];
            LLT SrcTy = Q.Types[1];
            return SrcTy.isVector() && DstTy.isVector() &&
                   SrcTy.getScalarSizeInBits() == 16 &&
                   DstTy.getScalarSizeInBits() == 64;
          },
          changeElementTo(1, f32))
      .clampNumElements(0, v4s32, v4s32)
      .clampNumElements(0, v2s64, v2s64)
      .scalarize(0);

  // Conversions
  getActionDefinitionsBuilder({G_FPTOSI, G_FPTOUI})
      .legalFor({{i32, f32},
                 {i64, f32},
                 {i32, f64},
                 {i64, f64},
                 {v2i32, v2f32},
                 {v4i32, v4f32},
                 {v2i64, v2f64}})
      .legalFor(HasFP16,
                {{i32, f16}, {i64, f16}, {v4i16, v4f16}, {v8i16, v8f16}})
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .scalarizeIf(scalarOrEltWiderThan(1, 64), 1)
      // The range of a fp16 value fits into an i17, so we can lower the width
      // to i64.
      .narrowScalarIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[1] == f16 && Query.Types[0].getSizeInBits() > 64;
          },
          changeTo(0, i64))
      .moreElementsToNextPow2(0)
      .widenScalarOrEltToNextPow2OrMinSize(0)
      .minScalar(0, s32)
      .widenScalarIf(
          [HasFP16](const LegalityQuery &Query) {
            return (!HasFP16 && Query.Types[1].getScalarType().isFloat16()) ||
                   Query.Types[1].getScalarType().isBFloat16();
          },
          changeElementTo(1, f32))
      .widenScalarIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[0].getScalarSizeInBits() <= 64 &&
                   Query.Types[0].getScalarSizeInBits() >
                       Query.Types[1].getScalarSizeInBits();
          },
          LegalizeMutations::changeElementSizeTo(1, 0))
      .widenScalarIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[1].getScalarSizeInBits() <= 64 &&
                   Query.Types[0].getScalarSizeInBits() <
                       Query.Types[1].getScalarSizeInBits();
          },
          LegalizeMutations::changeElementSizeTo(0, 1))
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampMaxNumElements(0, s64, 2)
      .libcallFor(
          {{i32, f128}, {i64, f128}, {i128, f128}, {i128, f32}, {i128, f64}});

  getActionDefinitionsBuilder({G_FPTOSI_SAT, G_FPTOUI_SAT})
      .legalFor({{i32, f32},
                 {i64, f32},
                 {i32, f64},
                 {i64, f64},
                 {v2i32, v2f32},
                 {v4i32, v4f32},
                 {v2i64, v2f64}})
      .legalFor(
          HasFP16,
          {{i16, f16}, {i32, f16}, {i64, f16}, {v4i16, v4f16}, {v8i16, v8f16}})
      // Handle types larger than i64 by scalarizing/lowering.
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .scalarizeIf(scalarOrEltWiderThan(1, 64), 1)
      // The range of a fp16 value fits into an i17, so we can lower the width
      // to i64.
      .narrowScalarIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[1] == f16 && Query.Types[0].getSizeInBits() > 64;
          },
          changeTo(0, i64))
      .lowerIf(::any(scalarWiderThan(0, 64), scalarWiderThan(1, 64)), 0)
      .moreElementsToNextPow2(0)
      .widenScalarToNextPow2(0, /*MinSize=*/32)
      .minScalar(0, s32)
      .widenScalarIf(
          [HasFP16](const LegalityQuery &Query) {
            return (!HasFP16 && Query.Types[1].getScalarType().isFloat16()) ||
                   Query.Types[1].getScalarType().isBFloat16();
          },
          changeElementTo(1, f32))
      .widenScalarIf(
          [=](const LegalityQuery &Query) {
            unsigned ITySize = Query.Types[0].getScalarSizeInBits();
            return (ITySize == 16 || ITySize == 32 || ITySize == 64) &&
                   ITySize > Query.Types[1].getScalarSizeInBits();
          },
          LegalizeMutations::changeElementSizeTo(1, 0))
      .widenScalarIf(
          [=](const LegalityQuery &Query) {
            unsigned FTySize = Query.Types[1].getScalarSizeInBits();
            return (FTySize == 16 || FTySize == 32 || FTySize == 64) &&
                   Query.Types[0].getScalarSizeInBits() < FTySize;
          },
          LegalizeMutations::changeElementSizeTo(0, 1))
      .widenScalarOrEltToNextPow2(0)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampMaxNumElements(0, s64, 2);

  getActionDefinitionsBuilder({G_SITOFP, G_UITOFP})
      .legalFor({{f32, i32},
                 {f64, i32},
                 {f32, i64},
                 {f64, i64},
                 {v2f32, v2i32},
                 {v4f32, v4i32},
                 {v2f64, v2i64}})
      .legalFor(HasFP16,
                {{f16, i32}, {f16, i64}, {v4f16, v4i16}, {v8f16, v8i16}})
      .unsupportedIf([&](const LegalityQuery &Query) {
        return Query.Types[0].getScalarType().isBFloat16();
      })
      .scalarizeIf(scalarOrEltWiderThan(1, 64), 1)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .moreElementsToNextPow2(1)
      .widenScalarOrEltToNextPow2OrMinSize(1)
      .minScalar(1, f32)
      .lowerIf([](const LegalityQuery &Query) {
        return Query.Types[1].isVector() &&
               Query.Types[1].getScalarSizeInBits() == 64 &&
               Query.Types[0].getScalarSizeInBits() == 16;
      })
      .widenScalarOrEltToNextPow2OrMinSize(0, /*MinSize=*/HasFP16 ? 16 : 32)
      .scalarizeIf(
          // v2i64->v2f32 needs to scalarize to avoid double-rounding issues.
          [](const LegalityQuery &Query) {
            return Query.Types[0].getScalarSizeInBits() == 32 &&
                   Query.Types[1].getScalarSizeInBits() == 64;
          },
          0)
      .widenScalarIf(
          [](const LegalityQuery &Query) {
            return Query.Types[1].getScalarSizeInBits() <= 64 &&
                   Query.Types[0].getScalarSizeInBits() <
                       Query.Types[1].getScalarSizeInBits();
          },
          LegalizeMutations::changeElementSizeTo(0, 1))
      .widenScalarIf(
          [](const LegalityQuery &Query) {
            return Query.Types[0].getScalarSizeInBits() <= 64 &&
                   Query.Types[0].getScalarSizeInBits() >
                       Query.Types[1].getScalarSizeInBits();
          },
          LegalizeMutations::changeElementSizeTo(1, 0))
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampMaxNumElements(0, s64, 2)
      .libcallFor({{f16, i128},
                   {f32, i128},
                   {f64, i128},
                   {f128, i128},
                   {f128, i32},
                   {f128, i64}});

  // Control-flow
  getActionDefinitionsBuilder(G_BR).alwaysLegal();
  getActionDefinitionsBuilder(G_BRCOND).legalFor({s32}).clampScalar(0, s32,
                                                                    s32);
  getActionDefinitionsBuilder(G_BRINDIRECT).legalFor({p0});

  getActionDefinitionsBuilder(G_SELECT)
      .legalFor({{s32, s32}, {s64, s32}, {p0, s32}})
      .widenScalarToNextPow2(0)
      .clampScalar(0, s32, s64)
      .clampScalar(1, s32, s32)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .minScalarEltSameAsIf(all(isVector(0), isVector(1)), 1, 0)
      .lowerIf(isVector(0));

  // Pointer-handling
  getActionDefinitionsBuilder(G_FRAME_INDEX).legalFor({p0});

  if (TM.getCodeModel() == CodeModel::Small)
    getActionDefinitionsBuilder(G_GLOBAL_VALUE).custom();
  else
    getActionDefinitionsBuilder(G_GLOBAL_VALUE).legalFor({p0});

  getActionDefinitionsBuilder(G_PTRAUTH_GLOBAL_VALUE)
      .legalIf(all(typeIs(0, p0), typeIs(1, p0)));

  getActionDefinitionsBuilder(G_PTRTOINT)
      .legalFor({{i64, p0}, {v2i64, v2p0}})
      .widenScalarToNextPow2(0, 64)
      .clampScalar(0, s64, s64)
      .clampMaxNumElements(0, s64, 2);

  getActionDefinitionsBuilder(G_INTTOPTR)
      .unsupportedIf([&](const LegalityQuery &Query) {
        return Query.Types[0].getSizeInBits() != Query.Types[1].getSizeInBits();
      })
      .legalFor({{p0, i64}, {v2p0, v2i64}})
      .clampMaxNumElements(1, s64, 2);

  // Casts for 32 and 64-bit width type are just copies.
  // Same for 128-bit width type, except they are on the FPR bank.
  getActionDefinitionsBuilder(G_BITCAST)
      .legalForCartesianProduct({s16})
      // Keeping 32-bit instructions legal to prevent regression in some tests
      .legalForCartesianProduct({s32, v2s16, v4s8})
      .legalForCartesianProduct({s64, v8s8, v4s16, v2s32})
      .legalForCartesianProduct({s128, v16s8, v8s16, v4s32, v2s64, v2p0})
      .customIf([=](const LegalityQuery &Query) {
        // Handle casts from i1 vectors to scalars.
        LLT DstTy = Query.Types[0];
        LLT SrcTy = Query.Types[1];
        return DstTy.isScalar() && SrcTy.isVector() &&
               SrcTy.getScalarSizeInBits() == 1;
      })
      .lowerIf([=](const LegalityQuery &Query) {
        return Query.Types[0].isVector() != Query.Types[1].isVector();
      })
      // moreElementsToNextPow2 cannot pad the source to match, so lower
      .lowerIf([=](const LegalityQuery &Query) {
        LLT DstTy = Query.Types[0];
        LLT SrcTy = Query.Types[1];
        if (!DstTy.isFixedVector() || !SrcTy.isFixedVector())
          return false;
        unsigned MoreElts = 1u << Log2_32_Ceil(DstTy.getNumElements());
        return SrcTy.getNumElements() * MoreElts % DstTy.getNumElements() != 0;
      })
      .moreElementsToNextPow2(0)
      .clampNumElements(0, v8s8, v16s8)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampMaxNumElements(0, s64, 2)
      .lower();

  getActionDefinitionsBuilder(G_VASTART).legalFor({p0});

  // va_list must be a pointer, but most sized types are pretty easy to handle
  // as the destination.
  getActionDefinitionsBuilder(G_VAARG)
      .customForCartesianProduct({s8, s16, s32, s64, p0}, {p0})
      .clampScalar(0, s8, s64)
      .widenScalarToNextPow2(0, /*Min*/ 8);

  getActionDefinitionsBuilder(G_ATOMIC_CMPXCHG_WITH_SUCCESS)
      .lowerIf(all(typeInSet(0, {s8, s16, s32, s64, s128}), typeIs(2, p0)));

  bool UseOutlineAtomics = ST.outlineAtomics() && !ST.hasLSE();

  getActionDefinitionsBuilder(G_ATOMIC_CMPXCHG)
      .legalFor(!UseOutlineAtomics, {{s32, p0}, {s64, p0}})
      .customFor(!UseOutlineAtomics, {{s128, p0}})
      .libcallFor(UseOutlineAtomics,
                  {{s8, p0}, {s16, p0}, {s32, p0}, {s64, p0}, {s128, p0}})
      .clampScalar(0, s32, s64);

  getActionDefinitionsBuilder({G_ATOMICRMW_XCHG, G_ATOMICRMW_ADD,
                               G_ATOMICRMW_SUB, G_ATOMICRMW_AND, G_ATOMICRMW_OR,
                               G_ATOMICRMW_XOR})
      .legalFor(!UseOutlineAtomics, {{s32, p0}, {s64, p0}})
      .libcallFor(UseOutlineAtomics,
                  {{s8, p0}, {s16, p0}, {s32, p0}, {s64, p0}})
      .clampScalar(0, s32, s64);

  // Do not outline these atomics operations, as per comment in
  // AArch64ISelLowering.cpp's shouldExpandAtomicRMWInIR().
  getActionDefinitionsBuilder(
      {G_ATOMICRMW_MIN, G_ATOMICRMW_MAX, G_ATOMICRMW_UMIN, G_ATOMICRMW_UMAX})
      .legalIf(all(typeInSet(0, {s32, s64}), typeIs(1, p0)))
      .clampScalar(0, s32, s64);

  getActionDefinitionsBuilder(G_BLOCK_ADDR).legalFor({p0});

  // Merge/Unmerge
  for (unsigned Op : {G_MERGE_VALUES, G_UNMERGE_VALUES}) {
    unsigned BigTyIdx = Op == G_MERGE_VALUES ? 0 : 1;
    unsigned LitTyIdx = Op == G_MERGE_VALUES ? 1 : 0;
    getActionDefinitionsBuilder(Op)
        .widenScalarToNextPow2(LitTyIdx, 8)
        // Above s64 lowered shifts narrow back to a merge and never terminate
        .lowerIf([=](const LegalityQuery &Q) {
          const LLT BigTy = Q.Types[BigTyIdx];
          return BigTy.isScalar() && !isPowerOf2_32(BigTy.getSizeInBits()) &&
                 BigTy.getSizeInBits() < 64;
        })
        .widenScalarToNextPow2(BigTyIdx, 32)
        .clampScalar(LitTyIdx, s8, s64)
        .clampScalar(BigTyIdx, s32, s128)
        .legalIf([=](const LegalityQuery &Q) {
          switch (Q.Types[BigTyIdx].getSizeInBits()) {
          case 32:
          case 64:
          case 128:
            break;
          default:
            return false;
          }
          switch (Q.Types[LitTyIdx].getSizeInBits()) {
          case 8:
          case 16:
          case 32:
          case 64:
            return true;
          default:
            return false;
          }
        });
  }

  // TODO : nxv4s16, nxv2s16, nxv2s32
  getActionDefinitionsBuilder(G_EXTRACT_VECTOR_ELT)
      .legalFor(HasSVE, {{s16, nxv16s8, s64},
                         {s16, nxv8s16, s64},
                         {s32, nxv4s32, s64},
                         {s64, nxv2s64, s64}})
      .unsupportedIf([=](const LegalityQuery &Query) {
        const LLT &EltTy = Query.Types[1].getElementType();
        if (Query.Types[1].isScalableVector())
          return false;
        return Query.Types[0] != EltTy;
      })
      .minScalar(2, s64)
      .customIf([=](const LegalityQuery &Query) {
        const LLT &VecTy = Query.Types[1];
        return VecTy == v8s8 || VecTy == v16s8 || VecTy == v2s16 ||
               VecTy == v4s16 || VecTy == v8s16 || VecTy == v2s32 ||
               VecTy == v4s32 || VecTy == v2s64 || VecTy == v2p0;
      })
      .minScalarOrEltIf(
          [=](const LegalityQuery &Query) {
            // We want to promote to <M x s1> to <M x s64> if that wouldn't
            // cause the total vec size to be > 128b.
            return Query.Types[1].isFixedVector() &&
                   Query.Types[1].getNumElements() <= 2;
          },
          0, s64)
      .minScalarOrEltIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[1].isFixedVector() &&
                   Query.Types[1].getNumElements() <= 4;
          },
          0, s32)
      .minScalarOrEltIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[1].isFixedVector() &&
                   Query.Types[1].getNumElements() <= 8;
          },
          0, s16)
      .minScalarOrEltIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[1].isFixedVector() &&
                   Query.Types[1].getNumElements() <= 16;
          },
          0, s8)
      .minScalarOrElt(0, s8) // Worst case, we need at least s8.
      .moreElementsToNextPow2(1)
      .clampMaxNumElements(1, s64, 2)
      .clampMaxNumElements(1, s32, 4)
      .clampMaxNumElements(1, s16, 8)
      .clampMaxNumElements(1, s8, 16)
      .clampMaxNumElements(1, p0, 2)
      .scalarizeIf(scalarOrEltWiderThan(1, 64), 1);

  getActionDefinitionsBuilder(G_INSERT_VECTOR_ELT)
      .legalIf(
          typeInSet(0, {v8s8, v16s8, v4s16, v8s16, v2s32, v4s32, v2s64, v2p0}))
      .legalFor(HasSVE, {{nxv16s8, s32, s64},
                         {nxv8s16, s32, s64},
                         {nxv4s32, s32, s64},
                         {nxv2s64, s64, s64}})
      .moreElementsToNextPow2(0)
      .widenVectorEltsToVectorMinSize(0, 64)
      .clampNumElements(0, v8s8, v16s8)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v2s32, v4s32)
      .clampMaxNumElements(0, s64, 2)
      .clampMaxNumElements(0, p0, 2)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0);

  getActionDefinitionsBuilder(G_BUILD_VECTOR)
      .legalFor({{v8s8, s8},
                 {v16s8, s8},
                 {v4s16, s16},
                 {v8s16, s16},
                 {v2s32, s32},
                 {v4s32, s32},
                 {v2s64, s64},
                 {v2p0, p0}})
      .clampNumElements(0, v4s32, v4s32)
      .clampNumElements(0, v2s64, v2s64)
      .minScalarOrElt(0, s8)
      .widenVectorEltsToVectorMinSize(0, 64)
      .widenScalarOrEltToNextPow2(0)
      .minScalarSameAs(1, 0);

  getActionDefinitionsBuilder(G_BUILD_VECTOR_TRUNC).lower();

  getActionDefinitionsBuilder(G_SPLAT_VECTOR)
      .legalFor({{v8s8, s8},
                 {v8s8, s32},
                 {v16s8, s8},
                 {v16s8, s32},
                 {v4s16, s16},
                 {v4s16, s32},
                 {v8s16, s16},
                 {v8s16, s32},
                 {v2s32, s32},
                 {v4s32, s32},
                 {v2s64, s64},
                 {v2p0, p0}});

  getActionDefinitionsBuilder(G_SHUFFLE_VECTOR)
      .legalIf([=](const LegalityQuery &Query) {
        const LLT &DstTy = Query.Types[0];
        const LLT &SrcTy = Query.Types[1];
        // For now just support the TBL2 variant which needs the source vectors
        // to be the same size as the dest.
        if (DstTy != SrcTy)
          return false;
        return llvm::is_contained(
            {v8s8, v16s8, v4s16, v8s16, v2s32, v4s32, v2s64}, DstTy);
      })
      .moreElementsIf(
          [](const LegalityQuery &Query) {
            return Query.Types[0].getNumElements() >
                   Query.Types[1].getNumElements();
          },
          changeTo(1, 0))
      .moreElementsToNextPow2(0)
      .moreElementsIf(
          [](const LegalityQuery &Query) {
            return Query.Types[0].getNumElements() <
                   Query.Types[1].getNumElements();
          },
          changeTo(0, 1))
      .widenScalarOrEltToNextPow2OrMinSize(0, 8)
      .clampNumElements(0, v8s8, v16s8)
      .clampNumElements(0, v4s16, v8s16)
      .clampNumElements(0, v4s32, v4s32)
      .clampNumElements(0, v2s64, v2s64)
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0)
      .bitcastIf(isPointerVector(0), [=](const LegalityQuery &Query) {
        // Bitcast pointers vector to i64.
        const LLT DstTy = Query.Types[0];
        return std::pair(
            0, LLT::vector(DstTy.getElementCount(), LLT::integer(64)));
      });

  getActionDefinitionsBuilder(G_CONCAT_VECTORS)
      .legalFor({{v16s8, v8s8}, {v8s16, v4s16}, {v4s32, v2s32}})
      .customIf([=](const LegalityQuery &Query) {
        return Query.Types[0].isFixedVector() &&
               Query.Types[0].getScalarSizeInBits() < 8;
      })
      .bitcastIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[0].isFixedVector() &&
                   Query.Types[1].isFixedVector() &&
                   Query.Types[0].getScalarSizeInBits() >= 8 &&
                   isPowerOf2_64(Query.Types[0].getScalarSizeInBits()) &&
                   Query.Types[0].getSizeInBits() <= 128 &&
                   Query.Types[1].getSizeInBits() <= 64;
          },
          [=](const LegalityQuery &Query) {
            const LLT DstTy = Query.Types[0];
            const LLT SrcTy = Query.Types[1];
            return std::pair(
                0, DstTy.changeElementSize(SrcTy.getSizeInBits())
                       .changeElementCount(
                           DstTy.getElementCount().divideCoefficientBy(
                               SrcTy.getNumElements())));
          });

  getActionDefinitionsBuilder(G_EXTRACT_SUBVECTOR)
      .legalFor({{v8s8, v16s8}, {v4s16, v8s16}, {v2s32, v4s32}})
      .widenScalarOrEltToNextPow2(0)
      .clampMaxNumElements(0, s8, 16)
      .clampMaxNumElements(0, s16, 8)
      .clampMaxNumElements(0, s32, 4)
      .clampNumElements(1, v8s8, v16s8)
      .clampNumElements(1, v4s16, v8s16)
      .clampNumElements(1, v2s32, v4s32)
      .lower()
      .immIdx(0); // Inform verifier imm idx 0 is handled.

  // TODO: {nxv16s8, s8}, {nxv8s16, s16}
  getActionDefinitionsBuilder(G_SPLAT_VECTOR)
      .legalFor(HasSVE, {{nxv4s32, s32}, {nxv2s64, s64}});

  getActionDefinitionsBuilder(G_JUMP_TABLE).legalFor({p0});

  getActionDefinitionsBuilder(G_BRJT).legalFor({{p0, s64}});

  getActionDefinitionsBuilder({G_TRAP, G_DEBUGTRAP, G_UBSANTRAP}).alwaysLegal();

  getActionDefinitionsBuilder(G_DYN_STACKALLOC).custom();

  getActionDefinitionsBuilder({G_STACKSAVE, G_STACKRESTORE}).lower();

  if (ST.hasMOPS()) {
    // G_BZERO is not supported. Currently it is only emitted by
    // PreLegalizerCombiner for G_MEMSET with zero constant.
    getActionDefinitionsBuilder(G_BZERO).unsupported();

    getActionDefinitionsBuilder(G_MEMSET)
        .legalForCartesianProduct({p0}, {s64}, {s64})
        .customForCartesianProduct({p0}, {s8}, {s64})
        .immIdx(0); // Inform verifier imm idx 0 is handled.

    getActionDefinitionsBuilder({G_MEMCPY, G_MEMMOVE})
        .legalForCartesianProduct({p0}, {p0}, {s64})
        .immIdx(0); // Inform verifier imm idx 0 is handled.

    // G_MEMCPY_INLINE does not have a tailcall immediate
    getActionDefinitionsBuilder(G_MEMCPY_INLINE)
        .legalForCartesianProduct({p0}, {p0}, {s64});

    getActionDefinitionsBuilder(G_MEMSET_INLINE)
        .legalForCartesianProduct({p0}, {s64}, {s64})
        .customForCartesianProduct({p0}, {s8}, {s64});
  } else {
    getActionDefinitionsBuilder({G_BZERO, G_MEMCPY, G_MEMMOVE, G_MEMSET})
        .libcall();
  }

  // For fadd reductions we have pairwise operations available. We treat the
  // usual legal types as legal and handle the lowering to pairwise instructions
  // later.
  getActionDefinitionsBuilder(G_VECREDUCE_FADD)
      .legalFor({{f32, v2f32}, {f32, v4f32}, {f64, v2f64}})
      .legalFor(HasFP16, {{f16, v4f16}, {f16, v8f16}})
      .widenScalarIf(
          [HasFP16](const LegalityQuery &Query) {
            return (!HasFP16 && Query.Types[0].getScalarType().isFloat16()) ||
                   Query.Types[0].getScalarType().isBFloat16();
          },
          changeElementTo(0, f32))
      .clampMaxNumElements(1, s64, 2)
      .clampMaxNumElements(1, s32, 4)
      .clampMaxNumElements(1, s16, 8)
      .moreElementsToNextPow2(1)
      .scalarize(1)
      .lower();

  // For fmul reductions we need to split up into individual operations. We
  // clamp to 128 bit vectors then to 64bit vectors to produce a cascade of
  // smaller types, followed by scalarizing what remains.
  getActionDefinitionsBuilder(G_VECREDUCE_FMUL)
      .widenScalarIf(
          [HasFP16](const LegalityQuery &Query) {
            return (!HasFP16 && Query.Types[0].getScalarType().isFloat16()) ||
                   Query.Types[0].getScalarType().isBFloat16();
          },
          changeElementTo(0, f32))
      .clampMaxNumElements(1, s64, 2)
      .clampMaxNumElements(1, s32, 4)
      .clampMaxNumElements(1, s16, 8)
      .clampMaxNumElements(1, s32, 2)
      .clampMaxNumElements(1, s16, 4)
      .scalarize(1)
      .lower();

  getActionDefinitionsBuilder({G_VECREDUCE_SEQ_FADD, G_VECREDUCE_SEQ_FMUL})
      .scalarize(2)
      .lower();

  getActionDefinitionsBuilder(G_VECREDUCE_ADD)
      .legalFor({{i8, v8i8},
                 {i8, v16i8},
                 {i16, v4i16},
                 {i16, v8i16},
                 {i32, v2i32},
                 {i32, v4i32},
                 {i64, v2i64}})
      .moreElementsToNextPow2(1)
      .clampMaxNumElements(1, s64, 2)
      .clampMaxNumElements(1, s32, 4)
      .clampMaxNumElements(1, s16, 8)
      .clampMaxNumElements(1, s8, 16)
      .widenVectorEltsToVectorMinSize(1, 64)
      .scalarize(1);

  getActionDefinitionsBuilder({G_VECREDUCE_FMIN, G_VECREDUCE_FMAX,
                               G_VECREDUCE_FMINIMUM, G_VECREDUCE_FMAXIMUM})
      .legalFor({{f32, v2f32}, {f32, v4f32}, {f64, v2f64}})
      .legalFor(HasFP16, {{f16, v4f16}, {f16, v8f16}})
      .widenScalarIf(
          [HasFP16](const LegalityQuery &Query) {
            return (!HasFP16 && Query.Types[0].getScalarType().isFloat16()) ||
                   Query.Types[0].getScalarType().isBFloat16();
          },
          changeElementTo(0, f32))
      .clampMaxNumElements(1, s64, 2)
      .clampMaxNumElements(1, s32, 4)
      .clampMaxNumElements(1, s16, 8)
      .scalarize(1)
      .lower();

  getActionDefinitionsBuilder(G_VECREDUCE_MUL)
      .clampMaxNumElements(1, s32, 2)
      .clampMaxNumElements(1, s16, 4)
      .clampMaxNumElements(1, s8, 8)
      .scalarize(1)
      .lower();

  getActionDefinitionsBuilder(
      {G_VECREDUCE_SMIN, G_VECREDUCE_SMAX, G_VECREDUCE_UMIN, G_VECREDUCE_UMAX})
      .legalFor({{i8, v8i8},
                 {i8, v16i8},
                 {i16, v4i16},
                 {i16, v8i16},
                 {i32, v2i32},
                 {i32, v4i32}})
      .moreElementsIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[1].isVector() &&
                   Query.Types[1].getElementType() != s8 &&
                   Query.Types[1].getNumElements() & 1;
          },
          LegalizeMutations::moreElementsToNextPow2(1))
      .clampMaxNumElements(1, s64, 2)
      .clampMaxNumElements(1, s32, 4)
      .clampMaxNumElements(1, s16, 8)
      .clampMaxNumElements(1, s8, 16)
      .scalarize(1)
      .lower();

  getActionDefinitionsBuilder(
      {G_VECREDUCE_OR, G_VECREDUCE_AND, G_VECREDUCE_XOR})
      // Try to break down into smaller vectors as long as they're at least 64
      // bits. This lets us use vector operations for some parts of the
      // reduction.
      .fewerElementsIf(
          [=](const LegalityQuery &Q) {
            LLT SrcTy = Q.Types[1];
            if (SrcTy.isScalar())
              return false;
            if (!isPowerOf2_32(SrcTy.getNumElements()))
              return false;
            // We can usually perform 64b vector operations.
            return SrcTy.getSizeInBits() > 64;
          },
          [=](const LegalityQuery &Q) {
            LLT SrcTy = Q.Types[1];
            return std::make_pair(1, SrcTy.divide(2));
          })
      .scalarize(1)
      .lower();

  // TODO: Update this to correct handling when adding AArch64/SVE support.
  getActionDefinitionsBuilder(G_VECTOR_COMPRESS).lower();

  // Access to floating-point environment.
  getActionDefinitionsBuilder({G_GET_FPENV, G_SET_FPENV, G_RESET_FPENV,
                               G_GET_FPMODE, G_SET_FPMODE, G_RESET_FPMODE})
      .libcall();

  getActionDefinitionsBuilder({G_GET_ROUNDING, G_SET_ROUNDING})
      .customFor({s32});

  getActionDefinitionsBuilder(G_IS_FPCLASS).lower();

  getActionDefinitionsBuilder(G_PREFETCH).custom();

  getActionDefinitionsBuilder({G_SCMP, G_UCMP}).lower();

  getActionDefinitionsBuilder({G_INTRINSIC, G_INTRINSIC_W_SIDE_EFFECTS})
      .alwaysLegal();
  getActionDefinitionsBuilder(G_FENCE).alwaysLegal();
  getActionDefinitionsBuilder(G_INVOKE_REGION_START).alwaysLegal();

  verify(*ST.getInstrInfo());
}
