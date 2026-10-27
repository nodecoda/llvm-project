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
// Mechanically converted from the former hand-written chains in
// X86LegalizerInfo.cpp, then reviewed by hand.  Every record below corresponds
// to one getActionDefinitionsBuilder() call; the generated C++ is identical up
// to whitespace and redundant parentheses.
//
// X86_Context declares the subtarget-feature booleans and LLT constants the
// rules are written in terms of, so this file is the whole legalizer: there is
// no longer a hand-written prologue in the constructor.
//
// LG_OpaqueRule is the escape hatch for guards that are still raw C++
// lambdas: 10 rules out of 239.
//
// Four `for (Op : {...})` loops in the former C++ were hand-flattened into
// one record per opcode; the loop variables therefore appear as literals.
// `if (Cond) Action.foo(...)` became `LG_...If<LGFeature<"Cond">, ...>`:
// the rule is always installed, but its guard is constant for a given
// subtarget, so a false guard is equivalent to the rule being absent.
//

include "llvm/Target/LegalizerInfo.td"

/// The subtarget-feature booleans and LLT constants the rules are written in
/// terms of.  Order matters: a derived entry may use the ones above it.
def X86_Context : LegalizeContext {
  let Comments = ["todo: AVX512 bool vector predicate types"];
  let Bools = [
    LG_Bool<"Is64Bit", "Subtarget.is64Bit()">,
    LG_Bool<"HasCMOV", "Subtarget.canUseCMOV()">,
    LG_Bool<"HasSSE1", "Subtarget.hasSSE1()">,
    LG_Bool<"HasSSE2", "Subtarget.hasSSE2()">,
    LG_Bool<"HasSSE41", "Subtarget.hasSSE41()">,
    LG_Bool<"HasAVX", "Subtarget.hasAVX()">,
    LG_Bool<"HasAVX2", "Subtarget.hasAVX2()">,
    LG_Bool<"HasAVX512", "Subtarget.hasAVX512()">,
    LG_Bool<"HasVLX", "Subtarget.hasVLX()">,
    LG_Bool<"HasDQI", "Subtarget.hasAVX512() && Subtarget.hasDQI()">,
    LG_Bool<"HasBWI", "Subtarget.hasAVX512() && Subtarget.hasBWI()">,
    LG_Bool<"UseX87", "!Subtarget.useSoftFloat() && Subtarget.hasX87()">,
    LG_Bool<"HasPOPCNT", "Subtarget.hasPOPCNT()">,
    LG_Bool<"HasLZCNT", "Subtarget.hasLZCNT()">,
    LG_Bool<"HasBMI", "Subtarget.hasBMI()">,
  ];
  let Types = [
    LG_Type<"p0",
            "LLT::pointer(0, Subtarget.isTarget64BitLP64() ? 64 : 32)">,
    LG_Type<"s1", "LLT::scalar(1)">,
    LG_Type<"s8", "LLT::scalar(8)">,
    LG_Type<"s16", "LLT::scalar(16)">,
    LG_Type<"s32", "LLT::scalar(32)">,
    LG_Type<"s64", "LLT::scalar(64)">,
    LG_Type<"s80", "LLT::scalar(80)">,
    LG_Type<"s128", "LLT::scalar(128)">,
    LG_Type<"sMaxScalar", "Subtarget.is64Bit() ? s64 : s32">,

    LG_Type<"v2s32", "LLT::fixed_vector(2, 32)">,
    LG_Type<"v4s8", "LLT::fixed_vector(4, 8)">,

    LG_Type<"v16s8", "LLT::fixed_vector(16, 8)">,
    LG_Type<"v8s16", "LLT::fixed_vector(8, 16)">,
    LG_Type<"v4s32", "LLT::fixed_vector(4, 32)">,
    LG_Type<"v2s64", "LLT::fixed_vector(2, 64)">,
    LG_Type<"v2p0", "LLT::fixed_vector(2, p0)">,

    LG_Type<"v32s8", "LLT::fixed_vector(32, 8)">,
    LG_Type<"v16s16", "LLT::fixed_vector(16, 16)">,
    LG_Type<"v8s32", "LLT::fixed_vector(8, 32)">,
    LG_Type<"v4s64", "LLT::fixed_vector(4, 64)">,
    LG_Type<"v4p0", "LLT::fixed_vector(4, p0)">,

    LG_Type<"v64s8", "LLT::fixed_vector(64, 8)">,
    LG_Type<"v32s16", "LLT::fixed_vector(32, 16)">,
    LG_Type<"v16s32", "LLT::fixed_vector(16, 32)">,
    LG_Type<"v8s64", "LLT::fixed_vector(8, 64)">,

    LG_Type<"s8MaxVector", "HasAVX512 ? v64s8 : HasAVX ? v32s8 : v16s8">,
    LG_Type<"s16MaxVector", "HasAVX512 ? v32s16 : HasAVX ? v16s16 : v8s16">,
    LG_Type<"s32MaxVector", "HasAVX512 ? v16s32 : HasAVX ? v8s32 : v4s32">,
    LG_Type<"s64MaxVector", "HasAVX512 ? v8s64 : HasAVX ? v4s64 : v2s64">,
  ];
  // Types used by individual rule sets.
  let TypeLists = [
    LG_TypeList<"IntTypes32", ["s8", "s16", "s32", "p0"]>,
    LG_TypeList<"IntTypes64", ["s8", "s16", "s32", "s64", "p0"]>,
    LG_TypeList<"PtrTypes32", ["s1", "s8", "s16", "s32"]>,
    LG_TypeList<"PtrTypes64", ["s1", "s8", "s16", "s32", "s64"]>,
  ];
}


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
    LG_LegalForTypes<["p0", "s1", "s8", "s16", "s32", "s64"], 0>,
    LG_LegalFor<LGFeature<"UseX87">, ["s80"], 0>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s128"], 0>,
    LG_LegalFor<LGFeature<"HasSSE2">, ["v16s8", "v8s16", "v4s32", "v2s64"], 0>,
    LG_LegalFor<LGFeature<"HasAVX">, ["v32s8", "v16s16", "v8s32", "v4s64"], 0>,
    LG_LegalFor<LGFeature<"HasAVX512">, ["v64s8", "v32s16", "v16s32", "v8s64"],
                0>,
    LG_WidenScalarOrEltToNextPow2<"0", "8">,
    LG_ClampScalarOrElt<"0", "s8", "sMaxScalar">,
    LG_MoreElementsToNextPow2<"0">,
    LG_ClampNumElements<"0", "v16s8", "s8MaxVector">,
    LG_ClampNumElements<"0", "v8s16", "s16MaxVector">,
    LG_ClampNumElements<"0", "v4s32", "s32MaxVector">,
    LG_ClampNumElements<"0", "v2s64", "s64MaxVector">,
    LG_ClampMaxNumElements<"0", "p0",
                           "Is64Bit ? s64MaxVector.getNumElements() : s32MaxVector.getNumElements()">,
    LG_ScalarizeIf<LGExpr<"scalarOrEltWiderThan(0, 64)">, "0">,
  ];
}

