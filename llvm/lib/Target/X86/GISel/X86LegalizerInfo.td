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
def HasPOPCNT : LegalizeFeature<"Subtarget.hasPOPCNT()">;
def POPCNTAnd64Bit : LegalizePredicateDag<(all_of HasPOPCNT, Is64Bit)>;
def X86_s16 : LegalizeNamedType<"LLT::scalar(16)">;
def X86_s32 : LegalizeNamedType<"LLT::scalar(32)">;
def X86_s64 : LegalizeNamedType<"LLT::scalar(64)">;
def X86_sMaxScalar : LegalizeNamedType<"Subtarget.is64Bit() ? X86_s64 : X86_s32">;

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

