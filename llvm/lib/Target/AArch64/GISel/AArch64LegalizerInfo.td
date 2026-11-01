//===-- AArch64LegalizerInfo.td ----------------------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// AArch64 legalizer rules, for the key partition.
//
// Unlike X86LegalizerInfo.td this file is NOT the legalizer: the chain stays in
// AArch64LegalizerInfo.cpp, and this file is a second, independent statement of
// the same rule sets.  -gen-legalizer-table reads it for the key partition and
// the domain; the answers are still filled from the C++ chain.  That makes the
// chain the oracle: if the two disagree anywhere the table can key, the
// self-check says so, and the disagreement is a fact about the .td, not about
// the C++.
//
// The slice is the bit-manipulation family, because issue 220802 is about
// G_CTPOP on vectors and this is the corner of AArch64 that answers it.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/LegalizerInfo.td"

/// The LLT constants the rules are written in terms of.  Named the same way
/// X86 names its own (X86_s32), so the two files read alike.
def AArch64_s32 : LegalizeNamedType<"LLT::scalar(32)">;
def AArch64_s64 : LegalizeNamedType<"LLT::scalar(64)">;
def AArch64_s128 : LegalizeNamedType<"LLT::scalar(128)">;
def AArch64_p0 : LegalizeNamedType<"LLT::pointer(0, 64)">;

def AArch64_v8s8 : LegalizeNamedType<"LLT::fixed_vector(8, 8)">;
def AArch64_v16s8 : LegalizeNamedType<"LLT::fixed_vector(16, 8)">;
def AArch64_v4s16 : LegalizeNamedType<"LLT::fixed_vector(4, 16)">;
def AArch64_v8s16 : LegalizeNamedType<"LLT::fixed_vector(8, 16)">;
def AArch64_v2s32 : LegalizeNamedType<"LLT::fixed_vector(2, 32)">;
def AArch64_v4s32 : LegalizeNamedType<"LLT::fixed_vector(4, 32)">;
def AArch64_v2s64 : LegalizeNamedType<"LLT::fixed_vector(2, 64)">;

/// Subtarget-feature booleans, as in the X86 file: the record name is the C++
/// identifier the generated table declares.
def HasCSSC : LegalizeFeature<"Subtarget.hasCSSC()">;
def NotCSSC : LegalizePredicateDag<(not HasCSSC)>;

/// Query-time guards.  The C++ writes `always` as a lambda; the .td has to name
/// it, because a rule's guard is a record here, not a C++ expression inline.
def IsVector0 : LegalizeTypePredicate<"isVector(0)">;
def AlwaysTrue : LegalizeAlwaysTrue<"[](const LegalityQuery &) { return true; }">;
def EltWider64 : LegalizeTypePredicate<"scalarOrEltWiderThan(0, 64)">;
def EltWider32 : LegalizeTypePredicate<"scalarOrEltWiderThan(0, 32)">;

def AArch64_ROTL : LegalizeRuleSet<["G_ROTL"]> {
  let Comments = ["rotate left: no instruction, lower it"];
  let Rules = [Lower];
}

def AArch64_SBFX_UBFX : LegalizeRuleSet<["G_SBFX", "G_UBFX"]> {
  let Comments = ["signed/unsigned bitfield extract"];
  let Rules = [
    CustomForPairs<[LegalizeTypePair<AArch64_s32, AArch64_s32>,
                    LegalizeTypePair<AArch64_s64, AArch64_s64>]>,
  ];
}