def X86_CONSTANT : LegalizeRuleSet<["G_CONSTANT"]> {
  let Rules = [
    LG_LegalForTypes<["p0", "s8", "s16", "s32"], 0>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s64"], 0>,
    LG_WidenScalarToNextPow2<"0", "8">,
    LG_ClampScalar<"0", "s8", "sMaxScalar">,
  ];
}

def X86_LROUND : LegalizeRuleSet<["G_LROUND", "G_LLROUND"]> {
  let Rules = [
    LG_OpaqueRule<"widenScalarIf", [{

      typeIs(1, s16),
      [=](const LegalityQuery &) {
        return std::pair<unsigned, LLT>(1, s32);
      }
    }]>,
    LG_Libcall,
  ];
}

def X86_FCOS : LegalizeRuleSet<[
  "G_FCOS", "G_FCOSH", "G_FACOS", "G_FSIN", "G_FSINH", "G_FASIN", "G_FTAN",
  "G_FTANH", "G_FATAN", "G_FATAN2", "G_FPOW", "G_FEXP", "G_FEXP2", "G_FEXP10",
  "G_FLOG", "G_FLOG2", "G_FLOG10", "G_FPOWI", "G_FSINCOS", "G_FCEIL", "G_FFLOOR"
]> {
  let Rules = [
    LG_Libcall,
  ];
}

def X86_FNEG : LegalizeRuleSet<["G_FNEG"]> {
  let Rules = [
    LG_LegalFor<LGExpr<"UseX87 && !HasSSE1">, ["s32"], 0>,
    LG_LegalFor<LGExpr<"UseX87 && !HasSSE2">, ["s64"], 0>,
    LG_LegalFor<LGFeature<"UseX87">, ["s80"], 0>,
    LG_Lower,
  ];
}

def X86_FSQRT : LegalizeRuleSet<["G_FSQRT"]> {
  let Rules = [
    LG_LegalFor<LGExpr<"HasSSE1 || UseX87">, ["s32"], 0>,
    LG_LegalFor<LGExpr<"HasSSE2 || UseX87">, ["s64"], 0>,
    LG_LegalFor<LGFeature<"UseX87">, ["s80"], 0>,
  ];
}

def X86_GET_ROUNDING : LegalizeRuleSet<["G_GET_ROUNDING", "G_SET_ROUNDING"]> {
  let Rules = [
    LG_CustomFor<["s32"], 0>,
  ];
}

def X86_MERGE_VALUES : LegalizeRuleSet<["G_MERGE_VALUES"]> {
  let Rules = [
    LG_WidenScalarToNextPow2<"1", "8">,
    LG_WidenScalarToNextPow2<"0", "16">,
    LG_MinScalar<"1", "s8">,
    LG_MinScalar<"0", "s32">,
    LG_OpaqueRule<"legalIf", [{

      [=](const LegalityQuery &Q) {
        switch (Q.Types[0].getSizeInBits()) {
        case 16:
        case 32:
        case 64:
        case 128:
        case 256:
        case 512:
          break;
        default:
          return false;
        }
        switch (Q.Types[1].getSizeInBits()) {
        case 8:
        case 16:
        case 32:
        case 64:
        case 128:
        case 256:
          return true;
        default:
          return false;
        }
      }
    }]>,
  ];
}

def X86_UNMERGE_VALUES : LegalizeRuleSet<["G_UNMERGE_VALUES"]> {
  let Rules = [
    LG_WidenScalarToNextPow2<"0", "8">,
    LG_WidenScalarToNextPow2<"1", "16">,
    LG_MinScalar<"0", "s8">,
    LG_MinScalar<"1", "s32">,
    LG_OpaqueRule<"legalIf", [{

      [=](const LegalityQuery &Q) {
        switch (Q.Types[1].getSizeInBits()) {
        case 16:
        case 32:
        case 64:
        case 128:
        case 256:
        case 512:
          break;
        default:
          return false;
        }
        switch (Q.Types[0].getSizeInBits()) {
        case 8:
        case 16:
        case 32:
        case 64:
        case 128:
        case 256:
          return true;
        default:
          return false;
        }
      }
    }]>,
  ];
}

