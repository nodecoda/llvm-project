/*===- TableGen'erated file -------------------------------------*- C++ -*-===*\
|*                                                                            *|
|* GlobalISel legalizer rule fragments                                        *|
|*                                                                            *|
|* Automatically generated file, do not edit!                                 *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

  bool X_HasAVX = Subtarget.hasAVX();
  bool X_HasAVX2 = Subtarget.hasAVX2();
  bool X_HasAVX512 = Subtarget.hasAVX512();
  bool X_HasBITALG = Subtarget.hasBITALG();
  bool X_HasBMI = Subtarget.hasBMI();
  bool X_HasBWI = Subtarget.hasAVX512() && Subtarget.hasBWI();
  bool X_HasCMOV = Subtarget.canUseCMOV();
  bool X_HasDQI = Subtarget.hasAVX512() && Subtarget.hasDQI();
  bool X_HasLZCNT = Subtarget.hasLZCNT();
  bool X_HasPOPCNT = Subtarget.hasPOPCNT();
  bool X_HasSSE1 = Subtarget.hasSSE1();
  bool X_HasSSE2 = Subtarget.hasSSE2();
  bool X_HasSSE41 = Subtarget.hasSSE41();
  bool X_HasVLX = Subtarget.hasVLX();
  bool X_HasVPOPCNTDQ = Subtarget.hasVPOPCNTDQ();
  bool X_Is64Bit = Subtarget.is64Bit();
  bool X_UseX87 = !Subtarget.useSoftFloat() && Subtarget.hasX87();

  const LLT X_p0 = LLT::pointer(0, Subtarget.isTarget64BitLP64() ? 64 : 32);
  const LLT X_s1 = LLT::scalar(1);
  const LLT X_s8 = LLT::scalar(8);
  const LLT X_s16 = LLT::scalar(16);
  const LLT X_s32 = LLT::scalar(32);
  const LLT X_s64 = LLT::scalar(64);
  const LLT X_s80 = LLT::scalar(80);
  const LLT X_s128 = LLT::scalar(128);
  const LLT X_v2s32 = LLT::fixed_vector(2, 32);
  const LLT X_v4s8 = LLT::fixed_vector(4, 8);
  const LLT X_v16s8 = LLT::fixed_vector(16, 8);
  const LLT X_v8s16 = LLT::fixed_vector(8, 16);
  const LLT X_v4s32 = LLT::fixed_vector(4, 32);
  const LLT X_v2s64 = LLT::fixed_vector(2, 64);
  const LLT X_v2p0 = LLT::fixed_vector(2, X_p0);
  const LLT X_v32s8 = LLT::fixed_vector(32, 8);
  const LLT X_v16s16 = LLT::fixed_vector(16, 16);
  const LLT X_v8s32 = LLT::fixed_vector(8, 32);
  const LLT X_v4s64 = LLT::fixed_vector(4, 64);
  const LLT X_v4p0 = LLT::fixed_vector(4, X_p0);
  const LLT X_v64s8 = LLT::fixed_vector(64, 8);
  const LLT X_v32s16 = LLT::fixed_vector(32, 16);
  const LLT X_v16s32 = LLT::fixed_vector(16, 32);
  const LLT X_v8s64 = LLT::fixed_vector(8, 64);
  const LLT X_sMaxScalar = (X_Is64Bit ? X_s64 : X_s32);
  const LLT X_s8MaxVectorNoAVX512 = (X_HasAVX ? X_v32s8 : X_v16s8);
  const LLT X_s8MaxVector = (X_HasAVX512 ? X_v64s8 : X_s8MaxVectorNoAVX512);
  const LLT X_s16MaxVectorNoAVX512 = (X_HasAVX ? X_v16s16 : X_v8s16);
  const LLT X_s16MaxVector = (X_HasAVX512 ? X_v32s16 : X_s16MaxVectorNoAVX512);
  const LLT X_s32MaxVectorNoAVX512 = (X_HasAVX ? X_v8s32 : X_v4s32);
  const LLT X_s32MaxVector = (X_HasAVX512 ? X_v16s32 : X_s32MaxVectorNoAVX512);
  const LLT X_s64MaxVectorNoAVX512 = (X_HasAVX ? X_v4s64 : X_v2s64);
  const LLT X_s64MaxVector = (X_HasAVX512 ? X_v8s64 : X_s64MaxVectorNoAVX512);
  const LLT X_sse2MaxScalar = (X_HasSSE2 ? X_s64 : X_s32);
  const LLT X_x87MinScalar = (X_UseX87 && !(X_HasSSE1) ? X_s16 : X_s32);
  const LLT X_cmovMinScalar = (X_HasCMOV ? X_s16 : X_s8);

  getActionDefinitionsBuilder({G_IMPLICIT_DEF, G_PHI, G_FREEZE, G_CONSTANT_FOLD_BARRIER})
      .legalFor({X_p0, X_s1, X_s8, X_s16, X_s32, X_s64})
      .legalFor(X_UseX87, {X_s80})
      .legalFor(X_Is64Bit, {X_s128})
      .legalFor(X_HasSSE2, {X_v16s8, X_v8s16, X_v4s32, X_v2s64})
      .legalFor(X_HasAVX, {X_v32s8, X_v16s16, X_v8s32, X_v4s64})
      .legalFor(X_HasAVX512, {X_v64s8, X_v32s16, X_v16s32, X_v8s64})
      .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 0, 8)
      .coreMinScalarOrElt([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalarOrElt([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .clampMinNumElements(0, X_s8, (X_v16s8).getNumElements())
      .clampMaxNumElements(0, X_s8, (X_s8MaxVector).getNumElements())
      .clampMinNumElements(0, X_s16, (X_v8s16).getNumElements())
      .clampMaxNumElements(0, X_s16, (X_s16MaxVector).getNumElements())
      .clampMinNumElements(0, X_s32, (X_v4s32).getNumElements())
      .clampMaxNumElements(0, X_s32, (X_s32MaxVector).getNumElements())
      .clampMinNumElements(0, X_s64, (X_v2s64).getNumElements())
      .clampMaxNumElements(0, X_s64, (X_s64MaxVector).getNumElements())
      .clampMaxNumElements(0, X_p0, (X_Is64Bit ? (X_s64MaxVector).getNumElements() : (X_s32MaxVector).getNumElements()))
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0);
  getActionDefinitionsBuilder(G_CONSTANT)
      .legalFor({X_p0, X_s8, X_s16, X_s32})
      .legalFor(X_Is64Bit, {X_s64})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar);
  getActionDefinitionsBuilder({G_LROUND, G_LLROUND})
      .coreWidenTo([=](const LegalityQuery &Query) { return (Query.Types[1] == X_s16); }, 1, [=](const LegalityQuery &Query) { return std::make_pair(1, X_s32); })
      .libcall();
  getActionDefinitionsBuilder({G_FCOS, G_FCOSH, G_FACOS, G_FSIN, G_FSINH, G_FASIN, G_FTAN, G_FTANH, G_FATAN, G_FATAN2, G_FPOW, G_FEXP, G_FEXP2, G_FEXP10, G_FLOG, G_FLOG2, G_FLOG10, G_FPOWI, G_FSINCOS, G_FCEIL, G_FFLOOR})
      .libcall();
  if (X_UseX87) {
    if (!X_HasSSE1) {
      getActionDefinitionsBuilder(G_FNEG)
          .legalFor({X_s32});
    }
    if (!X_HasSSE2) {
      getActionDefinitionsBuilder(G_FNEG)
          .legalFor({X_s64});
    }
    getActionDefinitionsBuilder(G_FNEG)
        .legalFor({X_s80});
  }
  getActionDefinitionsBuilder(G_FNEG)
      .lower();
  if ((X_HasSSE1 || X_UseX87)) {
    getActionDefinitionsBuilder(G_FSQRT)
        .legalFor({X_s32});
  }
  if ((X_HasSSE2 || X_UseX87)) {
    getActionDefinitionsBuilder(G_FSQRT)
        .legalFor({X_s64});
  }
  if (X_UseX87) {
    getActionDefinitionsBuilder(G_FSQRT)
        .legalFor({X_s80});
  }
  getActionDefinitionsBuilder({G_GET_ROUNDING, G_SET_ROUNDING})
      .customFor({X_s32});
  getActionDefinitionsBuilder(G_MERGE_VALUES)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 8)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 16)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s32)
      .coreLegalIf([=](const LegalityQuery &Query) { return ((((Query.Types[0]).getSizeInBits().getKnownMinValue() == 16)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 32)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 64)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 128)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 256)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 512))) && ((((Query.Types[1]).getSizeInBits().getKnownMinValue() == 8)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 16)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 32)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 64)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 128)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 256))); });
  getActionDefinitionsBuilder(G_UNMERGE_VALUES)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 8)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 16)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s32)
      .coreLegalIf([=](const LegalityQuery &Query) { return ((((Query.Types[1]).getSizeInBits().getKnownMinValue() == 16)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 32)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 64)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 128)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 256)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 512))) && ((((Query.Types[0]).getSizeInBits().getKnownMinValue() == 8)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 16)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 32)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 64)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 128)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 256))); });
  getActionDefinitionsBuilder({G_UMIN, G_UMAX, G_SMIN, G_SMAX})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 32)
      .lower();
  getActionDefinitionsBuilder({G_ADD, G_SUB})
      .legalFor({X_s8, X_s16, X_s32})
      .legalFor(X_Is64Bit, {X_s64})
      .legalFor(X_HasSSE2, {X_v16s8, X_v8s16, X_v4s32, X_v2s64})
      .legalFor(X_HasAVX2, {X_v32s8, X_v16s16, X_v8s32, X_v4s64})
      .legalFor(X_HasAVX512, {X_v16s32, X_v8s64})
      .legalFor(X_HasBWI, {X_v64s8, X_v32s16})
      .clampMinNumElements(0, X_s8, 16)
      .clampMinNumElements(0, X_s16, 8)
      .clampMinNumElements(0, X_s32, 4)
      .clampMinNumElements(0, X_s64, 2)
      .clampMaxNumElements(0, X_s8, (X_HasBWI ? 64 : (X_HasAVX2 ? 32 : 16)))
      .clampMaxNumElements(0, X_s16, (X_HasBWI ? 32 : (X_HasAVX2 ? 16 : 8)))
      .clampMaxNumElements(0, X_s32, (X_HasAVX512 ? 16 : (X_HasAVX2 ? 8 : 4)))
      .clampMaxNumElements(0, X_s64, (X_HasAVX512 ? 8 : (X_HasAVX2 ? 4 : 2)))
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 32)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder({G_UADDE, G_UADDO, G_USUBE, G_USUBO})
      .legalFor({{X_s8, X_s8}, {X_s16, X_s8}, {X_s32, X_s8}})
      .legalFor(X_Is64Bit, {{X_s64, X_s8}})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 32)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_s8)
      .scalarize(0);
  getActionDefinitionsBuilder(G_MUL)
      .legalFor({X_s8, X_s16, X_s32})
      .legalFor(X_Is64Bit, {X_s64})
      .legalFor(X_HasSSE2, {X_v8s16})
      .legalFor(X_HasSSE41, {X_v4s32})
      .legalFor(X_HasAVX2, {X_v16s16, X_v8s32})
      .legalFor(X_HasAVX512, {X_v16s32})
      .legalFor(X_HasDQI, {X_v8s64})
      .legalFor((X_HasDQI && X_HasVLX), {X_v2s64, X_v4s64})
      .legalFor(X_HasBWI, {X_v32s16})
      .clampMinNumElements(0, X_s16, 8)
      .clampMinNumElements(0, X_s32, 4)
      .clampMinNumElements(0, X_s64, (X_HasVLX ? 2 : 8))
      .clampMaxNumElements(0, X_s16, (X_HasBWI ? 32 : (X_HasAVX2 ? 16 : 8)))
      .clampMaxNumElements(0, X_s32, (X_HasAVX512 ? 16 : (X_HasAVX2 ? 8 : 4)))
      .clampMaxNumElements(0, X_s64, 8)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 32)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder({G_SMULH, G_UMULH})
      .legalFor({X_s8, X_s16, X_s32})
      .legalFor(X_Is64Bit, {X_s64})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 32)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder({G_SDIV, G_SREM, G_UDIV, G_UREM})
      .legalFor({X_s8, X_s16, X_s32})
      .legalFor(X_Is64Bit, {X_s64})
      .libcallFor({X_s64})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar);
  getActionDefinitionsBuilder({G_SHL, G_LSHR, G_ASHR})
      .legalFor({{X_s8, X_s8}, {X_s16, X_s8}, {X_s32, X_s8}})
      .legalFor(X_Is64Bit, {{X_s64, X_s8}})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_s8);
  getActionDefinitionsBuilder({G_AND, G_OR, G_XOR})
      .legalFor({X_s8, X_s16, X_s32})
      .legalFor(X_Is64Bit, {X_s64})
      .legalFor(X_HasSSE2, {X_v16s8, X_v8s16, X_v4s32, X_v2s64})
      .legalFor(X_HasAVX, {X_v32s8, X_v16s16, X_v8s32, X_v4s64})
      .legalFor(X_HasAVX512, {X_v64s8, X_v32s16, X_v16s32, X_v8s64})
      .clampMinNumElements(0, X_s8, (X_v16s8).getNumElements())
      .clampMaxNumElements(0, X_s8, (X_s8MaxVector).getNumElements())
      .clampMinNumElements(0, X_s16, (X_v8s16).getNumElements())
      .clampMaxNumElements(0, X_s16, (X_s16MaxVector).getNumElements())
      .clampMinNumElements(0, X_s32, (X_v4s32).getNumElements())
      .clampMaxNumElements(0, X_s32, (X_s32MaxVector).getNumElements())
      .clampMinNumElements(0, X_s64, (X_v2s64).getNumElements())
      .clampMaxNumElements(0, X_s64, (X_s64MaxVector).getNumElements())
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 32)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .scalarize(0);
  if (X_Is64Bit) {
    getActionDefinitionsBuilder(G_ICMP)
        .legalFor({{X_s8, X_s8}, {X_s8, X_s16}, {X_s8, X_s32}, {X_s8, X_s64}, {X_s8, X_p0}});
  } else {
    if (!X_Is64Bit) {
      getActionDefinitionsBuilder(G_ICMP)
          .legalFor({{X_s8, X_s8}, {X_s8, X_s16}, {X_s8, X_s32}, {X_s8, X_p0}});
    }
  }
  getActionDefinitionsBuilder(G_ICMP)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_sMaxScalar);
  getActionDefinitionsBuilder(G_BSWAP)
      .legalFor({X_s32})
      .legalFor(X_Is64Bit, {X_s64})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 32)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar);
  if (X_HasPOPCNT) {
    getActionDefinitionsBuilder(G_CTPOP)
        .legalFor({{X_s16, X_s16}, {X_s32, X_s32}})
        .legalFor(X_Is64Bit, {{X_s64, X_s64}});
  }
  if (X_HasVPOPCNTDQ) {
    getActionDefinitionsBuilder(G_CTPOP)
        .legalFor({{X_v4s32, X_v4s32}, {X_v8s32, X_v8s32}, {X_v16s32, X_v16s32}, {X_v2s64, X_v2s64}, {X_v4s64, X_v4s64}, {X_v8s64, X_v8s64}});
  }
  if (X_HasBITALG) {
    getActionDefinitionsBuilder(G_CTPOP)
        .legalFor({{X_v16s8, X_v16s8}, {X_v32s8, X_v32s8}, {X_v64s8, X_v64s8}, {X_v8s16, X_v8s16}, {X_v16s16, X_v16s16}, {X_v32s16, X_v32s16}});
  }
  getActionDefinitionsBuilder(G_CTPOP)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 16)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s16)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_sMaxScalar)
      .minScalarSameAs(0, 1)
      .maxScalarSameAs(0, 1);
  if (X_HasLZCNT) {
    getActionDefinitionsBuilder(G_CTLZ)
        .legalFor({{X_s16, X_s16}, {X_s32, X_s32}})
        .legalFor(X_Is64Bit, {{X_s64, X_s64}});
  }
  getActionDefinitionsBuilder(G_CTLZ)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 16)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s16)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_sMaxScalar)
      .minScalarSameAs(0, 1)
      .maxScalarSameAs(0, 1);
  getActionDefinitionsBuilder(G_CTTZ_ZERO_POISON)
      .legalFor({{X_s16, X_s16}, {X_s32, X_s32}})
      .legalFor(X_Is64Bit, {{X_s64, X_s64}})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 16)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s16)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_sMaxScalar)
      .minScalarSameAs(0, 1)
      .maxScalarSameAs(0, 1);
  if (X_HasBMI) {
    getActionDefinitionsBuilder(G_CTTZ)
        .legalFor({{X_s16, X_s16}, {X_s32, X_s32}})
        .legalFor(X_Is64Bit, {{X_s64, X_s64}});
  }
  getActionDefinitionsBuilder(G_CTTZ)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 16)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s16)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_sMaxScalar)
      .minScalarSameAs(0, 1)
      .maxScalarSameAs(0, 1);
  getActionDefinitionsBuilder(G_BR)
      .alwaysLegal();
  getActionDefinitionsBuilder(G_BRCOND)
      .legalFor({X_s1});
  getActionDefinitionsBuilder(G_PTRTOINT)
      .legalFor(X_Is64Bit, {{X_s1, X_p0}, {X_s8, X_p0}, {X_s16, X_p0}, {X_s32, X_p0}, {X_s64, X_p0}})
      .legalFor(!X_Is64Bit, {{X_s1, X_p0}, {X_s8, X_p0}, {X_s16, X_p0}, {X_s32, X_p0}})
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 8);
  getActionDefinitionsBuilder(G_INTTOPTR)
      .legalFor({{X_p0, X_sMaxScalar}});
  getActionDefinitionsBuilder(G_CONSTANT_POOL)
      .legalFor({X_p0});
  getActionDefinitionsBuilder(G_PTR_ADD)
      .legalFor({{X_p0, X_s32}})
      .legalFor(X_Is64Bit, {{X_p0, X_s64}})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 32)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_sMaxScalar);
  getActionDefinitionsBuilder(G_FRAME_INDEX)
      .legalFor({X_p0});
  getActionDefinitionsBuilder(G_GLOBAL_VALUE)
      .customFor({X_p0});
  getActionDefinitionsBuilder(G_LOAD)
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_s8, X_p0, X_s8, 1}, {X_s16, X_p0, X_s16, 1}, {X_s32, X_p0, X_s32, 1}, {X_s80, X_p0, X_s80, 1}, {X_p0, X_p0, X_p0, 1}, {X_v4s8, X_p0, X_v4s8, 1}})(Query); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (X_Is64Bit) && (LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_s64, X_p0, X_s64, 1}, {X_v2s32, X_p0, X_v2s32, 1}})(Query)); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (X_HasSSE1) && (LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_v4s32, X_p0, X_v4s32, 1}})(Query)); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (X_HasSSE2) && (LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_v16s8, X_p0, X_v16s8, 1}, {X_v8s16, X_p0, X_v8s16, 1}, {X_v2s64, X_p0, X_v2s64, 1}, {X_v2p0, X_p0, X_v2p0, 1}})(Query)); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (X_HasAVX) && (LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_v32s8, X_p0, X_v32s8, 1}, {X_v16s16, X_p0, X_v16s16, 1}, {X_v8s32, X_p0, X_v8s32, 1}, {X_v4s64, X_p0, X_v4s64, 1}, {X_v4p0, X_p0, X_v4p0, 1}})(Query)); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (X_HasAVX512) && (LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_v64s8, X_p0, X_v64s8, 1}, {X_v32s16, X_p0, X_v32s16, 1}, {X_v16s32, X_p0, X_v16s32, 1}, {X_v8s64, X_p0, X_v8s64, 1}})(Query)); })
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_s8, X_p0, X_s1, 1}, {X_s16, X_p0, X_s8, 1}, {X_s32, X_p0, X_s8, 1}, {X_s32, X_p0, X_s16, 1}})(Query); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (X_Is64Bit) && (LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_s64, X_p0, X_s8, 1}, {X_s64, X_p0, X_s16, 1}, {X_s64, X_p0, X_s32, 1}})(Query)); })
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder(G_STORE)
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_s8, X_p0, X_s8, 1}, {X_s16, X_p0, X_s16, 1}, {X_s32, X_p0, X_s32, 1}, {X_s80, X_p0, X_s80, 1}, {X_p0, X_p0, X_p0, 1}, {X_v4s8, X_p0, X_v4s8, 1}})(Query); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (X_Is64Bit) && (LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_s64, X_p0, X_s64, 1}, {X_v2s32, X_p0, X_v2s32, 1}})(Query)); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (X_HasSSE1) && (LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_v4s32, X_p0, X_v4s32, 1}})(Query)); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (X_HasSSE2) && (LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_v16s8, X_p0, X_v16s8, 1}, {X_v8s16, X_p0, X_v8s16, 1}, {X_v2s64, X_p0, X_v2s64, 1}, {X_v2p0, X_p0, X_v2p0, 1}})(Query)); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (X_HasAVX) && (LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_v32s8, X_p0, X_v32s8, 1}, {X_v16s16, X_p0, X_v16s16, 1}, {X_v8s32, X_p0, X_v8s32, 1}, {X_v4s64, X_p0, X_v4s64, 1}, {X_v4p0, X_p0, X_v4p0, 1}})(Query)); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (X_HasAVX512) && (LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_v64s8, X_p0, X_v64s8, 1}, {X_v32s16, X_p0, X_v32s16, 1}, {X_v16s32, X_p0, X_v16s32, 1}, {X_v8s64, X_p0, X_v8s64, 1}})(Query)); })
      .coreCustomIf([=](const LegalityQuery &Query) { return (Query.Types[0] != (Query.MMODescrs[0]).MemoryTy); })
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder(G_SEXTLOAD)
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_s16, X_p0, X_s8, 1}, {X_s32, X_p0, X_s8, 1}, {X_s32, X_p0, X_s16, 1}})(Query); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (X_Is64Bit) && (LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_s64, X_p0, X_s8, 1}, {X_s64, X_p0, X_s16, 1}, {X_s64, X_p0, X_s32, 1}})(Query)); });
  getActionDefinitionsBuilder(G_ZEXTLOAD)
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_s16, X_p0, X_s8, 1}, {X_s32, X_p0, X_s8, 1}, {X_s32, X_p0, X_s16, 1}})(Query); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (X_Is64Bit) && (LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_s64, X_p0, X_s8, 1}, {X_s64, X_p0, X_s16, 1}, {X_s64, X_p0, X_s32, 1}})(Query)); });
  if (X_UseX87) {
    getActionDefinitionsBuilder(G_FPEXTLOAD)
        .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_s80, X_p0, X_s32, 1}, {X_s80, X_p0, X_s64, 1}, {X_s64, X_p0, X_s32, 1}})(Query); });
  }
  if (X_UseX87) {
    getActionDefinitionsBuilder(G_FPTRUNCSTORE)
        .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{X_s80, X_p0, X_s32, 1}, {X_s80, X_p0, X_s64, 1}, {X_s64, X_p0, X_s32, 1}})(Query); });
  }
  getActionDefinitionsBuilder(G_ANYEXT)
      .coreLegalIf([=](const LegalityQuery &Query) { return (Query.Types[0] == X_s8 || Query.Types[0] == X_s16 || Query.Types[0] == X_s32 || Query.Types[0] == X_s128); })
      .legalFor(X_Is64Bit, {X_s64})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder({G_SEXT, G_ZEXT})
      .coreLegalIf([=](const LegalityQuery &Query) { return (Query.Types[0] == X_s8 || Query.Types[0] == X_s16 || Query.Types[0] == X_s32); })
      .legalFor(X_Is64Bit, {X_s64})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder(G_TRUNC)
      .legalFor({{X_s1, X_s8}, {X_s1, X_s16}, {X_s1, X_s32}, {X_s1, X_s64}, {X_s1, X_s128}, {X_s8, X_s8}, {X_s8, X_s16}, {X_s8, X_s32}, {X_s8, X_s64}, {X_s8, X_s128}, {X_s16, X_s8}, {X_s16, X_s16}, {X_s16, X_s32}, {X_s16, X_s64}, {X_s16, X_s128}, {X_s32, X_s8}, {X_s32, X_s16}, {X_s32, X_s32}, {X_s32, X_s64}, {X_s32, X_s128}, {X_s64, X_s8}, {X_s64, X_s16}, {X_s64, X_s32}, {X_s64, X_s64}, {X_s64, X_s128}});
  getActionDefinitionsBuilder(G_SEXT_INREG)
      .lower();
  getActionDefinitionsBuilder(G_FCONSTANT)
      .legalFor({X_s32, X_s64})
      .legalFor(X_UseX87, {X_s80});
  getActionDefinitionsBuilder({G_FADD, G_FSUB, G_FMUL, G_FDIV})
      .legalFor({X_s32, X_s64})
      .legalFor(X_HasSSE1, {X_v4s32})
      .legalFor(X_HasSSE2, {X_v2s64})
      .legalFor(X_HasAVX, {X_v8s32, X_v4s64})
      .legalFor(X_HasAVX512, {X_v16s32, X_v8s64})
      .legalFor(X_UseX87, {X_s80});
  if (X_UseX87) {
    getActionDefinitionsBuilder(G_FABS)
        .legalFor({X_s80})
        .legalFor(!X_Is64Bit, {X_s64});
  }
  getActionDefinitionsBuilder(G_FABS)
      .lower();
  if ((X_HasSSE1 || X_UseX87)) {
    getActionDefinitionsBuilder(G_FCMP)
        .coreLegalIf([=](const LegalityQuery &Query) { return (Query.Types[0] == X_s8 || Query.Types[0] == X_s32); });
  }
  if ((X_HasSSE2 || X_UseX87)) {
    getActionDefinitionsBuilder(G_FCMP)
        .coreLegalIf([=](const LegalityQuery &Query) { return (Query.Types[0] == X_s8 || Query.Types[0] == X_s64); });
  }
  if (X_UseX87) {
    getActionDefinitionsBuilder(G_FCMP)
        .coreLegalIf([=](const LegalityQuery &Query) { return (Query.Types[0] == X_s8 || Query.Types[0] == X_s80); });
  }
  getActionDefinitionsBuilder(G_FCMP)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_s8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_sse2MaxScalar)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1);
  if (X_HasSSE2) {
    getActionDefinitionsBuilder(G_FPEXT)
        .legalFor({{X_s64, X_s32}});
  }
  if (X_HasAVX) {
    getActionDefinitionsBuilder(G_FPEXT)
        .legalFor({{X_v4s64, X_v4s32}});
  }
  if (X_HasAVX512) {
    getActionDefinitionsBuilder(G_FPEXT)
        .legalFor({{X_v8s64, X_v8s32}});
  }
  if (X_UseX87) {
    getActionDefinitionsBuilder(G_FPEXT)
        .lowerFor({{X_s64, X_s32}, {X_s80, X_s32}, {X_s80, X_s64}});
  }
  getActionDefinitionsBuilder(G_FPEXT)
      .libcall();
  if (X_HasSSE2) {
    getActionDefinitionsBuilder(G_FPTRUNC)
        .legalFor({{X_s32, X_s64}});
  }
  if (X_HasAVX) {
    getActionDefinitionsBuilder(G_FPTRUNC)
        .legalFor({{X_v4s32, X_v4s64}});
  }
  if (X_HasAVX512) {
    getActionDefinitionsBuilder(G_FPTRUNC)
        .legalFor({{X_v8s32, X_v8s64}});
  }
  if (X_UseX87) {
    getActionDefinitionsBuilder(G_FPTRUNC)
        .lowerFor({{X_s32, X_s64}, {X_s32, X_s80}, {X_s64, X_s80}});
  }
  if (X_HasSSE1) {
    getActionDefinitionsBuilder(G_SITOFP)
        .legalFor({{X_s32, X_s32}})
        .legalFor(X_Is64Bit, {{X_s32, X_s64}});
  }
  if (X_HasSSE2) {
    getActionDefinitionsBuilder(G_SITOFP)
        .legalFor({{X_s64, X_s32}})
        .legalFor(X_Is64Bit, {{X_s64, X_s64}});
  }
  getActionDefinitionsBuilder(G_SITOFP)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_x87MinScalar)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_sMaxScalar)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .customFor(X_UseX87, {{X_s32, X_s16}, {X_s32, X_s32}, {X_s32, X_s64}, {X_s64, X_s16}, {X_s64, X_s32}, {X_s64, X_s64}, {X_s80, X_s16}, {X_s80, X_s32}, {X_s80, X_s64}})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sse2MaxScalar)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0);
  if (X_HasSSE1) {
    getActionDefinitionsBuilder(G_FPTOSI)
        .legalFor({{X_s32, X_s32}})
        .legalFor(X_Is64Bit, {{X_s64, X_s32}});
  }
  if (X_HasSSE2) {
    getActionDefinitionsBuilder(G_FPTOSI)
        .legalFor({{X_s32, X_s64}})
        .legalFor(X_Is64Bit, {{X_s64, X_s64}});
  }
  getActionDefinitionsBuilder(G_FPTOSI)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_x87MinScalar)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .customFor(X_UseX87, {{X_s16, X_s32}, {X_s16, X_s64}, {X_s16, X_s80}, {X_s32, X_s32}, {X_s32, X_s64}, {X_s32, X_s80}, {X_s64, X_s32}, {X_s64, X_s64}, {X_s64, X_s80}})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_sse2MaxScalar)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1);
  if (X_HasAVX512) {
    getActionDefinitionsBuilder(G_UITOFP)
        .legalFor({{X_s32, X_s32}, {X_s32, X_s64}, {X_s64, X_s32}, {X_s64, X_s64}});
  } else {
    if (!X_HasAVX512) {
      getActionDefinitionsBuilder(G_UITOFP)
          .coreCustomIf([=](const LegalityQuery &Query) { return (((X_HasSSE1) && ((Query.Types[0] == X_s32))) || ((X_HasSSE2) && ((Query.Types[0] == X_s64)))) && ((Query.Types[1].isScalar()) && (((Query.Types[1]).getSizeInBits().getKnownMinValue() < (X_Is64Bit ? 64 : 32)))); })
          .coreLowerIf([=](const LegalityQuery &Query) { return (((X_HasSSE1) && ((Query.Types[0] == X_s32))) || ((X_HasSSE2) && ((Query.Types[0] == X_s64)))) && ((X_Is64Bit) && ((Query.Types[1] == X_s64))); });
    }
  }
  getActionDefinitionsBuilder(G_UITOFP)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sse2MaxScalar)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_sMaxScalar)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1);
  if (X_HasAVX512) {
    getActionDefinitionsBuilder(G_FPTOUI)
        .legalFor({{X_s32, X_s32}, {X_s32, X_s64}, {X_s64, X_s32}, {X_s64, X_s64}});
  } else {
    if (!X_HasAVX512) {
      getActionDefinitionsBuilder(G_FPTOUI)
          .coreCustomIf([=](const LegalityQuery &Query) { return (((X_HasSSE1) && ((Query.Types[1] == X_s32))) || ((X_HasSSE2) && ((Query.Types[1] == X_s64)))) && ((Query.Types[0].isScalar()) && (((Query.Types[0]).getSizeInBits().getKnownMinValue() < (X_Is64Bit ? 64 : 32)))); })
          .coreLowerIf([=](const LegalityQuery &Query) { return (((X_HasSSE1) && ((Query.Types[1] == X_s32))) || ((X_HasSSE2) && ((Query.Types[1] == X_s64)))) && ((X_Is64Bit) && ((Query.Types[0] == X_s64))); });
    }
  }
  getActionDefinitionsBuilder(G_FPTOUI)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_sse2MaxScalar)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1);
  getActionDefinitionsBuilder(G_BUILD_VECTOR)
      .coreCustomIf([=](const LegalityQuery &Query) { return ((X_HasSSE1) && ((Query.Types[0] == X_v4s32))) || ((X_HasSSE2) && ((Query.Types[0] == X_v2s64 || Query.Types[0] == X_v8s16 || Query.Types[0] == X_v16s8))) || ((X_HasAVX) && ((Query.Types[0] == X_v4s64 || Query.Types[0] == X_v8s32 || Query.Types[0] == X_v16s16 || Query.Types[0] == X_v32s8))) || ((X_HasAVX512) && ((Query.Types[0] == X_v8s64 || Query.Types[0] == X_v16s32 || Query.Types[0] == X_v32s16 || Query.Types[0] == X_v64s8))); })
      .clampMaxNumElements(0, X_s8, (X_s8MaxVector).getNumElements())
      .clampMaxNumElements(0, X_s16, (X_s16MaxVector).getNumElements())
      .clampMaxNumElements(0, X_s32, (X_s32MaxVector).getNumElements())
      .clampMaxNumElements(0, X_s64, (X_s64MaxVector).getNumElements());
  getActionDefinitionsBuilder(G_EXTRACT)
      .coreLegalIf([=](const LegalityQuery &Query) { return ((X_HasAVX) && (LegalityPredicates::typePairInSet(0, 1, {{X_v16s8, X_v32s8}, {X_v8s16, X_v16s16}, {X_v4s32, X_v8s32}, {X_v2s64, X_v4s64}})(Query))) || ((X_HasAVX512) && (LegalityPredicates::typePairInSet(0, 1, {{X_v16s8, X_v64s8}, {X_v32s8, X_v64s8}, {X_v8s16, X_v32s16}, {X_v16s16, X_v32s16}, {X_v4s32, X_v16s32}, {X_v8s32, X_v16s32}, {X_v2s64, X_v8s64}, {X_v4s64, X_v8s64}})(Query))); });
  getActionDefinitionsBuilder(G_INSERT)
      .coreLegalIf([=](const LegalityQuery &Query) { return ((X_HasAVX) && (LegalityPredicates::typePairInSet(1, 0, {{X_v16s8, X_v32s8}, {X_v8s16, X_v16s16}, {X_v4s32, X_v8s32}, {X_v2s64, X_v4s64}})(Query))) || ((X_HasAVX512) && (LegalityPredicates::typePairInSet(1, 0, {{X_v16s8, X_v64s8}, {X_v32s8, X_v64s8}, {X_v8s16, X_v32s16}, {X_v16s16, X_v32s16}, {X_v4s32, X_v16s32}, {X_v8s32, X_v16s32}, {X_v2s64, X_v8s64}, {X_v4s64, X_v8s64}})(Query))); });
  if (X_HasSSE1) {
    getActionDefinitionsBuilder(G_CONCAT_VECTORS)
        .legalFor({{X_v32s8, X_v16s8}, {X_v16s16, X_v8s16}, {X_v8s32, X_v4s32}, {X_v4s64, X_v2s64}});
  }
  if (X_HasAVX) {
    getActionDefinitionsBuilder(G_CONCAT_VECTORS)
        .legalFor({{X_v64s8, X_v16s8}, {X_v64s8, X_v32s8}, {X_v32s16, X_v8s16}, {X_v32s16, X_v16s16}, {X_v16s32, X_v4s32}, {X_v16s32, X_v8s32}, {X_v8s64, X_v2s64}, {X_v8s64, X_v4s64}});
  }
  getActionDefinitionsBuilder(G_SELECT)
      .legalFor({{X_s16, X_s32}, {X_s32, X_s32}, {X_p0, X_s32}})
      .legalFor(!X_HasCMOV, {{X_s8, X_s32}})
      .legalFor(X_Is64Bit, {{X_s64, X_s32}})
      .legalFor(X_UseX87, {{X_s80, X_s32}})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, X_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, X_s32)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_cmovMinScalar)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, X_sMaxScalar);
  getActionDefinitionsBuilder({G_MEMCPY, G_MEMMOVE, G_MEMSET})
      .libcall();
  getActionDefinitionsBuilder({G_DYN_STACKALLOC, G_STACKSAVE, G_STACKRESTORE})
      .lower();
  if (X_Is64Bit) {
    getActionDefinitionsBuilder(G_IS_FPCLASS)
        .lowerFor({{X_s1, X_s32}, {X_s1, X_s64}, {X_s1, X_s80}});
  }
  getActionDefinitionsBuilder({G_INTRINSIC_ROUNDEVEN, G_INTRINSIC_TRUNC})
      .scalarize(0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, X_s32)
      .libcall();
  getActionDefinitionsBuilder({G_INTRINSIC, G_INTRINSIC_W_SIDE_EFFECTS})
      .alwaysLegal();
  getActionDefinitionsBuilder({G_TRAP, G_DEBUGTRAP, G_UBSANTRAP})
      .alwaysLegal();
  getActionDefinitionsBuilder(G_INVOKE_REGION_START)
      .alwaysLegal();
