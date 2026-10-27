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
// The LegalizeFeature records below declare the subtarget-feature booleans the
// rules are written in terms of, and X86_Context declares the LLT constants, so
// this file is the whole legalizer: there is no longer a hand-written prologue
// in the constructor.
//
// LegalizeOpaqueRule is the escape hatch for guards that are still raw C++
// lambdas: 10 rules out of 239.
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

// The LLT constants the rules are written in terms of.  The def name is the
// C++ identifier, exactly like the LegalizeFeature records above.
//
// TODO: AVX512 bool vector predicate types.

def p0 : LegalizeType<
    "LLT::pointer(0, Subtarget.isTarget64BitLP64() ? 64 : 32)">;
def s1 : LegalizeType<"LLT::scalar(1)">;
def s8 : LegalizeType<"LLT::scalar(8)">;
def s16 : LegalizeType<"LLT::scalar(16)">;
def s32 : LegalizeType<"LLT::scalar(32)">;
def s64 : LegalizeType<"LLT::scalar(64)">;
def s80 : LegalizeType<"LLT::scalar(80)">;
def s128 : LegalizeType<"LLT::scalar(128)">;
def sMaxScalar : LegalizeType<"Subtarget.is64Bit() ? s64 : s32">;

def v2s32 : LegalizeType<"LLT::fixed_vector(2, 32)">;
def v4s8 : LegalizeType<"LLT::fixed_vector(4, 8)">;

def v16s8 : LegalizeType<"LLT::fixed_vector(16, 8)">;
def v8s16 : LegalizeType<"LLT::fixed_vector(8, 16)">;
def v4s32 : LegalizeType<"LLT::fixed_vector(4, 32)">;
def v2s64 : LegalizeType<"LLT::fixed_vector(2, 64)">;
def v2p0 : LegalizeType<"LLT::fixed_vector(2, p0)">;

def v32s8 : LegalizeType<"LLT::fixed_vector(32, 8)">;
def v16s16 : LegalizeType<"LLT::fixed_vector(16, 16)">;
def v8s32 : LegalizeType<"LLT::fixed_vector(8, 32)">;
def v4s64 : LegalizeType<"LLT::fixed_vector(4, 64)">;
def v4p0 : LegalizeType<"LLT::fixed_vector(4, p0)">;

def v64s8 : LegalizeType<"LLT::fixed_vector(64, 8)">;
def v32s16 : LegalizeType<"LLT::fixed_vector(32, 16)">;
def v16s32 : LegalizeType<"LLT::fixed_vector(16, 32)">;
def v8s64 : LegalizeType<"LLT::fixed_vector(8, 64)">;

def s8MaxVector : LegalizeType<"HasAVX512 ? v64s8 : HasAVX ? v32s8 : v16s8">;
def s16MaxVector : LegalizeType<
    "HasAVX512 ? v32s16 : HasAVX ? v16s16 : v8s16">;
def s32MaxVector : LegalizeType<
    "HasAVX512 ? v16s32 : HasAVX ? v8s32 : v4s32">;
def s64MaxVector : LegalizeType<"HasAVX512 ? v8s64 : HasAVX ? v4s64 : v2s64">;

// Types used by individual rule sets.
def IntTypes32 : LegalizeTypeList<[s8, s16, s32, p0]>;
def IntTypes64 : LegalizeTypeList<[s8, s16, s32, s64, p0]>;
def PtrTypes32 : LegalizeTypeList<[s1, s8, s16, s32]>;
def PtrTypes64 : LegalizeTypeList<[s1, s8, s16, s32, s64]>;

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
    LegalFor<[p0, s1, s8, s16, s32, s64]>,
    LegalForIf<UseX87, [s80]>,
    LegalForIf<Is64Bit, [s128]>,
    LegalForIf<HasSSE2, [v16s8, v8s16, v4s32, v2s64]>,
    LegalForIf<HasAVX, [v32s8, v16s16, v8s32, v4s64]>,
    LegalForIf<HasAVX512, [v64s8, v32s16, v16s32, v8s64]>,
    WidenScalarOrEltToNextPow2<0, 8>,
    ClampScalarOrElt<0, s8, sMaxScalar>,
    MoreElementsToNextPow2<0>,
    ClampNumElements<0, v16s8, s8MaxVector>,
    ClampNumElements<0, v8s16, s16MaxVector>,
    ClampNumElements<0, v4s32, s32MaxVector>,
    ClampNumElements<0, v2s64, s64MaxVector>,
    ClampMaxNumElements<0, p0,
                                "Is64Bit ? s64MaxVector.getNumElements() : s32MaxVector.getNumElements()">,
    ScalarizeIf<LegalizeOpaquePred<"scalarOrEltWiderThan(0, 64)">, 0>,
  ];
}