def X86_UMIN : LegalizeRuleSet<["G_UMIN", "G_UMAX", "G_SMIN", "G_SMAX"]> {
  let Rules = [
    LG_WidenScalarToNextPow2<"0", "32">,
    LG_Lower,
  ];
}

def X86_ADD : LegalizeRuleSet<["G_ADD", "G_SUB"]> {
  let Comments = ["integer addition/subtraction"];
  let Rules = [
    LG_LegalForTypes<["s8", "s16", "s32"], 0>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s64"], 0>,
    LG_LegalFor<LGFeature<"HasSSE2">, ["v16s8", "v8s16", "v4s32", "v2s64"], 0>,
    LG_LegalFor<LGFeature<"HasAVX2">, ["v32s8", "v16s16", "v8s32", "v4s64"], 0>,
    LG_LegalFor<LGFeature<"HasAVX512">, ["v16s32", "v8s64"], 0>,
    LG_LegalFor<LGFeature<"HasBWI">, ["v64s8", "v32s16"], 0>,
    LG_ClampMinNumElements<"0", "s8", "16">,
    LG_ClampMinNumElements<"0", "s16", "8">,
    LG_ClampMinNumElements<"0", "s32", "4">,
    LG_ClampMinNumElements<"0", "s64", "2">,
    LG_ClampMaxNumElements<"0", "s8", "HasBWI ? 64 : (HasAVX2 ? 32 : 16)">,
    LG_ClampMaxNumElements<"0", "s16", "HasBWI ? 32 : (HasAVX2 ? 16 : 8)">,
    LG_ClampMaxNumElements<"0", "s32", "HasAVX512 ? 16 : (HasAVX2 ? 8 : 4)">,
    LG_ClampMaxNumElements<"0", "s64", "HasAVX512 ? 8 : (HasAVX2 ? 4 : 2)">,
    LG_WidenScalarToNextPow2<"0", "32">,
    LG_ClampScalar<"0", "s8", "sMaxScalar">,
    LG_Scalarize<"0">,
  ];
}

def X86_UADDE : LegalizeRuleSet<["G_UADDE", "G_UADDO", "G_USUBE", "G_USUBO"]> {
  let Rules = [
    LG_LegalForTypes<["s8, s8", "s16, s8", "s32, s8"], 1>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s64, s8"], 1>,
    LG_WidenScalarToNextPow2<"0", "32">,
    LG_ClampScalar<"0", "s8", "sMaxScalar">,
    LG_ClampScalar<"1", "s8", "s8">,
    LG_Scalarize<"0">,
  ];
}

def X86_MUL : LegalizeRuleSet<["G_MUL"]> {
  let Comments = ["integer multiply"];
  let Rules = [
    LG_LegalForTypes<["s8", "s16", "s32"], 0>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s64"], 0>,
    LG_LegalFor<LGFeature<"HasSSE2">, ["v8s16"], 0>,
    LG_LegalFor<LGFeature<"HasSSE41">, ["v4s32"], 0>,
    LG_LegalFor<LGFeature<"HasAVX2">, ["v16s16", "v8s32"], 0>,
    LG_LegalFor<LGFeature<"HasAVX512">, ["v16s32"], 0>,
    LG_LegalFor<LGFeature<"HasDQI">, ["v8s64"], 0>,
    LG_LegalFor<LGAnd<LGFeature<"HasDQI">, LGFeature<"HasVLX">>,
                ["v2s64", "v4s64"], 0>,
    LG_LegalFor<LGFeature<"HasBWI">, ["v32s16"], 0>,
    LG_ClampMinNumElements<"0", "s16", "8">,
    LG_ClampMinNumElements<"0", "s32", "4">,
    LG_ClampMinNumElements<"0", "s64", "HasVLX ? 2 : 8">,
    LG_ClampMaxNumElements<"0", "s16", "HasBWI ? 32 : (HasAVX2 ? 16 : 8)">,
    LG_ClampMaxNumElements<"0", "s32", "HasAVX512 ? 16 : (HasAVX2 ? 8 : 4)">,
    LG_ClampMaxNumElements<"0", "s64", "8">,
    LG_WidenScalarToNextPow2<"0", "32">,
    LG_ClampScalar<"0", "s8", "sMaxScalar">,
    LG_Scalarize<"0">,
  ];
}

def X86_SMULH : LegalizeRuleSet<["G_SMULH", "G_UMULH"]> {
  let Rules = [
    LG_LegalForTypes<["s8", "s16", "s32"], 0>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s64"], 0>,
    LG_WidenScalarToNextPow2<"0", "32">,
    LG_ClampScalar<"0", "s8", "sMaxScalar">,
    LG_Scalarize<"0">,
  ];
}

def X86_SDIV : LegalizeRuleSet<["G_SDIV", "G_SREM", "G_UDIV", "G_UREM"]> {
  let Comments = ["integer divisions"];
  let Rules = [
    LG_LegalForTypes<["s8", "s16", "s32"], 0>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s64"], 0>,
    LG_LibcallFor<["s64"], 0>,
    LG_ClampScalar<"0", "s8", "sMaxScalar">,
  ];
}

