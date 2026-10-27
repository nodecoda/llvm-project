//===- X86LegalizerInfo.td - X86 GlobalISel legalizer -*-- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POC: declarative GlobalISel legalizer rules for X86.
//
// Only the opcodes that have been migrated are described here.  Everything
// else still lives in X86LegalizerInfo.cpp; the two must not describe the
// same opcode, and -gen-legalizer enforces that within this file.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/LegalizerInfo.td"

def X86_CTPOP : LegalizeRuleSet<["G_CTPOP"]> {
  let Rules = [
    LG_LegalFor<LGFeature<"HasPOPCNT">, ["s16, s16", "s32, s32"]>,
    LG_LegalFor<LGAnd<LGFeature<"HasPOPCNT">, LGFeature<"Is64Bit">>,
                ["s64, s64"]>,
    LG_WidenScalarToNextPow2<"1", "16">,
    LG_ClampScalar<"1", "s16", "sMaxScalar">,
    LG_ScalarSameSizeAs<"0", "1">,
  ];
}