def X86_CONSTANT : LegalizeRuleSet<["G_CONSTANT"]> {
  let Rules = [
    LegalFor<[p0, s8, s16, s32]>,
    LegalForIf<Is64Bit, [s64]>,
    WidenScalarToNextPow2<0, 8>,
    ClampScalar<0, s8, sMaxScalar>,
  ];
}

def X86_LROUND : LegalizeRuleSet<["G_LROUND", "G_LLROUND"]> {
  let Rules = [
    LegalizeOpaqueRule<"widenScalarIf", [{

      typeIs(1, s16),
      [=](const LegalityQuery &) {
        return std::pair<unsigned, LLT>(1, s32);
      }
    }], ActionWidenScalar, "widen">,
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
    LegalForIf<UseX87NotSSE1, [s32]>,
    LegalForIf<UseX87NotSSE2, [s64]>,
    LegalForIf<UseX87, [s80]>,
    Lower,
  ];
}

def X86_FSQRT : LegalizeRuleSet<["G_FSQRT"]> {
  let Rules = [
    LegalForIf<SSE1OrX87, [s32]>,
    LegalForIf<SSE2OrX87, [s64]>,
    LegalForIf<UseX87, [s80]>,
  ];
}

def X86_GET_ROUNDING : LegalizeRuleSet<["G_GET_ROUNDING", "G_SET_ROUNDING"]> {
  let Rules = [
    CustomFor<[s32]>,
  ];
}

def X86_MERGE_VALUES : LegalizeRuleSet<["G_MERGE_VALUES"]> {
  let Rules = [
    WidenScalarToNextPow2<1, 8>,
    WidenScalarToNextPow2<0, 16>,
    MinScalar<1, s8>,
    MinScalar<0, s32>,
    LegalizeOpaqueRule<"legalIf", [{

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
    }], ActionLegal>,
  ];
}

def X86_UNMERGE_VALUES : LegalizeRuleSet<["G_UNMERGE_VALUES"]> {
  let Rules = [
    WidenScalarToNextPow2<0, 8>,
    WidenScalarToNextPow2<1, 16>,
    MinScalar<0, s8>,
    MinScalar<1, s32>,
    LegalizeOpaqueRule<"legalIf", [{

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
    }], ActionLegal>,
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
    LegalFor<[s8, s16, s32]>,
    LegalForIf<Is64Bit, [s64]>,
    LegalForIf<HasSSE2, [v16s8, v8s16, v4s32, v2s64]>,
    LegalForIf<HasAVX2, [v32s8, v16s16, v8s32, v4s64]>,
    LegalForIf<HasAVX512, [v16s32, v8s64]>,
    LegalForIf<HasBWI, [v64s8, v32s16]>,
    ClampMinNumElements<0, s8, "16">,
    ClampMinNumElements<0, s16, "8">,
    ClampMinNumElements<0, s32, "4">,
    ClampMinNumElements<0, s64, "2">,
    ClampMaxNumElements<0, s8, "HasBWI ? 64 : (HasAVX2 ? 32 : 16)">,
    ClampMaxNumElements<0, s16, "HasBWI ? 32 : (HasAVX2 ? 16 : 8)">,
    ClampMaxNumElements<0, s32,
                                "HasAVX512 ? 16 : (HasAVX2 ? 8 : 4)">,
    ClampMaxNumElements<0, s64,
                                "HasAVX512 ? 8 : (HasAVX2 ? 4 : 2)">,
    WidenScalarToNextPow2<0, 32>,
    ClampScalar<0, s8, sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_UADDE : LegalizeRuleSet<["G_UADDE", "G_UADDO", "G_USUBE", "G_USUBO"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<s8, s8>, LegalizeTypePair<s16, s8>, LegalizeTypePair<s32, s8>]>,
    LegalForPairsIf<Is64Bit, [LegalizeTypePair<s64, s8>]>,
    WidenScalarToNextPow2<0, 32>,
    ClampScalar<0, s8, sMaxScalar>,
    ClampScalar<1, s8, s8>,
    Scalarize<0>,
  ];
}