def X86_SHL : LegalizeRuleSet<["G_SHL", "G_LSHR", "G_ASHR"]> {
  let Comments = ["integer shifts"];
  let Rules = [
    LG_LegalForTypes<["s8, s8", "s16, s8", "s32, s8"], 1>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s64, s8"], 1>,
    LG_ClampScalar<"0", "s8", "sMaxScalar">,
    LG_ClampScalar<"1", "s8", "s8">,
  ];
}

def X86_AND : LegalizeRuleSet<["G_AND", "G_OR", "G_XOR"]> {
  let Comments = ["integer logic"];
  let Rules = [
    LG_LegalForTypes<["s8", "s16", "s32"], 0>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s64"], 0>,
    LG_LegalFor<LGFeature<"HasSSE2">, ["v16s8", "v8s16", "v4s32", "v2s64"], 0>,
    LG_LegalFor<LGFeature<"HasAVX">, ["v32s8", "v16s16", "v8s32", "v4s64"], 0>,
    LG_LegalFor<LGFeature<"HasAVX512">, ["v64s8", "v32s16", "v16s32", "v8s64"],
                0>,
    LG_ClampNumElements<"0", "v16s8", "s8MaxVector">,
    LG_ClampNumElements<"0", "v8s16", "s16MaxVector">,
    LG_ClampNumElements<"0", "v4s32", "s32MaxVector">,
    LG_ClampNumElements<"0", "v2s64", "s64MaxVector">,
    LG_WidenScalarToNextPow2<"0", "32">,
    LG_ClampScalar<"0", "s8", "sMaxScalar">,
    LG_Scalarize<"0">,
  ];
}

def X86_ICMP : LegalizeRuleSet<["G_ICMP"]> {
  let Rules = [
    LG_LegalForCartesianProduct<"{s8}", "Is64Bit ? IntTypes64 : IntTypes32">,
    LG_ClampScalar<"0", "s8", "s8">,
    LG_WidenScalarToNextPow2<"1", "8">,
    LG_ClampScalar<"1", "s8", "sMaxScalar">,
  ];
}

def X86_BSWAP : LegalizeRuleSet<["G_BSWAP"]> {
  let Comments = ["bswap"];
  let Rules = [
    LG_LegalForTypes<["s32"], 0>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s64"], 0>,
    LG_WidenScalarToNextPow2<"0", "32">,
    LG_ClampScalar<"0", "s32", "sMaxScalar">,
  ];
}

def X86_CTPOP : LegalizeRuleSet<["G_CTPOP"]> {
  let Comments = ["popcount"];
  let Rules = [
    LG_LegalFor<LGFeature<"HasPOPCNT">, ["s16, s16", "s32, s32"], 1>,
    LG_LegalFor<LGAnd<LGFeature<"HasPOPCNT">, LGFeature<"Is64Bit">>,
                ["s64, s64"], 1>,
    LG_WidenScalarToNextPow2<"1", "16">,
    LG_ClampScalar<"1", "s16", "sMaxScalar">,
    LG_ScalarSameSizeAs<"0", "1">,
  ];
}

def X86_CTLZ : LegalizeRuleSet<["G_CTLZ"]> {
  let Comments = ["count leading zeros (LZCNT)"];
  let Rules = [
    LG_LegalFor<LGFeature<"HasLZCNT">, ["s16, s16", "s32, s32"], 1>,
    LG_LegalFor<LGAnd<LGFeature<"HasLZCNT">, LGFeature<"Is64Bit">>,
                ["s64, s64"], 1>,
    LG_WidenScalarToNextPow2<"1", "16">,
    LG_ClampScalar<"1", "s16", "sMaxScalar">,
    LG_ScalarSameSizeAs<"0", "1">,
  ];
}

def X86_CTTZ_ZERO_POISON : LegalizeRuleSet<["G_CTTZ_ZERO_POISON"]> {
  let Comments = ["count trailing zeros"];
  let Rules = [
    LG_LegalForTypes<["s16, s16", "s32, s32"], 1>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s64, s64"], 1>,
    LG_WidenScalarToNextPow2<"1", "16">,
    LG_ClampScalar<"1", "s16", "sMaxScalar">,
    LG_ScalarSameSizeAs<"0", "1">,
  ];
}

def X86_CTTZ : LegalizeRuleSet<["G_CTTZ"]> {
  let Rules = [
    LG_LegalFor<LGFeature<"HasBMI">, ["s16, s16", "s32, s32"], 1>,
    LG_LegalFor<LGAnd<LGFeature<"HasBMI">, LGFeature<"Is64Bit">>, ["s64, s64"],
                1>,
    LG_WidenScalarToNextPow2<"1", "16">,
    LG_ClampScalar<"1", "s16", "sMaxScalar">,
    LG_ScalarSameSizeAs<"0", "1">,
  ];
}

def X86_BR : LegalizeRuleSet<["G_BR"]> {
  let Rules = [
    LG_AlwaysLegal,
  ];
}

def X86_BRCOND : LegalizeRuleSet<["G_BRCOND"]> {
  let Rules = [
    LG_LegalForTypes<["s1"], 0>,
  ];
}

def X86_PTRTOINT : LegalizeRuleSet<["G_PTRTOINT"]> {
  let Rules = [
    LG_LegalForCartesianProduct<"Is64Bit ? PtrTypes64 : PtrTypes32", "{p0}">,
    LG_MaxScalar<"0", "sMaxScalar">,
    LG_WidenScalarToNextPow2<"0", "8">,
  ];
}