def AArch64_CTPOP : LegalizeRuleSet<["G_CTPOP"]> {
  let Comments = ["popcount: CSSC does it in one instruction"];
  let Rules = [
    LegalForPairsIf<HasCSSC, [LegalizeTypePair<AArch64_s32, AArch64_s32>,
                              LegalizeTypePair<AArch64_s64, AArch64_s64>]>,
    LegalForPairs<[LegalizeTypePair<AArch64_v8s8, AArch64_v8s8>,
                   LegalizeTypePair<AArch64_v16s8, AArch64_v16s8>]>,
    CustomForPairsIf<NotCSSC, [LegalizeTypePair<AArch64_s32, AArch64_s32>,
                               LegalizeTypePair<AArch64_s64, AArch64_s64>]>,
    CustomForPairs<[LegalizeTypePair<AArch64_s128, AArch64_s128>,
                    LegalizeTypePair<AArch64_v4s16, AArch64_v4s16>,
                    LegalizeTypePair<AArch64_v8s16, AArch64_v8s16>,
                    LegalizeTypePair<AArch64_v2s32, AArch64_v2s32>,
                    LegalizeTypePair<AArch64_v4s32, AArch64_v4s32>,
                    LegalizeTypePair<AArch64_v2s64, AArch64_v2s64>]>,
    ClampScalar<0, AArch64_s32, AArch64_s128>,
    WidenScalarToNextPow2<0>,
    WidenScalarOrEltToNextPow2OrMinSize<0, 8>,
    MinScalarEltSameAsIf<AlwaysTrue, 1, 0>,
    MaxScalarEltSameAsIf<AlwaysTrue, 1, 0>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    ClampNumElements<0, AArch64_v2s64, AArch64_v2s64>,
    MoreElementsToNextPow2<0>,
    ScalarizeIf<EltWider64, 0>,
  ];
}

def AArch64_CTLZ : LegalizeRuleSet<["G_CTLZ", "G_CTLS"]> {
  let Comments = ["count leading zeros / leading sign bits"];
  let Rules = [
    LegalForPairs<[LegalizeTypePair<AArch64_s32, AArch64_s32>,
                   LegalizeTypePair<AArch64_s64, AArch64_s64>,
                   LegalizeTypePair<AArch64_v8s8, AArch64_v8s8>,
                   LegalizeTypePair<AArch64_v16s8, AArch64_v16s8>,
                   LegalizeTypePair<AArch64_v4s16, AArch64_v4s16>,
                   LegalizeTypePair<AArch64_v8s16, AArch64_v8s16>,
                   LegalizeTypePair<AArch64_v2s32, AArch64_v2s32>,
                   LegalizeTypePair<AArch64_v4s32, AArch64_v4s32>]>,
    WidenScalarToNextPow2<1, 32>,
    ClampScalar<1, AArch64_s32, AArch64_s64>,
    WidenScalarOrEltToNextPow2OrMinSize<1, 8>,
    ClampNumElements<0, AArch64_v8s8, AArch64_v16s8>,
    ClampNumElements<0, AArch64_v4s16, AArch64_v8s16>,
    ClampNumElements<0, AArch64_v2s32, AArch64_v4s32>,
    MoreElementsToNextPow2<0>,
    ScalarizeIf<EltWider32, 0>,
    ScalarSameSizeAs<0, 1>,
  ];
}

def AArch64_CTLZ_ZERO_POISON : LegalizeRuleSet<["G_CTLZ_ZERO_POISON"]> {
  let Comments = ["zero-poison form: lower it"];
  let Rules = [Lower];
}

def AArch64_CTTZ : LegalizeRuleSet<["G_CTTZ"]> {
  let Comments = ["count trailing zeros"];
  let Rules = [
    LowerIf<IsVector0>,
    WidenScalarToNextPow2<1, 32>,
    ClampScalar<1, AArch64_s32, AArch64_s64>,
    ScalarSameSizeAs<0, 1>,
    LegalForIf<HasCSSC, [AArch64_s32, AArch64_s64]>,
    CustomForIf<NotCSSC, [AArch64_s32, AArch64_s64]>,
  ];
}

def AArch64_CTTZ_ZERO_POISON : LegalizeRuleSet<["G_CTTZ_ZERO_POISON"]> {
  let Comments = ["zero-poison form: lower it"];
  let Rules = [Lower];
}