def X86_MUL : LegalizeRuleSet<["G_MUL"]> {
  let Comments = ["integer multiply"];
  let Rules = [
    LegalFor<[s8, s16, s32]>,
    LegalForIf<Is64Bit, [s64]>,
    LegalForIf<HasSSE2, [v8s16]>,
    LegalForIf<HasSSE41, [v4s32]>,
    LegalForIf<HasAVX2, [v16s16, v8s32]>,
    LegalForIf<HasAVX512, [v16s32]>,
    LegalForIf<HasDQI, [v8s64]>,
    LegalForIf<DQIAndVLX, [v2s64, v4s64]>,
    LegalForIf<HasBWI, [v32s16]>,
    ClampMinNumElements<0, s16, "8">,
    ClampMinNumElements<0, s32, "4">,
    ClampMinNumElements<0, s64, "HasVLX ? 2 : 8">,
    ClampMaxNumElements<0, s16, "HasBWI ? 32 : (HasAVX2 ? 16 : 8)">,
    ClampMaxNumElements<0, s32,
                                "HasAVX512 ? 16 : (HasAVX2 ? 8 : 4)">,
    ClampMaxNumElements<0, s64, "8">,
    WidenScalarToNextPow2<0, 32>,
    ClampScalar<0, s8, sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_SMULH : LegalizeRuleSet<["G_SMULH", "G_UMULH"]> {
  let Rules = [
    LegalFor<[s8, s16, s32]>,
    LegalForIf<Is64Bit, [s64]>,
    WidenScalarToNextPow2<0, 32>,
    ClampScalar<0, s8, sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_SDIV : LegalizeRuleSet<["G_SDIV", "G_SREM", "G_UDIV", "G_UREM"]> {
  let Comments = ["integer divisions"];
  let Rules = [
    LegalFor<[s8, s16, s32]>,
    LegalForIf<Is64Bit, [s64]>,
    LibcallFor<[s64]>,
    ClampScalar<0, s8, sMaxScalar>,
  ];
}

def X86_SHL : LegalizeRuleSet<["G_SHL", "G_LSHR", "G_ASHR"]> {
  let Comments = ["integer shifts"];
  let Rules = [
    LegalForPairs<[LegalizeTypePair<s8, s8>, LegalizeTypePair<s16, s8>, LegalizeTypePair<s32, s8>]>,
    LegalForPairsIf<Is64Bit, [LegalizeTypePair<s64, s8>]>,
    ClampScalar<0, s8, sMaxScalar>,
    ClampScalar<1, s8, s8>,
  ];
}

def X86_AND : LegalizeRuleSet<["G_AND", "G_OR", "G_XOR"]> {
  let Comments = ["integer logic"];
  let Rules = [
    LegalFor<[s8, s16, s32]>,
    LegalForIf<Is64Bit, [s64]>,
    LegalForIf<HasSSE2, [v16s8, v8s16, v4s32, v2s64]>,
    LegalForIf<HasAVX, [v32s8, v16s16, v8s32, v4s64]>,
    LegalForIf<HasAVX512, [v64s8, v32s16, v16s32, v8s64]>,
    ClampNumElements<0, v16s8, s8MaxVector>,
    ClampNumElements<0, v8s16, s16MaxVector>,
    ClampNumElements<0, v4s32, s32MaxVector>,
    ClampNumElements<0, v2s64, s64MaxVector>,
    WidenScalarToNextPow2<0, 32>,
    ClampScalar<0, s8, sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_ICMP : LegalizeRuleSet<["G_ICMP"]> {
  let Rules = [
    LegalForCartesianProduct<"{s8}",
                                     "Is64Bit ? IntTypes64 : IntTypes32">,
    ClampScalar<0, s8, s8>,
    WidenScalarToNextPow2<1, 8>,
    ClampScalar<1, s8, sMaxScalar>,
  ];
}

def X86_BSWAP : LegalizeRuleSet<["G_BSWAP"]> {
  let Comments = ["bswap"];
  let Rules = [
    LegalFor<[s32]>,
    LegalForIf<Is64Bit, [s64]>,
    WidenScalarToNextPow2<0, 32>,
    ClampScalar<0, s32, sMaxScalar>,
  ];
}

def X86_CTPOP : LegalizeRuleSet<["G_CTPOP"]> {
  let Comments = ["popcount"];
  let Rules = [
    LegalForPairsIf<HasPOPCNT, [LegalizeTypePair<s16, s16>, LegalizeTypePair<s32, s32>]>,
    LegalForPairsIf<POPCNTAnd64Bit, [LegalizeTypePair<s64, s64>]>,
    WidenScalarToNextPow2<1, 16>,
    ClampScalar<1, s16, sMaxScalar>,
    ScalarSameSizeAs<0, 1>,
  ];
}

def X86_CTLZ : LegalizeRuleSet<["G_CTLZ"]> {
  let Comments = ["count leading zeros (LZCNT)"];
  let Rules = [
    LegalForPairsIf<HasLZCNT, [LegalizeTypePair<s16, s16>, LegalizeTypePair<s32, s32>]>,
    LegalForPairsIf<LZCNTAnd64Bit, [LegalizeTypePair<s64, s64>]>,
    WidenScalarToNextPow2<1, 16>,
    ClampScalar<1, s16, sMaxScalar>,
    ScalarSameSizeAs<0, 1>,
  ];
}

def X86_CTTZ_ZERO_POISON : LegalizeRuleSet<["G_CTTZ_ZERO_POISON"]> {
  let Comments = ["count trailing zeros"];
  let Rules = [
    LegalForPairs<[LegalizeTypePair<s16, s16>, LegalizeTypePair<s32, s32>]>,
    LegalForPairsIf<Is64Bit, [LegalizeTypePair<s64, s64>]>,
    WidenScalarToNextPow2<1, 16>,
    ClampScalar<1, s16, sMaxScalar>,
    ScalarSameSizeAs<0, 1>,
  ];
}

def X86_CTTZ : LegalizeRuleSet<["G_CTTZ"]> {
  let Rules = [
    LegalForPairsIf<HasBMI, [LegalizeTypePair<s16, s16>, LegalizeTypePair<s32, s32>]>,
    LegalForPairsIf<BMIAnd64Bit, [LegalizeTypePair<s64, s64>]>,
    WidenScalarToNextPow2<1, 16>,
    ClampScalar<1, s16, sMaxScalar>,
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
    LegalFor<[s1]>,
  ];
}

def X86_PTRTOINT : LegalizeRuleSet<["G_PTRTOINT"]> {
  let Rules = [
    LegalForCartesianProduct<"Is64Bit ? PtrTypes64 : PtrTypes32",
                                     "{p0}">,
    MaxScalar<0, sMaxScalar>,
    WidenScalarToNextPow2<0, 8>,
  ];
}

def X86_INTTOPTR : LegalizeRuleSet<["G_INTTOPTR"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<p0, sMaxScalar>]>,
  ];
}

def X86_CONSTANT_POOL : LegalizeRuleSet<["G_CONSTANT_POOL"]> {
  let Rules = [
    LegalFor<[p0]>,
  ];
}

def X86_PTR_ADD : LegalizeRuleSet<["G_PTR_ADD"]> {
  let Rules = [
    LegalForPairs<[LegalizeTypePair<p0, s32>]>,
    LegalForPairsIf<Is64Bit, [LegalizeTypePair<p0, s64>]>,
    WidenScalarToNextPow2<1, 32>,
    ClampScalar<1, s32, sMaxScalar>,
  ];
}

def X86_FRAME_INDEX : LegalizeRuleSet<["G_FRAME_INDEX"]> {
  let Rules = [
    LegalFor<[p0]>,
  ];
}

def X86_GLOBAL_VALUE : LegalizeRuleSet<["G_GLOBAL_VALUE"]> {
  let Rules = [
    CustomFor<[p0]>,
  ];
}

def X86_LOAD : LegalizeRuleSet<["G_LOAD"]> {
  let Comments = ["load/store: add more corner cases"];
  let Rules = [
    LegalForMemDesc<[LegalizeMemDesc<s8, p0, s8, 1>, LegalizeMemDesc<s16, p0, s16, 1>,
                            LegalizeMemDesc<s32, p0, s32, 1>, LegalizeMemDesc<s80, p0, s80, 1>,
                            LegalizeMemDesc<p0, p0, p0, 1>, LegalizeMemDesc<v4s8, p0, v4s8, 1>]>,
    LegalForMemDescIf<Is64Bit,
                              [LegalizeMemDesc<s64, p0, s64, 1>, LegalizeMemDesc<v2s32, p0, v2s32, 1>]>,
    LegalForMemDescIf<HasSSE1, [LegalizeMemDesc<v4s32, p0, v4s32, 1>]>,
    LegalForMemDescIf<HasSSE2,
                              [LegalizeMemDesc<v16s8, p0, v16s8, 1>, LegalizeMemDesc<v8s16, p0, v8s16, 1>,
                               LegalizeMemDesc<v2s64, p0, v2s64, 1>, LegalizeMemDesc<v2p0, p0, v2p0, 1>]>,
    LegalForMemDescIf<HasAVX,
                              [LegalizeMemDesc<v32s8, p0, v32s8, 1>, LegalizeMemDesc<v16s16, p0, v16s16, 1>,
                               LegalizeMemDesc<v8s32, p0, v8s32, 1>, LegalizeMemDesc<v4s64, p0, v4s64, 1>,
                               LegalizeMemDesc<v4p0, p0, v4p0, 1>]>,
    LegalForMemDescIf<HasAVX512,
                              [LegalizeMemDesc<v64s8, p0, v64s8, 1>, LegalizeMemDesc<v32s16, p0, v32s16, 1>,
                               LegalizeMemDesc<v16s32, p0, v16s32, 1>, LegalizeMemDesc<v8s64, p0, v8s64, 1>]>,
    // X86 supports extending loads but not stores for GPRs.
    LegalForMemDesc<[LegalizeMemDesc<s8, p0, s1, 1>, LegalizeMemDesc<s16, p0, s8, 1>,
                            LegalizeMemDesc<s32, p0, s8, 1>, LegalizeMemDesc<s32, p0, s16, 1>]>,
    LegalForMemDescIf<Is64Bit,
                              [LegalizeMemDesc<s64, p0, s8, 1>, LegalizeMemDesc<s64, p0, s16, 1>,
                               LegalizeMemDesc<s64, p0, s32, 1>]>,
    WidenScalarToNextPow2<0, 8>,
    ClampScalar<0, s8, sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_STORE : LegalizeRuleSet<["G_STORE"]> {
  let Rules = [
    LegalForMemDesc<[LegalizeMemDesc<s8, p0, s8, 1>, LegalizeMemDesc<s16, p0, s16, 1>,
                            LegalizeMemDesc<s32, p0, s32, 1>, LegalizeMemDesc<s80, p0, s80, 1>,
                            LegalizeMemDesc<p0, p0, p0, 1>, LegalizeMemDesc<v4s8, p0, v4s8, 1>]>,
    LegalForMemDescIf<Is64Bit,
                              [LegalizeMemDesc<s64, p0, s64, 1>, LegalizeMemDesc<v2s32, p0, v2s32, 1>]>,
    LegalForMemDescIf<HasSSE1, [LegalizeMemDesc<v4s32, p0, v4s32, 1>]>,
    LegalForMemDescIf<HasSSE2,
                              [LegalizeMemDesc<v16s8, p0, v16s8, 1>, LegalizeMemDesc<v8s16, p0, v8s16, 1>,
                               LegalizeMemDesc<v2s64, p0, v2s64, 1>, LegalizeMemDesc<v2p0, p0, v2p0, 1>]>,
    LegalForMemDescIf<HasAVX,
                              [LegalizeMemDesc<v32s8, p0, v32s8, 1>, LegalizeMemDesc<v16s16, p0, v16s16, 1>,
                               LegalizeMemDesc<v8s32, p0, v8s32, 1>, LegalizeMemDesc<v4s64, p0, v4s64, 1>,
                               LegalizeMemDesc<v4p0, p0, v4p0, 1>]>,
    LegalForMemDescIf<HasAVX512,
                              [LegalizeMemDesc<v64s8, p0, v64s8, 1>, LegalizeMemDesc<v32s16, p0, v32s16, 1>,
                               LegalizeMemDesc<v16s32, p0, v16s32, 1>, LegalizeMemDesc<v8s64, p0, v8s64, 1>]>,
    LegalizeOpaqueRule<"customIf", [{

      [=](const LegalityQuery &Query) {
        return Query.Types[0] != Query.MMODescrs[0].MemoryTy;
      }
    }], ActionCustom>,
    WidenScalarToNextPow2<0, 8>,
    ClampScalar<0, s8, sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_SEXTLOAD : LegalizeRuleSet<["G_SEXTLOAD"]> {
  let Rules = [
    LegalForMemDesc<[LegalizeMemDesc<s16, p0, s8, 1>, LegalizeMemDesc<s32, p0, s8, 1>,
                            LegalizeMemDesc<s32, p0, s16, 1>]>,
    LegalForMemDescIf<Is64Bit,
                              [LegalizeMemDesc<s64, p0, s8, 1>, LegalizeMemDesc<s64, p0, s16, 1>,
                               LegalizeMemDesc<s64, p0, s32, 1>]>,
  ];
}

def X86_ZEXTLOAD : LegalizeRuleSet<["G_ZEXTLOAD"]> {
  let Rules = [
    LegalForMemDesc<[LegalizeMemDesc<s16, p0, s8, 1>, LegalizeMemDesc<s32, p0, s8, 1>,
                            LegalizeMemDesc<s32, p0, s16, 1>]>,
    LegalForMemDescIf<Is64Bit,
                              [LegalizeMemDesc<s64, p0, s8, 1>, LegalizeMemDesc<s64, p0, s16, 1>,
                               LegalizeMemDesc<s64, p0, s32, 1>]>,
  ];
}

def X86_FPEXTLOAD : LegalizeRuleSet<["G_FPEXTLOAD"]> {
  let Rules = [
    LegalForMemDescIf<UseX87,
                              [LegalizeMemDesc<s80, p0, s32, 1>, LegalizeMemDesc<s80, p0, s64, 1>,
                               LegalizeMemDesc<s64, p0, s32, 1>]>,
  ];
}

def X86_FPTRUNCSTORE : LegalizeRuleSet<["G_FPTRUNCSTORE"]> {
  let Rules = [
    LegalForMemDescIf<UseX87,
                              [LegalizeMemDesc<s80, p0, s32, 1>, LegalizeMemDesc<s80, p0, s64, 1>,
                               LegalizeMemDesc<s64, p0, s32, 1>]>,
  ];
}

def X86_ANYEXT : LegalizeRuleSet<["G_ANYEXT"]> {
  let Comments = ["sext, zext, and anyext"];
  let Rules = [
    LegalFor<[s8, s16, s32, s128]>,
    LegalForIf<Is64Bit, [s64]>,
    WidenScalarToNextPow2<0, 8>,
    ClampScalar<0, s8, sMaxScalar>,
    WidenScalarToNextPow2<1, 8>,
    ClampScalar<1, s8, sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_SEXT : LegalizeRuleSet<["G_SEXT", "G_ZEXT"]> {
  let Rules = [
    LegalFor<[s8, s16, s32]>,
    LegalForIf<Is64Bit, [s64]>,
    WidenScalarToNextPow2<0, 8>,
    ClampScalar<0, s8, sMaxScalar>,
    WidenScalarToNextPow2<1, 8>,
    ClampScalar<1, s8, sMaxScalar>,
    Scalarize<0>,
  ];
}

def X86_TRUNC : LegalizeRuleSet<["G_TRUNC"]> {
  let Rules = [
    LegalForCartesianProduct<"{s1, s8, s16, s32, s64}",
                                     "{s8, s16, s32, s64, s128}">,
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
    LegalFor<[s32, s64]>,
    LegalForIf<UseX87, [s80]>,
  ];
}

def X86_FADD : LegalizeRuleSet<["G_FADD", "G_FSUB", "G_FMUL", "G_FDIV"]> {
  let Comments = ["fp arithmetic"];
  let Rules = [
    LegalFor<[s32, s64]>,
    LegalForIf<HasSSE1, [v4s32]>,
    LegalForIf<HasSSE2, [v2s64]>,
    LegalForIf<HasAVX, [v8s32, v4s64]>,
    LegalForIf<HasAVX512, [v16s32, v8s64]>,
    LegalForIf<UseX87, [s80]>,
  ];
}

def X86_FABS : LegalizeRuleSet<["G_FABS"]> {
  let Rules = [
    LegalForIf<UseX87, [s80]>,
    LegalForIf<UseX87Not64Bit, [s64]>,
    Lower,
  ];
}

def X86_FCMP : LegalizeRuleSet<["G_FCMP"]> {
  let Comments = ["fp comparison"];
  let Rules = [
    LegalForIf<SSE1OrX87, [s8, s32]>,
    LegalForIf<SSE2OrX87, [s8, s64]>,
    LegalForIf<UseX87, [s8, s80]>,
    ClampScalar<0, s8, s8>,
    ClampScalar<1, s32, LegalizeTypeExpr<"HasSSE2 ? s64 : s32">>,
    WidenScalarToNextPow2<1>,
  ];
}

def X86_FPEXT : LegalizeRuleSet<["G_FPEXT"]> {
  let Comments = ["fp conversions"];
  let Rules = [
    LegalForPairsIf<HasSSE2, [LegalizeTypePair<s64, s32>]>,
    LegalForPairsIf<HasAVX, [LegalizeTypePair<v4s64, v4s32>]>,
    LegalForPairsIf<HasAVX512, [LegalizeTypePair<v8s64, v8s32>]>,
    LowerForPairsIf<UseX87, [LegalizeTypePair<s64, s32>, LegalizeTypePair<s80, s32>, LegalizeTypePair<s80, s64>]>,
    Libcall,
  ];
}

def X86_FPTRUNC : LegalizeRuleSet<["G_FPTRUNC"]> {
  let Rules = [
    LegalForPairsIf<HasSSE2, [LegalizeTypePair<s32, s64>]>,
    LegalForPairsIf<HasAVX, [LegalizeTypePair<v4s32, v4s64>]>,
    LegalForPairsIf<HasAVX512, [LegalizeTypePair<v8s32, v8s64>]>,
    LowerForPairsIf<UseX87, [LegalizeTypePair<s32, s64>, LegalizeTypePair<s32, s80>, LegalizeTypePair<s64, s80>]>,
  ];
}

def X86_SITOFP : LegalizeRuleSet<["G_SITOFP"]> {
  let Rules = [
    LegalForPairsIf<HasSSE1, [LegalizeTypePair<s32, s32>]>,
    LegalForPairsIf<SSE1And64Bit, [LegalizeTypePair<s32, s64>]>,
    LegalForPairsIf<HasSSE2, [LegalizeTypePair<s64, s32>]>,
    LegalForPairsIf<SSE2And64Bit, [LegalizeTypePair<s64, s64>]>,
    ClampScalar<1, LegalizeTypeExpr<"(UseX87 && !HasSSE1) ? s16 : s32">, sMaxScalar>,
    WidenScalarToNextPow2<1>,
    CustomForCartesianProductIf<UseX87, "{s32, s64, s80}",
                                      "{s16, s32, s64}">,
    ClampScalar<0, s32, LegalizeTypeExpr<"HasSSE2 ? s64 : s32">>,
    WidenScalarToNextPow2<0>,
  ];
}

def X86_FPTOSI : LegalizeRuleSet<["G_FPTOSI"]> {
  let Rules = [
    LegalForPairsIf<HasSSE1, [LegalizeTypePair<s32, s32>]>,
    LegalForPairsIf<SSE1And64Bit, [LegalizeTypePair<s64, s32>]>,
    LegalForPairsIf<HasSSE2, [LegalizeTypePair<s32, s64>]>,
    LegalForPairsIf<SSE2And64Bit, [LegalizeTypePair<s64, s64>]>,
    ClampScalar<0, LegalizeTypeExpr<"(UseX87 && !HasSSE1) ? s16 : s32">, sMaxScalar>,
    WidenScalarToNextPow2<0>,
    CustomForCartesianProductIf<UseX87, "{s16, s32, s64}",
                                      "{s32, s64, s80}">,
    ClampScalar<1, s32, LegalizeTypeExpr<"HasSSE2 ? s64 : s32">>,
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
                     [LegalizeTypePair<s32, s32>, LegalizeTypePair<s32, s64>, LegalizeTypePair<s64, s32>, LegalizeTypePair<s64, s64>]>,
    LegalizeOpaqueRule<"customIf", [{

      [=](const LegalityQuery &Query) {
              return !HasAVX512 &&
                     ((HasSSE1 && typeIs(0, s32)(Query)) ||
                      (HasSSE2 && typeIs(0, s64)(Query))) &&
                     scalarNarrowerThan(1, Is64Bit ? 64 : 32)(Query);
            }
    }], ActionCustom>,
    LegalizeOpaqueRule<"lowerIf", [{

      [=](const LegalityQuery &Query) {
              // Lower conversions from s64
              return !HasAVX512 &&
                     ((HasSSE1 && typeIs(0, s32)(Query)) ||
                      (HasSSE2 && typeIs(0, s64)(Query))) &&
                     (Is64Bit && typeIs(1, s64)(Query));
            }
    }], ActionLower>,
    ClampScalar<0, s32, LegalizeTypeExpr<"HasSSE2 ? s64 : s32">>,
    WidenScalarToNextPow2<0>,
    ClampScalar<1, s32, sMaxScalar>,
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
                     [LegalizeTypePair<s32, s32>, LegalizeTypePair<s32, s64>, LegalizeTypePair<s64, s32>, LegalizeTypePair<s64, s64>]>,
    LegalizeOpaqueRule<"customIf", [{

      [=](const LegalityQuery &Query) {
        return !HasAVX512 &&
               ((HasSSE1 && typeIs(1, s32)(Query)) ||
                (HasSSE2 && typeIs(1, s64)(Query))) &&
               scalarNarrowerThan(0, Is64Bit ? 64 : 32)(Query);
      }
    }], ActionCustom>,
    LegalizeOpaqueRule<"lowerIf", [{

      [=](const LegalityQuery &Query) {
        return !HasAVX512 &&
               ((HasSSE1 && typeIs(1, s32)(Query)) ||
                (HasSSE2 && typeIs(1, s64)(Query))) &&
               (Is64Bit && typeIs(0, s64)(Query));
      }
    }], ActionLower>,
    ClampScalar<0, s32, sMaxScalar>,
    WidenScalarToNextPow2<0>,
    ClampScalar<1, s32, LegalizeTypeExpr<"HasSSE2 ? s64 : s32">>,
    WidenScalarToNextPow2<1>,
  ];
}

def X86_BUILD_VECTOR : LegalizeRuleSet<["G_BUILD_VECTOR"]> {
  let Comments = ["vector ops"];
  let Rules = [
    LegalizeOpaqueRule<"customIf", [{

      [=](const LegalityQuery &Query) {
              return (HasSSE1 && typeInSet(0, {v4s32})(Query)) ||
                     (HasSSE2 && typeInSet(0, {v2s64, v8s16, v16s8})(Query)) ||
                     (HasAVX &&
                      typeInSet(0, {v4s64, v8s32, v16s16, v32s8})(Query)) ||
                     (HasAVX512 &&
                      typeInSet(0, {v8s64, v16s32, v32s16, v64s8})(Query));
            }
    }], ActionCustom>,
    ClampNumElements<0, v16s8, s8MaxVector>,
    ClampNumElements<0, v8s16, s16MaxVector>,
    ClampNumElements<0, v4s32, s32MaxVector>,
    ClampNumElements<0, v2s64, s64MaxVector>,
    MoreElementsToNextPow2<0>,
  ];
}

def X86_EXTRACT : LegalizeRuleSet<["G_EXTRACT", "G_INSERT"]> {
  let Rules = [
    LegalizeOpaqueRule<"legalIf", [{

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
    }], ActionLegal>,
  ];
}

def X86_CONCAT_VECTORS : LegalizeRuleSet<["G_CONCAT_VECTORS"]> {
  let Comments = [
    "todo: only permit dst types up to max legal vector register size?",
  ];
  let Rules = [
    LegalForPairsIf<HasSSE1,
                     [LegalizeTypePair<v32s8, v16s8>, LegalizeTypePair<v16s16, v8s16>,
                      LegalizeTypePair<v8s32, v4s32>, LegalizeTypePair<v4s64, v2s64>]>,
    LegalForPairsIf<HasAVX,
                     [LegalizeTypePair<v64s8, v16s8>, LegalizeTypePair<v64s8, v32s8>, LegalizeTypePair<v32s16, v8s16>,
                      LegalizeTypePair<v32s16, v16s16>, LegalizeTypePair<v16s32, v4s32>, LegalizeTypePair<v16s32, v8s32>,
                      LegalizeTypePair<v8s64, v2s64>, LegalizeTypePair<v8s64, v4s64>]>,
  ];
}

def X86_SELECT : LegalizeRuleSet<["G_SELECT"]> {
  let Comments = ["todo: vectors and address spaces"];
  let Rules = [
    LegalForPairs<[LegalizeTypePair<s16, s32>, LegalizeTypePair<s32, s32>, LegalizeTypePair<p0, s32>]>,
    LegalForPairsIf<NotCMOV, [LegalizeTypePair<s8, s32>]>,
    LegalForPairsIf<Is64Bit, [LegalizeTypePair<s64, s32>]>,
    LegalForPairsIf<UseX87, [LegalizeTypePair<s80, s32>]>,
    ClampScalar<1, s32, s32>,
    WidenScalarToNextPow2<0, 8>,
    ClampScalar<0, LegalizeTypeExpr<"HasCMOV ? s16 : s8">, sMaxScalar>,
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
    LowerForPairsIf<Is64Bit, [LegalizeTypePair<s1, s32>, LegalizeTypePair<s1, s64>, LegalizeTypePair<s1, s80>]>,
  ];
}

def X86_INTRINSIC_ROUNDEVEN : LegalizeRuleSet<[
  "G_INTRINSIC_ROUNDEVEN", "G_INTRINSIC_TRUNC"
]> {
  let Rules = [
    Scalarize<0>,
    MinScalar<0, LegalizeTypeExpr<"LLT::scalar(32)">>,
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