def X86_INTTOPTR : LegalizeRuleSet<["G_INTTOPTR"]> {
  let Rules = [
    LG_LegalForTypes<["p0, sMaxScalar"], 1>,
  ];
}

def X86_CONSTANT_POOL : LegalizeRuleSet<["G_CONSTANT_POOL"]> {
  let Rules = [
    LG_LegalForTypes<["p0"], 0>,
  ];
}

def X86_PTR_ADD : LegalizeRuleSet<["G_PTR_ADD"]> {
  let Rules = [
    LG_LegalForTypes<["p0, s32"], 1>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["p0, s64"], 1>,
    LG_WidenScalarToNextPow2<"1", "32">,
    LG_ClampScalar<"1", "s32", "sMaxScalar">,
  ];
}

def X86_FRAME_INDEX : LegalizeRuleSet<["G_FRAME_INDEX"]> {
  let Rules = [
    LG_LegalForTypes<["p0"], 0>,
  ];
}

def X86_GLOBAL_VALUE : LegalizeRuleSet<["G_GLOBAL_VALUE"]> {
  let Rules = [
    LG_CustomFor<["p0"], 0>,
  ];
}

def X86_LOAD : LegalizeRuleSet<["G_LOAD"]> {
  let Comments = ["load/store: add more corner cases"];
  let Rules = [
    LG_LegalForMemDesc<["s8, p0, s8, 1", "s16, p0, s16, 1",
                        "s32, p0, s32, 1", "s80, p0, s80, 1",
                        "p0, p0, p0, 1", "v4s8, p0, v4s8, 1"]>,
    LG_LegalForMemDescIf<LGFeature<"Is64Bit">,
                         ["s64, p0, s64, 1", "v2s32, p0, v2s32, 1"]>,
    LG_LegalForMemDescIf<LGFeature<"HasSSE1">, ["v4s32, p0, v4s32, 1"]>,
    LG_LegalForMemDescIf<LGFeature<"HasSSE2">,
                         ["v16s8, p0, v16s8, 1", "v8s16, p0, v8s16, 1",
                          "v2s64, p0, v2s64, 1", "v2p0, p0, v2p0, 1"]>,
    LG_LegalForMemDescIf<LGFeature<"HasAVX">,
                         ["v32s8, p0, v32s8, 1", "v16s16, p0, v16s16, 1",
                          "v8s32, p0, v8s32, 1", "v4s64, p0, v4s64, 1",
                          "v4p0, p0, v4p0, 1"]>,
    LG_LegalForMemDescIf<LGFeature<"HasAVX512">,
                         ["v64s8, p0, v64s8, 1", "v32s16, p0, v32s16, 1",
                          "v16s32, p0, v16s32, 1", "v8s64, p0, v8s64, 1"]>,
    // X86 supports extending loads but not stores for GPRs.
    LG_LegalForMemDesc<["s8, p0, s1, 1", "s16, p0, s8, 1",
                        "s32, p0, s8, 1", "s32, p0, s16, 1"]>,
    LG_LegalForMemDescIf<LGFeature<"Is64Bit">,
                         ["s64, p0, s8, 1", "s64, p0, s16, 1",
                          "s64, p0, s32, 1"]>,
    LG_WidenScalarToNextPow2<"0", "8">,
    LG_ClampScalar<"0", "s8", "sMaxScalar">,
    LG_Scalarize<"0">,
  ];
}

def X86_STORE : LegalizeRuleSet<["G_STORE"]> {
  let Rules = [
    LG_LegalForMemDesc<["s8, p0, s8, 1", "s16, p0, s16, 1",
                        "s32, p0, s32, 1", "s80, p0, s80, 1",
                        "p0, p0, p0, 1", "v4s8, p0, v4s8, 1"]>,
    LG_LegalForMemDescIf<LGFeature<"Is64Bit">,
                         ["s64, p0, s64, 1", "v2s32, p0, v2s32, 1"]>,
    LG_LegalForMemDescIf<LGFeature<"HasSSE1">, ["v4s32, p0, v4s32, 1"]>,
    LG_LegalForMemDescIf<LGFeature<"HasSSE2">,
                         ["v16s8, p0, v16s8, 1", "v8s16, p0, v8s16, 1",
                          "v2s64, p0, v2s64, 1", "v2p0, p0, v2p0, 1"]>,
    LG_LegalForMemDescIf<LGFeature<"HasAVX">,
                         ["v32s8, p0, v32s8, 1", "v16s16, p0, v16s16, 1",
                          "v8s32, p0, v8s32, 1", "v4s64, p0, v4s64, 1",
                          "v4p0, p0, v4p0, 1"]>,
    LG_LegalForMemDescIf<LGFeature<"HasAVX512">,
                         ["v64s8, p0, v64s8, 1", "v32s16, p0, v32s16, 1",
                          "v16s32, p0, v16s32, 1", "v8s64, p0, v8s64, 1"]>,
    LG_OpaqueRule<"customIf", [{

      [=](const LegalityQuery &Query) {
        return Query.Types[0] != Query.MMODescrs[0].MemoryTy;
      }
    }]>,
    LG_WidenScalarToNextPow2<"0", "8">,
    LG_ClampScalar<"0", "s8", "sMaxScalar">,
    LG_Scalarize<"0">,
  ];
}

def X86_SEXTLOAD : LegalizeRuleSet<["G_SEXTLOAD"]> {
  let Rules = [
    LG_LegalForMemDesc<["s16, p0, s8, 1", "s32, p0, s8, 1",
                        "s32, p0, s16, 1"]>,
    LG_LegalForMemDescIf<LGFeature<"Is64Bit">,
                         ["s64, p0, s8, 1", "s64, p0, s16, 1",
                          "s64, p0, s32, 1"]>,
  ];
}

def X86_ZEXTLOAD : LegalizeRuleSet<["G_ZEXTLOAD"]> {
  let Rules = [
    LG_LegalForMemDesc<["s16, p0, s8, 1", "s32, p0, s8, 1",
                        "s32, p0, s16, 1"]>,
    LG_LegalForMemDescIf<LGFeature<"Is64Bit">,
                         ["s64, p0, s8, 1", "s64, p0, s16, 1",
                          "s64, p0, s32, 1"]>,
  ];
}

def X86_FPEXTLOAD : LegalizeRuleSet<["G_FPEXTLOAD"]> {
  let Rules = [
    LG_LegalForMemDescIf<LGFeature<"UseX87">,
                         ["s80, p0, s32, 1", "s80, p0, s64, 1",
                          "s64, p0, s32, 1"]>,
  ];
}

def X86_FPTRUNCSTORE : LegalizeRuleSet<["G_FPTRUNCSTORE"]> {
  let Rules = [
    LG_LegalForMemDescIf<LGFeature<"UseX87">,
                         ["s80, p0, s32, 1", "s80, p0, s64, 1",
                          "s64, p0, s32, 1"]>,
  ];
}

def X86_ANYEXT : LegalizeRuleSet<["G_ANYEXT"]> {
  let Comments = ["sext, zext, and anyext"];
  let Rules = [
    LG_LegalForTypes<["s8", "s16", "s32", "s128"], 0>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s64"], 0>,
    LG_WidenScalarToNextPow2<"0", "8">,
    LG_ClampScalar<"0", "s8", "sMaxScalar">,
    LG_WidenScalarToNextPow2<"1", "8">,
    LG_ClampScalar<"1", "s8", "sMaxScalar">,
    LG_Scalarize<"0">,
  ];
}

def X86_SEXT : LegalizeRuleSet<["G_SEXT", "G_ZEXT"]> {
  let Rules = [
    LG_LegalForTypes<["s8", "s16", "s32"], 0>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s64"], 0>,
    LG_WidenScalarToNextPow2<"0", "8">,
    LG_ClampScalar<"0", "s8", "sMaxScalar">,
    LG_WidenScalarToNextPow2<"1", "8">,
    LG_ClampScalar<"1", "s8", "sMaxScalar">,
    LG_Scalarize<"0">,
  ];
}

def X86_TRUNC : LegalizeRuleSet<["G_TRUNC"]> {
  let Rules = [
    LG_LegalForCartesianProduct<"{s1, s8, s16, s32, s64}",
                                "{s8, s16, s32, s64, s128}">,
  ];
}

def X86_SEXT_INREG : LegalizeRuleSet<["G_SEXT_INREG"]> {
  let Rules = [
    LG_Lower,
  ];
}

def X86_FCONSTANT : LegalizeRuleSet<["G_FCONSTANT"]> {
  let Comments = ["fp constants"];
  let Rules = [
    LG_LegalForTypes<["s32", "s64"], 0>,
    LG_LegalFor<LGFeature<"UseX87">, ["s80"], 0>,
  ];
}

def X86_FADD : LegalizeRuleSet<["G_FADD", "G_FSUB", "G_FMUL", "G_FDIV"]> {
  let Comments = ["fp arithmetic"];
  let Rules = [
    LG_LegalForTypes<["s32", "s64"], 0>,
    LG_LegalFor<LGFeature<"HasSSE1">, ["v4s32"], 0>,
    LG_LegalFor<LGFeature<"HasSSE2">, ["v2s64"], 0>,
    LG_LegalFor<LGFeature<"HasAVX">, ["v8s32", "v4s64"], 0>,
    LG_LegalFor<LGFeature<"HasAVX512">, ["v16s32", "v8s64"], 0>,
    LG_LegalFor<LGFeature<"UseX87">, ["s80"], 0>,
  ];
}

def X86_FABS : LegalizeRuleSet<["G_FABS"]> {
  let Rules = [
    LG_LegalFor<LGFeature<"UseX87">, ["s80"], 0>,
    LG_LegalFor<LGExpr<"UseX87 && !Is64Bit">, ["s64"], 0>,
    LG_Lower,
  ];
}

def X86_FCMP : LegalizeRuleSet<["G_FCMP"]> {
  let Comments = ["fp comparison"];
  let Rules = [
    LG_LegalFor<LGExpr<"HasSSE1 || UseX87">, ["s8", "s32"], 0>,
    LG_LegalFor<LGExpr<"HasSSE2 || UseX87">, ["s8", "s64"], 0>,
    LG_LegalFor<LGFeature<"UseX87">, ["s8", "s80"], 0>,
    LG_ClampScalar<"0", "s8", "s8">,
    LG_ClampScalar<"1", "s32", "HasSSE2 ? s64 : s32">,
    LG_WidenScalarToNextPow2<"1">,
  ];
}

def X86_FPEXT : LegalizeRuleSet<["G_FPEXT"]> {
  let Comments = ["fp conversions"];
  let Rules = [
    LG_LegalFor<LGFeature<"HasSSE2">, ["s64, s32"], 1>,
    LG_LegalFor<LGFeature<"HasAVX">, ["v4s64, v4s32"], 1>,
    LG_LegalFor<LGFeature<"HasAVX512">, ["v8s64, v8s32"], 1>,
    LG_LowerFor<LGFeature<"UseX87">, ["s64, s32", "s80, s32", "s80, s64"], 1>,
    LG_Libcall,
  ];
}

def X86_FPTRUNC : LegalizeRuleSet<["G_FPTRUNC"]> {
  let Rules = [
    LG_LegalFor<LGFeature<"HasSSE2">, ["s32, s64"], 1>,
    LG_LegalFor<LGFeature<"HasAVX">, ["v4s32, v4s64"], 1>,
    LG_LegalFor<LGFeature<"HasAVX512">, ["v8s32, v8s64"], 1>,
    LG_LowerFor<LGFeature<"UseX87">, ["s32, s64", "s32, s80", "s64, s80"], 1>,
  ];
}

def X86_SITOFP : LegalizeRuleSet<["G_SITOFP"]> {
  let Rules = [
    LG_LegalFor<LGFeature<"HasSSE1">, ["s32, s32"], 1>,
    LG_LegalFor<LGAnd<LGFeature<"HasSSE1">, LGFeature<"Is64Bit">>, ["s32, s64"],
                1>,
    LG_LegalFor<LGFeature<"HasSSE2">, ["s64, s32"], 1>,
    LG_LegalFor<LGAnd<LGFeature<"HasSSE2">, LGFeature<"Is64Bit">>, ["s64, s64"],
                1>,
    LG_ClampScalar<"1", "(UseX87 && !HasSSE1) ? s16 : s32", "sMaxScalar">,
    LG_WidenScalarToNextPow2<"1">,
    LG_CustomForCartesianProduct<LGFeature<"UseX87">, "{s32, s64, s80}",
                                 "{s16, s32, s64}">,
    LG_ClampScalar<"0", "s32", "HasSSE2 ? s64 : s32">,
    LG_WidenScalarToNextPow2<"0">,
  ];
}

def X86_FPTOSI : LegalizeRuleSet<["G_FPTOSI"]> {
  let Rules = [
    LG_LegalFor<LGFeature<"HasSSE1">, ["s32, s32"], 1>,
    LG_LegalFor<LGAnd<LGFeature<"HasSSE1">, LGFeature<"Is64Bit">>, ["s64, s32"],
                1>,
    LG_LegalFor<LGFeature<"HasSSE2">, ["s32, s64"], 1>,
    LG_LegalFor<LGAnd<LGFeature<"HasSSE2">, LGFeature<"Is64Bit">>, ["s64, s64"],
                1>,
    LG_ClampScalar<"0", "(UseX87 && !HasSSE1) ? s16 : s32", "sMaxScalar">,
    LG_WidenScalarToNextPow2<"0">,
    LG_CustomForCartesianProduct<LGFeature<"UseX87">, "{s16, s32, s64}",
                                 "{s32, s64, s80}">,
    LG_ClampScalar<"1", "s32", "HasSSE2 ? s64 : s32">,
    LG_WidenScalarToNextPow2<"1">,
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
    LG_LegalFor<LGFeature<"HasAVX512">,
                ["s32, s32", "s32, s64", "s64, s32", "s64, s64"], 1>,
    LG_OpaqueRule<"customIf", [{

      [=](const LegalityQuery &Query) {
              return !HasAVX512 &&
                     ((HasSSE1 && typeIs(0, s32)(Query)) ||
                      (HasSSE2 && typeIs(0, s64)(Query))) &&
                     scalarNarrowerThan(1, Is64Bit ? 64 : 32)(Query);
            }
    }]>,
    LG_OpaqueRule<"lowerIf", [{

      [=](const LegalityQuery &Query) {
              // Lower conversions from s64
              return !HasAVX512 &&
                     ((HasSSE1 && typeIs(0, s32)(Query)) ||
                      (HasSSE2 && typeIs(0, s64)(Query))) &&
                     (Is64Bit && typeIs(1, s64)(Query));
            }
    }]>,
    LG_ClampScalar<"0", "s32", "HasSSE2 ? s64 : s32">,
    LG_WidenScalarToNextPow2<"0">,
    LG_ClampScalar<"1", "s32", "sMaxScalar">,
    LG_WidenScalarToNextPow2<"1">,
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
    LG_LegalFor<LGFeature<"HasAVX512">,
                ["s32, s32", "s32, s64", "s64, s32", "s64, s64"], 1>,
    LG_OpaqueRule<"customIf", [{

      [=](const LegalityQuery &Query) {
        return !HasAVX512 &&
               ((HasSSE1 && typeIs(1, s32)(Query)) ||
                (HasSSE2 && typeIs(1, s64)(Query))) &&
               scalarNarrowerThan(0, Is64Bit ? 64 : 32)(Query);
      }
    }]>,
    LG_OpaqueRule<"lowerIf", [{

      [=](const LegalityQuery &Query) {
        return !HasAVX512 &&
               ((HasSSE1 && typeIs(1, s32)(Query)) ||
                (HasSSE2 && typeIs(1, s64)(Query))) &&
               (Is64Bit && typeIs(0, s64)(Query));
      }
    }]>,
    LG_ClampScalar<"0", "s32", "sMaxScalar">,
    LG_WidenScalarToNextPow2<"0">,
    LG_ClampScalar<"1", "s32", "HasSSE2 ? s64 : s32">,
    LG_WidenScalarToNextPow2<"1">,
  ];
}

def X86_BUILD_VECTOR : LegalizeRuleSet<["G_BUILD_VECTOR"]> {
  let Comments = ["vector ops"];
  let Rules = [
    LG_OpaqueRule<"customIf", [{

      [=](const LegalityQuery &Query) {
              return (HasSSE1 && typeInSet(0, {v4s32})(Query)) ||
                     (HasSSE2 && typeInSet(0, {v2s64, v8s16, v16s8})(Query)) ||
                     (HasAVX &&
                      typeInSet(0, {v4s64, v8s32, v16s16, v32s8})(Query)) ||
                     (HasAVX512 &&
                      typeInSet(0, {v8s64, v16s32, v32s16, v64s8})(Query));
            }
    }]>,
    LG_ClampNumElements<"0", "v16s8", "s8MaxVector">,
    LG_ClampNumElements<"0", "v8s16", "s16MaxVector">,
    LG_ClampNumElements<"0", "v4s32", "s32MaxVector">,
    LG_ClampNumElements<"0", "v2s64", "s64MaxVector">,
    LG_MoreElementsToNextPow2<"0">,
  ];
}

def X86_EXTRACT : LegalizeRuleSet<["G_EXTRACT", "G_INSERT"]> {
  let Rules = [
    LG_OpaqueRule<"legalIf", [{

      [=](const LegalityQuery &Query) {
              unsigned SubIdx = Query.Opcode == G_EXTRACT ? 0 : 1;
              unsigned FullIdx = Query.Opcode == G_EXTRACT ? 1 : 0;
              return (HasAVX && typePairInSet(SubIdx, FullIdx,
                                              {{v16s8, v32s8},
                                               {v8s16, v16s16},
                                               {v4s32, v8s32},
                                               {v2s64, v4s64}})(Query)) ||
                     (HasAVX512 && typePairInSet(SubIdx, FullIdx,
                                                 {{v16s8, v64s8},
                                                  {v32s8, v64s8},
                                                  {v8s16, v32s16},
                                                  {v16s16, v32s16},
                                                  {v4s32, v16s32},
                                                  {v8s32, v16s32},
                                                  {v2s64, v8s64},
                                                  {v4s64, v8s64}})(Query));
            }
    }]>,
  ];
}

def X86_CONCAT_VECTORS : LegalizeRuleSet<["G_CONCAT_VECTORS"]> {
  let Comments = [
    "todo: only permit dst types up to max legal vector register size?",
  ];
  let Rules = [
    LG_LegalFor<LGFeature<"HasSSE1">,
                ["v32s8, v16s8", "v16s16, v8s16",
                 "v8s32, v4s32", "v4s64, v2s64"], 1>,
    LG_LegalFor<LGFeature<"HasAVX">,
                ["v64s8, v16s8", "v64s8, v32s8", "v32s16, v8s16",
                 "v32s16, v16s16", "v16s32, v4s32", "v16s32, v8s32",
                 "v8s64, v2s64", "v8s64, v4s64"], 1>,
  ];
}

def X86_SELECT : LegalizeRuleSet<["G_SELECT"]> {
  let Comments = ["todo: vectors and address spaces"];
  let Rules = [
    LG_LegalForTypes<["s16, s32", "s32, s32", "p0, s32"], 1>,
    LG_LegalFor<LGNot<LGFeature<"HasCMOV">>, ["s8, s32"], 1>,
    LG_LegalFor<LGFeature<"Is64Bit">, ["s64, s32"], 1>,
    LG_LegalFor<LGFeature<"UseX87">, ["s80, s32"], 1>,
    LG_ClampScalar<"1", "s32", "s32">,
    LG_WidenScalarToNextPow2<"0", "8">,
    LG_ClampScalar<"0", "HasCMOV ? s16 : s8", "sMaxScalar">,
  ];
}

def X86_MEMCPY : LegalizeRuleSet<["G_MEMCPY", "G_MEMMOVE", "G_MEMSET"]> {
  let Comments = ["memory intrinsics"];
  let Rules = [
    LG_Libcall,
  ];
}

def X86_DYN_STACKALLOC : LegalizeRuleSet<[
  "G_DYN_STACKALLOC", "G_STACKSAVE", "G_STACKRESTORE"
]> {
  let Rules = [
    LG_Lower,
  ];
}

def X86_IS_FPCLASS : LegalizeRuleSet<["G_IS_FPCLASS"]> {
  let Comments = [
    "fp intrinsics",
    "fpclass for i686 is disabled for llvm issue #171992",
  ];
  let Rules = [
    LG_LowerFor<LGFeature<"Is64Bit">, ["s1, s32", "s1, s64", "s1, s80"], 1>,
  ];
}

def X86_INTRINSIC_ROUNDEVEN : LegalizeRuleSet<[
  "G_INTRINSIC_ROUNDEVEN", "G_INTRINSIC_TRUNC"
]> {
  let Rules = [
    LG_Scalarize<"0">,
    LG_MinScalar<"0", "LLT::scalar(32)">,
    LG_Libcall,
  ];
}

def X86_INTRINSIC : LegalizeRuleSet<[
  "G_INTRINSIC", "G_INTRINSIC_W_SIDE_EFFECTS"
]> {
  let Rules = [
    LG_AlwaysLegal,
  ];
}

def X86_TRAP : LegalizeRuleSet<["G_TRAP", "G_DEBUGTRAP", "G_UBSANTRAP"]> {
  let Rules = [
    LG_AlwaysLegal,
  ];
}

def X86_INVOKE_REGION_START : LegalizeRuleSet<["G_INVOKE_REGION_START"]> {
  let Rules = [
    LG_AlwaysLegal,
  ];
}
