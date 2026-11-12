/*===- TableGen'erated file -------------------------------------*- C++ -*-===*\
|*                                                                            *|
|* GlobalISel legalizer rule fragments                                        *|
|*                                                                            *|
|* Automatically generated file, do not edit!                                 *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

  bool A_HasCSSC = ST.hasCSSC();
  bool A_HasFP16 = ST.hasFullFP16();
  bool A_HasRCPC3 = ST.hasRCPC3();
  bool A_HasSVE = ST.hasSVE();
  bool A_SmallCodeModel = TM.getCodeModel() == CodeModel::Small;
  bool A_UseOutlineAtomics = ST.outlineAtomics() && !ST.hasLSE();
  bool A_hasBF16 = ST.hasBF16();
  bool A_hasMOPS = ST.hasMOPS();

  const LLT A_p0 = LLT::pointer(0, 64);
  const LLT A_s8 = LLT::scalar(8);
  const LLT A_s16 = LLT::scalar(16);
  const LLT A_s32 = LLT::scalar(32);
  const LLT A_s64 = LLT::scalar(64);
  const LLT A_s128 = LLT::scalar(128);
  const LLT A_v16s8 = LLT::fixed_vector(16, 8);
  const LLT A_v8s8 = LLT::fixed_vector(8, 8);
  const LLT A_v4s8 = LLT::fixed_vector(4, 8);
  const LLT A_v2s8 = LLT::fixed_vector(2, 8);
  const LLT A_v8s16 = LLT::fixed_vector(8, 16);
  const LLT A_v4s16 = LLT::fixed_vector(4, 16);
  const LLT A_v2s16 = LLT::fixed_vector(2, 16);
  const LLT A_v2s32 = LLT::fixed_vector(2, 32);
  const LLT A_v4s32 = LLT::fixed_vector(4, 32);
  const LLT A_v2s64 = LLT::fixed_vector(2, 64);
  const LLT A_v2p0 = LLT::fixed_vector(2, A_p0);
  const LLT A_nxv16s8 = LLT::scalable_vector(16, A_s8);
  const LLT A_nxv8s16 = LLT::scalable_vector(8, A_s16);
  const LLT A_nxv4s32 = LLT::scalable_vector(4, A_s32);
  const LLT A_nxv2s64 = LLT::scalable_vector(2, A_s64);
  const LLT A_bf16 = LLT::bfloat16();
  const LLT A_v4bf16 = LLT::fixed_vector(4, A_bf16);
  const LLT A_v8bf16 = LLT::fixed_vector(8, A_bf16);
  const LLT A_f16 = LLT::float16();
  const LLT A_v4f16 = LLT::fixed_vector(4, A_f16);
  const LLT A_v8f16 = LLT::fixed_vector(8, A_f16);
  const LLT A_f32 = LLT::float32();
  const LLT A_v2f32 = LLT::fixed_vector(2, A_f32);
  const LLT A_v4f32 = LLT::fixed_vector(4, A_f32);
  const LLT A_f64 = LLT::float64();
  const LLT A_v2f64 = LLT::fixed_vector(2, A_f64);
  const LLT A_f128 = LLT::float128();
  const LLT A_i8 = LLT::integer(8);
  const LLT A_v8i8 = LLT::fixed_vector(8, A_i8);
  const LLT A_v16i8 = LLT::fixed_vector(16, A_i8);
  const LLT A_i16 = LLT::integer(16);
  const LLT A_v8i16 = LLT::fixed_vector(8, A_i16);
  const LLT A_v4i16 = LLT::fixed_vector(4, A_i16);
  const LLT A_i32 = LLT::integer(32);
  const LLT A_v2i32 = LLT::fixed_vector(2, A_i32);
  const LLT A_v4i32 = LLT::fixed_vector(4, A_i32);
  const LLT A_i64 = LLT::integer(64);
  const LLT A_v2i64 = LLT::fixed_vector(2, A_i64);
  const LLT A_i128 = LLT::integer(128);
  const LLT A_nxv16i8 = LLT::scalable_vector(16, A_i8);
  const LLT A_nxv8i16 = LLT::scalable_vector(8, A_i16);
  const LLT A_nxv4i32 = LLT::scalable_vector(4, A_i32);
  const LLT A_nxv2i64 = LLT::scalable_vector(2, A_i64);

  getActionDefinitionsBuilder({G_IMPLICIT_DEF, G_FREEZE, G_CONSTANT_FOLD_BARRIER})
      .legalFor({A_p0, A_s8, A_s16, A_s32, A_s64, A_s128})
      .legalFor({A_v2s8, A_v4s8, A_v8s8, A_v16s8, A_v2s16, A_v4s16, A_v8s16, A_v2s32, A_v4s32, A_v2s64, A_v2p0})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreWidenVectorEltsToVectorMinSize([](const LegalityQuery &) { return true; }, 0, 64)
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(0, A_s64, 2)
      .clampMaxNumElements(0, A_p0, 2)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0);
  getActionDefinitionsBuilder(G_PHI)
      .legalFor({A_p0, A_s16, A_s32, A_s64})
      .legalFor({A_v16s8, A_v8s16, A_v4s32, A_v2s64, A_v2p0, A_v8s8, A_v4s16, A_v2s32})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s16)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(0, A_s64, 2)
      .clampMaxNumElements(0, A_p0, 2)
      .coreWidenScalarOrEltToNextPow2OrMinSize([](const LegalityQuery &) { return true; }, 0, 8);
  getActionDefinitionsBuilder(G_INSERT)
      .coreLegalIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_s32 || Query.Types[0] == A_s64 || Query.Types[0] == A_p0)) && ((Query.Types[1] == A_s8 || Query.Types[1] == A_s16 || Query.Types[1] == A_s32)) && (((Query.Types[1]).getSizeInBits().getKnownMinValue() < (Query.Types[0]).getSizeInBits().getKnownMinValue())); })
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, A_s8)
      .coreMaxScalar([=](const LegalityQuery &Query) { return (Query.Types[0] == A_s32); }, 1, A_s16)
      .coreMaxScalar([=](const LegalityQuery &Query) { return (Query.Types[0] == A_s64 || Query.Types[0] == A_p0); }, 1, A_s32);
  getActionDefinitionsBuilder(G_EXTRACT)
      .coreLegalIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_s16 || Query.Types[0] == A_s32 || Query.Types[0] == A_s64 || Query.Types[0] == A_p0)) && ((Query.Types[1] == A_s32 || Query.Types[1] == A_s64 || Query.Types[1] == A_s128 || Query.Types[1] == A_p0)) && (((Query.Types[0]).getSizeInBits().getKnownMinValue() < (Query.Types[1]).getSizeInBits().getKnownMinValue())); })
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, A_s128)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s16)
      .coreMaxScalar([=](const LegalityQuery &Query) { return (Query.Types[1] == A_s32); }, 0, A_s16)
      .coreMaxScalar([=](const LegalityQuery &Query) { return (Query.Types[1] == A_s64 || Query.Types[1] == A_p0); }, 0, A_s32)
      .coreMaxScalar([=](const LegalityQuery &Query) { return (Query.Types[1] == A_s128); }, 0, A_s64);
  getActionDefinitionsBuilder({G_ADD, G_SUB, G_AND, G_OR, G_XOR})
      .legalFor({A_i32, A_i64, A_v8i8, A_v16i8, A_v4i16, A_v8i16, A_v2i32, A_v4i32, A_v2i64})
      .legalFor(A_HasSVE, {A_nxv16i8, A_nxv8i16, A_nxv4i32, A_nxv2i64})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .clampMaxNumElements(0, A_s8, 16)
      .clampMaxNumElements(0, A_s16, 8)
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(0, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(0, A_s64, (A_v2s64).getNumElements())
      .coreMinScalarOrElt([=](const LegalityQuery &Query) { return (((Query.Types[0]).isVector() ? (Query.Types[0]).getElementCount().getKnownMinValue() : 0) <= 2); }, 0, A_s32)
      .coreMinScalarOrElt([=](const LegalityQuery &Query) { return (((Query.Types[0]).isVector() ? (Query.Types[0]).getElementCount().getKnownMinValue() : 0) <= 4); }, 0, A_s16)
      .coreMinScalarOrElt([=](const LegalityQuery &Query) { return (((Query.Types[0]).isVector() ? (Query.Types[0]).getElementCount().getKnownMinValue() : 0) <= 16); }, 0, A_s8)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0);
  getActionDefinitionsBuilder(G_MUL)
      .legalFor({A_i32, A_i64, A_v8i8, A_v16i8, A_v4i16, A_v8i16, A_v2i32, A_v4i32, A_v2i64})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .clampMaxNumElements(0, A_s8, 16)
      .clampMaxNumElements(0, A_s16, 8)
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(0, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(0, A_s64, (A_v2s64).getNumElements())
      .coreMinScalarOrElt([=](const LegalityQuery &Query) { return (((Query.Types[0]).isVector() ? (Query.Types[0]).getElementCount().getKnownMinValue() : 0) <= 2); }, 0, A_s32)
      .coreMinScalarOrElt([=](const LegalityQuery &Query) { return (((Query.Types[0]).isVector() ? (Query.Types[0]).getElementCount().getKnownMinValue() : 0) <= 4); }, 0, A_s16)
      .coreMinScalarOrElt([=](const LegalityQuery &Query) { return (((Query.Types[0]).isVector() ? (Query.Types[0]).getElementCount().getKnownMinValue() : 0) <= 16); }, 0, A_s8)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0);
  getActionDefinitionsBuilder({G_SHL, G_ASHR, G_LSHR})
      .coreCustomIf([=](const LegalityQuery &Query) { return (!(Query.Types[0].isVector())) && (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 32)) && (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 32)); })
      .legalFor({{A_i32, A_i32}, {A_i32, A_i64}, {A_i64, A_i64}, {A_v8i8, A_v8i8}, {A_v16i8, A_v16i8}, {A_v4i16, A_v4i16}, {A_v8i16, A_v8i16}, {A_v2i32, A_v2i32}, {A_v4i32, A_v4i32}, {A_v2i64, A_v2i64}})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, A_s64)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(0, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(0, A_s64, (A_v2s64).getNumElements())
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .minScalarSameAs(1, 0)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .minScalarEltSameAsIf([=](const LegalityQuery &Query) { return Query.Types[0].isVector(); }, 1, 0)
      .maxScalarEltSameAsIf([=](const LegalityQuery &Query) { return Query.Types[0].isVector(); }, 1, 0);
  getActionDefinitionsBuilder(G_PTR_ADD)
      .legalFor({{A_p0, A_i64}, {A_v2p0, A_v2i64}})
      .coreMinScalarOrElt([](const LegalityQuery &) { return true; }, 1, A_s64)
      .coreMaxScalarOrElt([](const LegalityQuery &) { return true; }, 1, A_s64)
      .clampMinNumElements(0, A_p0, (A_v2p0).getNumElements())
      .clampMaxNumElements(0, A_p0, (A_v2p0).getNumElements());
  getActionDefinitionsBuilder(G_PTRMASK)
      .legalFor({{A_p0, A_s64}});
  getActionDefinitionsBuilder({G_SDIV, G_UDIV})
      .legalFor({A_i32, A_i64})
      .libcallFor({A_i128})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .scalarize(0);
  getActionDefinitionsBuilder({G_SREM, G_UREM, G_SDIVREM, G_UDIVREM})
      .lowerFor({A_i8, A_i16, A_i32, A_i64, A_v2i32, A_v4i32, A_v2i64})
      .libcallFor({A_i128})
      .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalarOrElt([](const LegalityQuery &) { return true; }, 0, A_s32)
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(0, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(0, A_s64, (A_v2s64).getNumElements())
      .scalarize(0);
  getActionDefinitionsBuilder({G_SMULO, G_UMULO})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 32)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .lower();
  getActionDefinitionsBuilder({G_SMULH, G_UMULH})
      .legalFor({A_i64, A_v16i8, A_v8i16, A_v4i32})
      .clampMaxNumElements(0, A_s8, 16)
      .clampMaxNumElements(0, A_s16, 8)
      .clampMaxNumElements(0, A_s32, 4)
      .lower();
  getActionDefinitionsBuilder({G_SMULFIX, G_UMULFIX, G_SMULFIXSAT, G_UMULFIXSAT})
      .lower();
  getActionDefinitionsBuilder({G_SMIN, G_SMAX, G_UMIN, G_UMAX})
      .legalFor({A_v8i8, A_v16i8, A_v4i16, A_v8i16, A_v2i32, A_v4i32})
      .legalFor(A_HasCSSC, {A_i32, A_i64})
      .coreMinScalar([=](const LegalityQuery &Query) { return A_HasCSSC; }, 0, A_s32)
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .lower();
  if (A_HasCSSC) {
    getActionDefinitionsBuilder(G_ABS)
        .legalFor({A_i32, A_i64});
  }
  getActionDefinitionsBuilder(G_ABS)
      .legalFor({A_v16i8, A_v8i16, A_v4i32, A_v2i64, A_v2p0, A_v8i8, A_v4i16, A_v2i32})
      .coreCustomIf([=](const LegalityQuery &Query) { return (Query.Types[0].isScalar()) && (((Query.Types[0]).getSizeInBits().getKnownMinValue() < 128)); })
      .coreWidenTo([=](const LegalityQuery &Query) { return (Query.Types[0] == A_v4s8); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, A_v4i16); })
      .coreWidenTo([=](const LegalityQuery &Query) { return (Query.Types[0] == A_v2s16); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, A_v2i32); })
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(0, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(0, A_s64, (A_v2s64).getNumElements())
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .lower();
  getActionDefinitionsBuilder({G_ABDS, G_ABDU, G_UAVGFLOOR, G_UAVGCEIL, G_SAVGFLOOR, G_SAVGCEIL})
      .legalFor({A_v8i8, A_v16i8, A_v4i16, A_v8i16, A_v2i32, A_v4i32})
      .lower();
  getActionDefinitionsBuilder({G_SADDE, G_SSUBE, G_UADDE, G_USUBE, G_SADDO, G_SSUBO, G_UADDO, G_USUBO})
      .legalFor({{A_i32, A_i32}, {A_i64, A_i32}})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, A_s64)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .lower();
  getActionDefinitionsBuilder({G_FSHL, G_FSHR})
      .customFor({{A_i32, A_i32}, {A_i32, A_i64}, {A_i64, A_i64}})
      .lower();
  getActionDefinitionsBuilder(G_ROTR)
      .legalFor({{A_i32, A_i64}, {A_i64, A_i64}})
      .coreCustomIf([=](const LegalityQuery &Query) { return (Query.Types[0].isScalar()) && (((Query.Types[1]).getScalarSizeInBits() < 64)); })
      .lower();
  getActionDefinitionsBuilder(G_ROTL)
      .lower();
  getActionDefinitionsBuilder({G_SBFX, G_UBFX})
      .customFor({{A_s32, A_s32}, {A_s64, A_s64}});
  getActionDefinitionsBuilder(G_CTPOP)
      .legalFor(A_HasCSSC, {{A_i32, A_i32}, {A_i64, A_i64}})
      .legalFor({{A_v8i8, A_v8i8}, {A_v16i8, A_v16i8}})
      .customFor(!A_HasCSSC, {{A_s32, A_s32}, {A_s64, A_s64}})
      .customFor({{A_s128, A_s128}, {A_v4s16, A_v4s16}, {A_v8s16, A_v8s16}, {A_v2s32, A_v2s32}, {A_v4s32, A_v4s32}, {A_v2s64, A_v2s64}})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s128)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreWidenScalarOrEltToNextPow2OrMinSize([](const LegalityQuery &) { return true; }, 0, 8)
      .minScalarEltSameAsIf([=](const LegalityQuery &Query) { return true; }, 1, 0)
      .maxScalarEltSameAsIf([=](const LegalityQuery &Query) { return true; }, 1, 0)
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(0, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(0, A_s64, (A_v2s64).getNumElements())
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0);
  getActionDefinitionsBuilder({G_CTLZ, G_CTLS})
      .legalFor({{A_i32, A_i32}, {A_i64, A_i64}, {A_v8i8, A_v8i8}, {A_v16i8, A_v16i8}, {A_v4i16, A_v4i16}, {A_v8i16, A_v8i16}, {A_v2i32, A_v2i32}, {A_v4i32, A_v4i32}})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 32)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, A_s64)
      .coreWidenScalarOrEltToNextPow2OrMinSize([](const LegalityQuery &) { return true; }, 1, 8)
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 32); }, 0)
      .minScalarSameAs(0, 1)
      .maxScalarSameAs(0, 1);
  getActionDefinitionsBuilder(G_INSERT_SUBVECTOR)
      .lower();
  getActionDefinitionsBuilder(G_CTLZ_ZERO_POISON)
      .lower();
  getActionDefinitionsBuilder(G_CTTZ)
      .coreLowerIf([=](const LegalityQuery &Query) { return Query.Types[0].isVector(); })
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 32)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, A_s64)
      .minScalarSameAs(0, 1)
      .maxScalarSameAs(0, 1)
      .legalFor(A_HasCSSC, {A_s32, A_s64})
      .customFor(!A_HasCSSC, {A_s32, A_s64});
  getActionDefinitionsBuilder(G_CTTZ_ZERO_POISON)
      .lower();
  getActionDefinitionsBuilder(G_BITREVERSE)
      .legalFor({A_i32, A_i64, A_v8i8, A_v16i8})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 32)
      .coreWidenScalarOrEltToNextPow2OrMinSize([](const LegalityQuery &) { return true; }, 0, 8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(0, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(0, A_s64, (A_v2s64).getNumElements())
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .lower();
  getActionDefinitionsBuilder(G_CLMUL)
      .legalFor({A_v8i8, A_v16i8});
  getActionDefinitionsBuilder(G_BSWAP)
      .legalFor({A_i32, A_i64, A_v4i16, A_v8i16, A_v2i32, A_v4i32, A_v2i64})
      .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 0, 16)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(0, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(0, A_s64, (A_v2s64).getNumElements())
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0);
  getActionDefinitionsBuilder({G_UADDSAT, G_SADDSAT, G_USUBSAT, G_SSUBSAT})
      .legalFor({A_v8i8, A_v16i8, A_v4i16, A_v8i16, A_v2i32, A_v4i32, A_v2i64})
      .legalFor(A_HasSVE, {A_nxv16i8, A_nxv8i16, A_nxv4i32, A_nxv2i64})
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(0, A_s64, 2)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .lower();
  getActionDefinitionsBuilder({G_FADD, G_FSUB, G_FMUL, G_FDIV, G_FMA, G_FSQRT, G_FMAXNUM, G_FMINNUM, G_FMAXIMUM, G_FMINIMUM, G_FCEIL, G_FFLOOR, G_FRINT, G_FNEARBYINT, G_INTRINSIC_TRUNC, G_INTRINSIC_ROUND, G_INTRINSIC_ROUNDEVEN})
      .legalFor({A_f32, A_f64, A_v2f32, A_v4f32, A_v2f64})
      .legalFor(A_HasFP16, {A_f16, A_v4f16, A_v8f16})
      .libcallFor({A_f128})
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .coreWidenTo([=](const LegalityQuery &Query) { return ((!(A_HasFP16)) && (((Query.Types[0]).getScalarType()).isFloat16())) || (((Query.Types[0]).getScalarType()).isBFloat16()); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementType(A_f32)); })
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(0, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(0, A_s64, (A_v2s64).getNumElements())
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0);
  getActionDefinitionsBuilder({G_FABS, G_FNEG})
      .legalFor({A_f32, A_f64, A_v2f32, A_v4f32, A_v2f64})
      .legalFor(A_HasFP16, {A_f16, A_bf16, A_v4f16, A_v4bf16, A_v8f16, A_v8bf16})
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .coreLowerIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); })
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(0, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(0, A_s64, (A_v2s64).getNumElements())
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .lowerFor({A_f16, A_bf16, A_v4f16, A_v4bf16, A_v8f16, A_v8bf16});
  getActionDefinitionsBuilder({G_FREM, G_FCOS, G_FSIN, G_FPOW, G_FLOG, G_FLOG2, G_FLOG10, G_FTAN, G_FEXP, G_FEXP2, G_FEXP10, G_FACOS, G_FASIN, G_FATAN, G_FATAN2, G_FCOSH, G_FSINH, G_FTANH, G_FMODF})
      .libcallFor({A_f32, A_f64, A_f128})
      .widenScalarFor({A_f16, A_bf16}, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementType(A_f32)); })
      .scalarize(0);
  getActionDefinitionsBuilder({G_FPOWI, G_FLDEXP})
      .libcallFor({{A_f32, A_i32}, {A_f64, A_i32}, {A_f128, A_i32}})
      .widenScalarFor({A_f16, A_bf16}, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementType(A_f32)); })
      .scalarize(0);
  getActionDefinitionsBuilder({G_LROUND, G_INTRINSIC_LRINT})
      .legalFor({{A_i32, A_f32}, {A_i32, A_f64}, {A_i64, A_f32}, {A_i64, A_f64}})
      .legalFor(A_HasFP16, {{A_i32, A_f16}, {A_i64, A_f16}})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, A_s32)
      .libcallFor({{A_s64, A_s128}})
      .lower();
  getActionDefinitionsBuilder({G_LLROUND, G_INTRINSIC_LLRINT})
      .legalFor({{A_i64, A_f32}, {A_i64, A_f64}})
      .legalFor(A_HasFP16, {{A_i64, A_f16}})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, A_s32)
      .libcallFor({{A_s64, A_s128}})
      .lower();
  getActionDefinitionsBuilder(G_FCOPYSIGN)
      .coreMoreTo([=](const LegalityQuery &Query) { return Query.Types[0].isScalar(); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, LLT::fixed_vector(((Query.Types[0] == A_s16) ? 4 : 2), Query.Types[0])); })
      .lower();
  getActionDefinitionsBuilder(G_FMAD)
      .lower();
  getActionDefinitionsBuilder(G_SEXTLOAD)
      .coreLowerIf([=](const LegalityQuery &Query) { return isAtLeastOrStrongerThan((Query.MMODescrs[0]).Ordering, AtomicOrdering::Unordered); })
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{A_s32, A_p0, A_s8, 8}, {A_s32, A_p0, A_s16, 8}, {A_s32, A_p0, A_s32, 8}, {A_s64, A_p0, A_s8, 2}, {A_s64, A_p0, A_s16, 2}, {A_s64, A_p0, A_s32, 4}, {A_s64, A_p0, A_s64, 8}, {A_p0, A_p0, A_s64, 8}, {A_v2s32, A_p0, A_s64, 8}})(Query); })
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreUnsupportedIf([=](const LegalityQuery &Query) { return !(llvm::isPowerOf2_32(((Query.MMODescrs[0]).MemoryTy).getSizeInBytes().getKnownMinValue())); })
      .lower();
  getActionDefinitionsBuilder(G_ZEXTLOAD)
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{A_s32, A_p0, A_s8, 8}, {A_s32, A_p0, A_s16, 8}, {A_s32, A_p0, A_s32, 8}, {A_s64, A_p0, A_s8, 2}, {A_s64, A_p0, A_s16, 2}, {A_s64, A_p0, A_s32, 4}, {A_s64, A_p0, A_s64, 8}, {A_p0, A_p0, A_s64, 8}, {A_v2s32, A_p0, A_s64, 8}})(Query); })
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreUnsupportedIf([=](const LegalityQuery &Query) { return !(llvm::isPowerOf2_32(((Query.MMODescrs[0]).MemoryTy).getSizeInBytes().getKnownMinValue())); })
      .lower();
  if (A_HasRCPC3) {
    getActionDefinitionsBuilder(G_LOAD)
        .coreCustomIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_s128)) && (((Query.MMODescrs[0]).Ordering == AtomicOrdering::Acquire)); });
  }
  getActionDefinitionsBuilder(G_LOAD)
      .coreCustomIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_s128)) && (isAtLeastOrStrongerThan((Query.MMODescrs[0]).Ordering, AtomicOrdering::Unordered)); })
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{A_s8, A_p0, A_s8, 8}, {A_s16, A_p0, A_s16, 8}, {A_s32, A_p0, A_s32, 8}, {A_s64, A_p0, A_s64, 8}, {A_p0, A_p0, A_s64, 8}, {A_s128, A_p0, A_s128, 8}, {A_v8s8, A_p0, A_s64, 8}, {A_v16s8, A_p0, A_s128, 8}, {A_v4s16, A_p0, A_s64, 8}, {A_v8s16, A_p0, A_s128, 8}, {A_v2s32, A_p0, A_s64, 8}, {A_v4s32, A_p0, A_s128, 8}, {A_v2s64, A_p0, A_s128, 8}})(Query); })
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{A_s32, A_p0, A_s8, 8}, {A_s32, A_p0, A_s16, 8}, {A_s64, A_p0, A_s32, 8}})(Query); })
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{A_nxv16s8, A_p0, A_nxv16s8, 8}, {A_nxv8s16, A_p0, A_nxv8s16, 8}, {A_nxv4s32, A_p0, A_nxv4s32, 8}, {A_nxv2s64, A_p0, A_nxv2s64, 8}})(Query); })
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 8)
      .clampMaxNumElements(0, A_s8, 16)
      .clampMaxNumElements(0, A_s16, 8)
      .clampMaxNumElements(0, A_s32, 4)
      .clampMaxNumElements(0, A_s64, 2)
      .clampMaxNumElements(0, A_p0, 2)
      .coreLowerIf([=](const LegalityQuery &Query) { return (!(((Query.MMODescrs[0]).MemoryTy).isByteSized())) || (!(llvm::isPowerOf2_32(((Query.MMODescrs[0]).MemoryTy).getSizeInBytes().getKnownMinValue()))); })
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreNarrowTo([=](const LegalityQuery &Query) { return (Query.Types[0].isScalar()) && ((Query.Types[0] != (Query.MMODescrs[0]).MemoryTy)) && (((Query.Types[0]).getSizeInBits().getKnownMinValue() > 32)); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, A_i32); })
      .coreBitcastToType([=](const LegalityQuery &Query) { return (Query.Types[0] == A_v4s8); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, LLT::integer((Query.Types[0]).getSizeInBits().getKnownMinValue())); })
      .coreCustomIf([=](const LegalityQuery &Query) { return (Query.Types[0].isPointerVector()) && (((Query.Types[0]).getAddressSpace() == 0)); })
      .coreScalarizeIf([=](const LegalityQuery &Query) { return (Query.Types[0] == A_v2s16 || Query.Types[0] == A_v2s8); }, 0)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0);
  if (A_HasRCPC3) {
    getActionDefinitionsBuilder(G_STORE)
        .coreCustomIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_s128)) && (((Query.MMODescrs[0]).Ordering == AtomicOrdering::Release)); });
  }
  getActionDefinitionsBuilder(G_STORE)
      .coreCustomIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_s128)) && (isAtLeastOrStrongerThan((Query.MMODescrs[0]).Ordering, AtomicOrdering::Unordered)); })
      .coreWidenTo([=](const LegalityQuery &Query) { return ((Query.Types[0].isScalar()) && (((Query.Types[0]).getSizeInBits().getKnownMinValue() < 32))) && (isAtLeastOrStrongerThan((Query.MMODescrs[0]).Ordering, AtomicOrdering::Release)); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementSize((A_s32).getSizeInBits().getKnownMinValue())); })
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{A_s8, A_p0, A_s8, 8}, {A_s16, A_p0, A_s8, 8}, {A_s32, A_p0, A_s8, 8}, {A_s64, A_p0, A_s8, 8}, {A_s16, A_p0, A_s16, 8}, {A_s32, A_p0, A_s16, 8}, {A_s64, A_p0, A_s16, 8}, {A_s32, A_p0, A_s8, 8}, {A_s32, A_p0, A_s16, 8}, {A_s32, A_p0, A_s32, 8}, {A_s64, A_p0, A_s64, 8}, {A_s64, A_p0, A_s32, 8}, {A_p0, A_p0, A_s64, 8}, {A_s128, A_p0, A_s128, 8}, {A_v16s8, A_p0, A_s128, 8}, {A_v8s8, A_p0, A_s64, 8}, {A_v4s16, A_p0, A_s64, 8}, {A_v8s16, A_p0, A_s128, 8}, {A_v2s32, A_p0, A_s64, 8}, {A_v4s32, A_p0, A_s128, 8}, {A_v2s64, A_p0, A_s128, 8}})(Query); })
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{A_nxv16s8, A_p0, A_nxv16s8, 8}, {A_nxv8s16, A_p0, A_nxv8s16, 8}, {A_nxv4s32, A_p0, A_nxv4s32, 8}, {A_nxv2s64, A_p0, A_nxv2s64, 8}})(Query); })
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreMinScalarOrElt([](const LegalityQuery &) { return true; }, 0, A_s8)
      .coreLowerIf([=](const LegalityQuery &Query) { return (Query.Types[0].isScalar()) && ((Query.Types[0] != (Query.MMODescrs[0]).MemoryTy)); })
      .clampMaxNumElements(0, A_s8, 16)
      .clampMaxNumElements(0, A_s16, 8)
      .clampMaxNumElements(0, A_s32, 4)
      .clampMaxNumElements(0, A_s64, 2)
      .clampMaxNumElements(0, A_p0, 2)
      .coreLowerIf([=](const LegalityQuery &Query) { return !(llvm::isPowerOf2_32(((Query.MMODescrs[0]).MemoryTy).getSizeInBytes().getKnownMinValue())); })
      .coreBitcastToType([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_v4s8)) && (((Query.Types[0]).getSizeInBits().getKnownMinValue() == ((Query.MMODescrs[0]).MemoryTy).getSizeInBits().getKnownMinValue())); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, LLT::integer((Query.Types[0]).getSizeInBits().getKnownMinValue())); })
      .coreCustomIf([=](const LegalityQuery &Query) { return (Query.Types[0].isPointerVector()) && (((Query.Types[0]).getAddressSpace() == 0)); })
      .coreScalarizeIf([=](const LegalityQuery &Query) { return (Query.Types[0] == A_v2s16 || Query.Types[0] == A_v2s8); }, 0)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .lower();
  getActionDefinitionsBuilder(G_INDEXED_STORE)
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairAndMemDescInSet(0, 1, 0, {{A_p0, A_s8, A_s8, 8}, {A_p0, A_s16, A_s16, 8}, {A_p0, A_s32, A_s8, 8}, {A_p0, A_s32, A_s16, 8}, {A_p0, A_s32, A_s32, 8}, {A_p0, A_s64, A_s64, 8}, {A_p0, A_p0, A_p0, 8}, {A_p0, A_v8s8, A_v8s8, 8}, {A_p0, A_v16s8, A_v16s8, 8}, {A_p0, A_v4s16, A_v4s16, 8}, {A_p0, A_v8s16, A_v8s16, 8}, {A_p0, A_v2s32, A_v2s32, 8}, {A_p0, A_v4s32, A_v4s32, 8}, {A_p0, A_v2s64, A_v2s64, 8}, {A_p0, A_v2p0, A_v2p0, 8}, {A_p0, A_s128, A_s128, 8}})(Query); })
      .unsupported();
  getActionDefinitionsBuilder(G_INDEXED_LOAD)
      .coreUnsupportedIf([=](const LegalityQuery &Query) { return isAtLeastOrStrongerThan((Query.MMODescrs[0]).Ordering, AtomicOrdering::Unordered); })
      .coreLegalIf([=](const LegalityQuery &Query) { return (!((!((Query.Types[0] == A_v16s8 || Query.Types[0] == A_v8s16 || Query.Types[0] == A_v4s32 || Query.Types[0] == A_v2s64 || Query.Types[0] == A_v2p0 || Query.Types[0] == A_v8s8 || Query.Types[0] == A_v4s16 || Query.Types[0] == A_v2s32))) && (!((Query.Types[0] == A_s8 || Query.Types[0] == A_s16 || Query.Types[0] == A_s32 || Query.Types[0] == A_s64 || Query.Types[0] == A_p0))) && ((Query.Types[0] != A_s128)))) && ((!((Query.Types[1] != A_p0))) && (true)); })
      .unsupported();
  getActionDefinitionsBuilder({G_INDEXED_SEXTLOAD, G_INDEXED_ZEXTLOAD})
      .coreUnsupportedIf([=](const LegalityQuery &Query) { return isAtLeastOrStrongerThan((Query.MMODescrs[0]).Ordering, AtomicOrdering::Unordered); })
      .coreLegalIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_s16 || Query.Types[0] == A_s32 || Query.Types[0] == A_s64)) && ((!((Query.Types[1] != A_p0))) && ((((Query.Types[0] == A_s16)) && (((Query.MMODescrs[0]).MemoryTy == A_s8))) || ((!((Query.Types[0] == A_s16))) && ((((Query.Types[0] == A_s32)) && ((((Query.MMODescrs[0]).MemoryTy == A_s8)) || (((Query.MMODescrs[0]).MemoryTy == A_s16)))) || ((!((Query.Types[0] == A_s32))) && (((Query.Types[0] == A_s64)) && ((((Query.MMODescrs[0]).MemoryTy == A_s8)) || (((Query.MMODescrs[0]).MemoryTy == A_s16)) || (((Query.MMODescrs[0]).MemoryTy == A_s32))))))))); })
      .unsupported();
  getActionDefinitionsBuilder(G_CONSTANT)
      .legalFor({A_p0, A_s8, A_s16, A_s32, A_s64})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64);
  getActionDefinitionsBuilder(G_FCONSTANT)
      .legalFor({A_s16, A_s32, A_s64, A_s128});
  getActionDefinitionsBuilder(G_ICMP)
      .legalFor({{A_i32, A_i32}, {A_i32, A_i64}, {A_i32, A_p0}})
      .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .coreMinScalarOrElt([](const LegalityQuery &) { return true; }, 1, A_s8)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, A_s64)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[1]).getScalarSizeInBits() > 64); }, 1)
      .minScalarEltSameAsIf([=](const LegalityQuery &Query) { return (Query.Types[0].isVector()) && (!(Query.Types[1].isPointerVector())) && (((Query.Types[0]).getElementType() != (Query.Types[1]).getElementType())); }, 0, 1)
      .coreMinScalarOrElt([=](const LegalityQuery &Query) { return (Query.Types[1] == A_v2s16); }, 1, A_s32)
      .coreMinScalarOrElt([=](const LegalityQuery &Query) { return Query.Types[1].isPointerVector(); }, 0, A_s64)
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .clampMinNumElements(1, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(1, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(1, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(1, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(1, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(1, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(1, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(1, A_s64, (A_v2s64).getNumElements())
      .clampMinNumElements(1, A_p0, (A_v2p0).getNumElements())
      .clampMaxNumElements(1, A_p0, (A_v2p0).getNumElements())
      .coreCustomIf([=](const LegalityQuery &Query) { return Query.Types[0].isVector(); });
  getActionDefinitionsBuilder(G_FCMP)
      .legalFor({{A_i32, A_f32}, {A_i32, A_f64}, {A_v4i32, A_v4f32}, {A_v2i32, A_v2f32}, {A_v2i64, A_v2f64}})
      .legalFor(A_HasFP16, {{A_i32, A_f16}, {A_v4i16, A_v4f16}, {A_v8i16, A_v8f16}})
      .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreWidenTo([=](const LegalityQuery &Query) { return ((!(A_HasFP16)) && (((Query.Types[1]).getScalarType()).isFloat16())) || (((Query.Types[1]).getScalarType()).isBFloat16()); }, 1, [=](const LegalityQuery &Query) { return std::make_pair(1, (Query.Types[1]).changeElementType(A_f32)); })
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[1]).getScalarSizeInBits() > 64); }, 1)
      .minScalarEltSameAsIf([=](const LegalityQuery &Query) { return (Query.Types[0].isVector()) && (!(Query.Types[1].isPointerVector())) && (((Query.Types[0]).getElementType() != (Query.Types[1]).getElementType())); }, 0, 1)
      .clampMinNumElements(1, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(1, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(1, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(1, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(1, A_s64, 2)
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .libcallFor({{A_s32, A_s128}});
  getActionDefinitionsBuilder({G_ZEXT, G_SEXT, G_ANYEXT})
      .coreLegalIf([=](const LegalityQuery &Query) { return (!(Query.Types[0].isVector())) && ((!((((Query.Types[0]).getSizeInBits().getKnownMinValue() < 8)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() >= 128)) || (!(llvm::isPowerOf2_32((Query.Types[0]).getSizeInBits().getKnownMinValue()))))) && ((!((((Query.Types[1]).getSizeInBits().getKnownMinValue() < 8)) || (!(llvm::isPowerOf2_32((Query.Types[1]).getSizeInBits().getKnownMinValue()))))) && (true))); })
      .legalFor({{A_v8s16, A_v8s8}, {A_v4s32, A_v4s16}, {A_v2s64, A_v2s32}})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .clampMaxNumElements(1, A_s8, 8)
      .clampMaxNumElements(1, A_s16, 4)
      .clampMaxNumElements(1, A_s32, 2)
      .coreLowerIf([=](const LegalityQuery &Query) { return (((Query.Types[0]).getScalarSizeInBits() > ((Query.Types[1]).getScalarSizeInBits() * 2))) && (Query.Types[0].isVector()) && ((((Query.Types[1]).getScalarSizeInBits() == 8)) || (((Query.Types[1]).getScalarSizeInBits() == 16))); })
      .clampMinNumElements(1, A_s8, 8)
      .clampMinNumElements(1, A_s16, 4)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0);
  getActionDefinitionsBuilder(G_TRUNC)
      .legalFor({{A_v8s8, A_v8s16}, {A_v4s16, A_v4s32}, {A_v2s32, A_v2s64}})
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .clampMaxNumElements(0, A_s8, 8)
      .clampMaxNumElements(0, A_s16, 4)
      .clampMaxNumElements(0, A_s32, 2)
      .coreMinScalarOrElt([=](const LegalityQuery &Query) { return Query.Types[0].isVector(); }, 0, A_s8)
      .coreLowerIf([=](const LegalityQuery &Query) { return (Query.Types[0].isVector()) && (((Query.Types[1]).getSizeInBits().getKnownMinValue() > 128)) && ((((Query.Types[0]).getScalarSizeInBits() * 2) <= (Query.Types[1]).getScalarSizeInBits())); })
      .clampMinNumElements(0, A_s8, 8)
      .clampMinNumElements(0, A_s16, 4)
      .alwaysLegal();
  getActionDefinitionsBuilder({G_TRUNC_SSAT_S, G_TRUNC_SSAT_U, G_TRUNC_USAT_U})
      .legalFor({{A_v8i8, A_v8i16}, {A_v4i16, A_v4i32}, {A_v2i32, A_v2i64}})
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v2s32).getNumElements())
      .lower();
  getActionDefinitionsBuilder(G_SEXT_INREG)
      .coreLegalIf([=](const LegalityQuery &Query) { return (Query.Types[0] == A_i32 || Query.Types[0] == A_i64 || Query.Types[0] == A_v8i8 || Query.Types[0] == A_v16i8 || Query.Types[0] == A_v4i16 || Query.Types[0] == A_v8i16 || Query.Types[0] == A_v2i32 || Query.Types[0] == A_v4i32 || Query.Types[0] == A_v2i64); })
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(0, A_s64, 2)
      .lower();
  getActionDefinitionsBuilder(G_FPTRUNC)
      .legalFor({{A_f16, A_f32}, {A_f16, A_f64}, {A_f32, A_f64}, {A_v4f16, A_v4f32}, {A_v2f32, A_v2f64}})
      .legalFor(A_hasBF16, {{A_bf16, A_f32}, {A_v4bf16, A_v4f32}})
      .libcallFor({{A_f16, A_f128}, {A_f32, A_f128}, {A_f64, A_f128}})
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .coreCustomIf([=](const LegalityQuery &Query) { return (((Query.Types[1]).getScalarSizeInBits() == 64)) && (((Query.Types[0]).getScalarSizeInBits() == 16)); })
      .lowerFor({{A_bf16, A_f32}, {A_v4bf16, A_v4f32}})
      .clampMinNumElements(1, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(1, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(1, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(1, A_s64, (A_v2s64).getNumElements())
      .scalarize(0);
  getActionDefinitionsBuilder(G_FPEXT)
      .legalFor({{A_f32, A_f16}, {A_f64, A_f16}, {A_f32, A_bf16}, {A_f64, A_f32}, {A_v4f32, A_v4f16}, {A_v4f32, A_v4bf16}, {A_v2f64, A_v2f32}})
      .libcallFor({{A_f128, A_f64}, {A_f128, A_f32}, {A_f128, A_f16}})
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreWidenTo([=](const LegalityQuery &Query) { return (Query.Types[1].isVector()) && (Query.Types[0].isVector()) && (((Query.Types[1]).getScalarSizeInBits() == 16)) && (((Query.Types[0]).getScalarSizeInBits() == 64)); }, 1, [=](const LegalityQuery &Query) { return std::make_pair(1, (Query.Types[1]).changeElementType(A_f32)); })
      .clampMinNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(0, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(0, A_s64, (A_v2s64).getNumElements())
      .scalarize(0);
  getActionDefinitionsBuilder({G_FPTOSI, G_FPTOUI})
      .legalFor({{A_i32, A_f32}, {A_i64, A_f32}, {A_i32, A_f64}, {A_i64, A_f64}, {A_v2i32, A_v2f32}, {A_v4i32, A_v4f32}, {A_v2i64, A_v2f64}})
      .legalFor(A_HasFP16, {{A_i32, A_f16}, {A_i64, A_f16}, {A_v4i16, A_v4f16}, {A_v8i16, A_v8f16}})
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[1]).getScalarSizeInBits() > 64); }, 1)
      .coreNarrowTo([=](const LegalityQuery &Query) { return ((Query.Types[1] == A_f16)) && (((Query.Types[0]).getSizeInBits().getKnownMinValue() > 64)); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, A_i64); })
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreWidenTo([=](const LegalityQuery &Query) { return ((!(A_HasFP16)) && (((Query.Types[1]).getScalarType()).isFloat16())) || (((Query.Types[1]).getScalarType()).isBFloat16()); }, 1, [=](const LegalityQuery &Query) { return std::make_pair(1, (Query.Types[1]).changeElementType(A_f32)); })
      .coreWidenTo([=](const LegalityQuery &Query) { return (((Query.Types[0]).getScalarSizeInBits() <= 64)) && (((Query.Types[0]).getScalarSizeInBits() > (Query.Types[1]).getScalarSizeInBits())); }, 1, [=](const LegalityQuery &Query) { return std::make_pair(1, (Query.Types[1]).changeElementSize((Query.Types[0]).getScalarSizeInBits())); })
      .coreWidenTo([=](const LegalityQuery &Query) { return (((Query.Types[1]).getScalarSizeInBits() <= 64)) && (((Query.Types[0]).getScalarSizeInBits() < (Query.Types[1]).getScalarSizeInBits())); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementSize((Query.Types[1]).getScalarSizeInBits())); })
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(0, A_s64, 2)
      .libcallFor({{A_i32, A_f128}, {A_i64, A_f128}, {A_i128, A_f128}, {A_i128, A_f32}, {A_i128, A_f64}});
  getActionDefinitionsBuilder({G_FPTOSI_SAT, G_FPTOUI_SAT})
      .legalFor({{A_i32, A_f32}, {A_i64, A_f32}, {A_i32, A_f64}, {A_i64, A_f64}, {A_v2i32, A_v2f32}, {A_v4i32, A_v4f32}, {A_v2i64, A_v2f64}})
      .legalFor(A_HasFP16, {{A_i16, A_f16}, {A_i32, A_f16}, {A_i64, A_f16}, {A_v4i16, A_v4f16}, {A_v8i16, A_v8f16}})
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[1]).getScalarSizeInBits() > 64); }, 1)
      .coreNarrowTo([=](const LegalityQuery &Query) { return ((Query.Types[1] == A_f16)) && (((Query.Types[0]).getSizeInBits().getKnownMinValue() > 64)); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, A_i64); })
      .coreLowerIf([=](const LegalityQuery &Query) { return ((Query.Types[0].isScalar()) && (((Query.Types[0]).getSizeInBits().getKnownMinValue() > 64))) || ((Query.Types[1].isScalar()) && (((Query.Types[1]).getSizeInBits().getKnownMinValue() > 64))); })
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 32)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreWidenTo([=](const LegalityQuery &Query) { return ((!(A_HasFP16)) && (((Query.Types[1]).getScalarType()).isFloat16())) || (((Query.Types[1]).getScalarType()).isBFloat16()); }, 1, [=](const LegalityQuery &Query) { return std::make_pair(1, (Query.Types[1]).changeElementType(A_f32)); })
      .coreWidenTo([=](const LegalityQuery &Query) { return ((((Query.Types[0]).getScalarSizeInBits() == 16)) || (((Query.Types[0]).getScalarSizeInBits() == 32)) || (((Query.Types[0]).getScalarSizeInBits() == 64))) && (((Query.Types[0]).getScalarSizeInBits() > (Query.Types[1]).getScalarSizeInBits())); }, 1, [=](const LegalityQuery &Query) { return std::make_pair(1, (Query.Types[1]).changeElementSize((Query.Types[0]).getScalarSizeInBits())); })
      .coreWidenTo([=](const LegalityQuery &Query) { return ((((Query.Types[1]).getScalarSizeInBits() == 16)) || (((Query.Types[1]).getScalarSizeInBits() == 32)) || (((Query.Types[1]).getScalarSizeInBits() == 64))) && (((Query.Types[0]).getScalarSizeInBits() < (Query.Types[1]).getScalarSizeInBits())); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementSize((Query.Types[1]).getScalarSizeInBits())); })
      .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(0, A_s64, 2);
  getActionDefinitionsBuilder({G_SITOFP, G_UITOFP})
      .legalFor({{A_f32, A_i32}, {A_f64, A_i32}, {A_f32, A_i64}, {A_f64, A_i64}, {A_v2f32, A_v2i32}, {A_v4f32, A_v4i32}, {A_v2f64, A_v2i64}})
      .legalFor(A_HasFP16, {{A_f16, A_i32}, {A_f16, A_i64}, {A_v4f16, A_v4i16}, {A_v8f16, A_v8i16}})
      .coreUnsupportedIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarType()).isBFloat16(); })
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[1]).getScalarSizeInBits() > 64); }, 1)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, A_f32)
      .coreLowerIf([=](const LegalityQuery &Query) { return (Query.Types[1].isVector()) && (((Query.Types[1]).getScalarSizeInBits() == 64)) && (((Query.Types[0]).getScalarSizeInBits() == 16)); })
      .coreWidenScalarOrEltToNextPow2OrMinSize([](const LegalityQuery &) { return true; }, 0, (A_HasFP16 ? 16 : 32))
      .coreScalarizeIf([=](const LegalityQuery &Query) { return (((Query.Types[0]).getScalarSizeInBits() == 32)) && (((Query.Types[1]).getScalarSizeInBits() == 64)); }, 0)
      .coreWidenTo([=](const LegalityQuery &Query) { return (((Query.Types[1]).getScalarSizeInBits() <= 64)) && (((Query.Types[0]).getScalarSizeInBits() < (Query.Types[1]).getScalarSizeInBits())); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementSize((Query.Types[1]).getScalarSizeInBits())); })
      .coreWidenTo([=](const LegalityQuery &Query) { return (((Query.Types[0]).getScalarSizeInBits() <= 64)) && (((Query.Types[0]).getScalarSizeInBits() > (Query.Types[1]).getScalarSizeInBits())); }, 1, [=](const LegalityQuery &Query) { return std::make_pair(1, (Query.Types[1]).changeElementSize((Query.Types[0]).getScalarSizeInBits())); })
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(0, A_s64, 2)
      .libcallFor({{A_f16, A_i128}, {A_f32, A_i128}, {A_f64, A_i128}, {A_f128, A_i128}, {A_f128, A_i32}, {A_f128, A_i64}});
  getActionDefinitionsBuilder(G_BR)
      .alwaysLegal();
  getActionDefinitionsBuilder(G_BRCOND)
      .legalFor({A_s32})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s32);
  getActionDefinitionsBuilder(G_BRINDIRECT)
      .legalFor({A_p0});
  getActionDefinitionsBuilder(G_SELECT)
      .legalFor({{A_s32, A_s32}, {A_s64, A_s32}, {A_p0, A_s32}})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, A_s32)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .minScalarEltSameAsIf([=](const LegalityQuery &Query) { return (Query.Types[0].isVector()) && (Query.Types[1].isVector()); }, 1, 0)
      .coreLowerIf([=](const LegalityQuery &Query) { return Query.Types[0].isVector(); });
  getActionDefinitionsBuilder(G_FRAME_INDEX)
      .legalFor({A_p0});
  if (A_SmallCodeModel) {
    getActionDefinitionsBuilder(G_GLOBAL_VALUE)
        .custom();
  }
  if (!A_SmallCodeModel) {
    getActionDefinitionsBuilder(G_GLOBAL_VALUE)
        .legalFor({A_p0});
  }
  getActionDefinitionsBuilder(G_PTRAUTH_GLOBAL_VALUE)
      .coreLegalIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_p0)) && ((Query.Types[1] == A_p0)); });
  getActionDefinitionsBuilder(G_PTRTOINT)
      .legalFor({{A_i64, A_p0}, {A_v2i64, A_v2p0}})
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 64)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .clampMaxNumElements(0, A_s64, 2);
  getActionDefinitionsBuilder(G_INTTOPTR)
      .coreUnsupportedIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getSizeInBits().getKnownMinValue() != (Query.Types[1]).getSizeInBits().getKnownMinValue()); })
      .legalFor({{A_p0, A_i64}, {A_v2p0, A_v2i64}})
      .clampMaxNumElements(1, A_s64, 2);
  getActionDefinitionsBuilder(G_BITCAST)
      .legalFor({{A_s16, A_s16}})
      .legalFor({{A_s32, A_s32}, {A_s32, A_v2s16}, {A_s32, A_v4s8}, {A_v2s16, A_s32}, {A_v2s16, A_v2s16}, {A_v2s16, A_v4s8}, {A_v4s8, A_s32}, {A_v4s8, A_v2s16}, {A_v4s8, A_v4s8}})
      .legalFor({{A_s64, A_s64}, {A_s64, A_v8s8}, {A_s64, A_v4s16}, {A_s64, A_v2s32}, {A_v8s8, A_s64}, {A_v8s8, A_v8s8}, {A_v8s8, A_v4s16}, {A_v8s8, A_v2s32}, {A_v4s16, A_s64}, {A_v4s16, A_v8s8}, {A_v4s16, A_v4s16}, {A_v4s16, A_v2s32}, {A_v2s32, A_s64}, {A_v2s32, A_v8s8}, {A_v2s32, A_v4s16}, {A_v2s32, A_v2s32}})
      .legalFor({{A_s128, A_s128}, {A_s128, A_v16s8}, {A_s128, A_v8s16}, {A_s128, A_v4s32}, {A_s128, A_v2s64}, {A_s128, A_v2p0}, {A_v16s8, A_s128}, {A_v16s8, A_v16s8}, {A_v16s8, A_v8s16}, {A_v16s8, A_v4s32}, {A_v16s8, A_v2s64}, {A_v16s8, A_v2p0}, {A_v8s16, A_s128}, {A_v8s16, A_v16s8}, {A_v8s16, A_v8s16}, {A_v8s16, A_v4s32}, {A_v8s16, A_v2s64}, {A_v8s16, A_v2p0}, {A_v4s32, A_s128}, {A_v4s32, A_v16s8}, {A_v4s32, A_v8s16}, {A_v4s32, A_v4s32}, {A_v4s32, A_v2s64}, {A_v4s32, A_v2p0}, {A_v2s64, A_s128}, {A_v2s64, A_v16s8}, {A_v2s64, A_v8s16}, {A_v2s64, A_v4s32}, {A_v2s64, A_v2s64}, {A_v2s64, A_v2p0}, {A_v2p0, A_s128}, {A_v2p0, A_v16s8}, {A_v2p0, A_v8s16}, {A_v2p0, A_v4s32}, {A_v2p0, A_v2s64}, {A_v2p0, A_v2p0}})
      .coreCustomIf([=](const LegalityQuery &Query) { return (Query.Types[0].isScalar()) && (Query.Types[1].isVector()) && (((Query.Types[1]).getScalarSizeInBits() == 1)); })
      .coreLowerIf([=](const LegalityQuery &Query) { return ((Query.Types[0].isVector()) && (!(Query.Types[1].isVector()))) || ((!(Query.Types[0].isVector())) && (Query.Types[1].isVector())); })
      .coreLowerIf([=](const LegalityQuery &Query) { return (!((!((Query.Types[0].isVector()) && (!(Query.Types[0].isScalableVector())))) || (!((Query.Types[1].isVector()) && (!(Query.Types[1].isScalableVector())))))) && ((((((Query.Types[1]).isVector() ? (Query.Types[1]).getElementCount().getKnownMinValue() : 0) * llvm::PowerOf2Ceil(((Query.Types[0]).isVector() ? (Query.Types[0]).getElementCount().getKnownMinValue() : 0))) % ((Query.Types[0]).isVector() ? (Query.Types[0]).getElementCount().getKnownMinValue() : 0)) != 0)); })
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(0, A_s64, 2)
      .lower();
  getActionDefinitionsBuilder(G_VASTART)
      .legalFor({A_p0});
  getActionDefinitionsBuilder(G_VAARG)
      .customFor({{A_s8, A_p0}, {A_s16, A_p0}, {A_s32, A_p0}, {A_s64, A_p0}, {A_p0, A_p0}})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 8);
  getActionDefinitionsBuilder(G_ATOMIC_CMPXCHG_WITH_SUCCESS)
      .coreLowerIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_s8 || Query.Types[0] == A_s16 || Query.Types[0] == A_s32 || Query.Types[0] == A_s64 || Query.Types[0] == A_s128)) && ((Query.Types[2] == A_p0)); });
  getActionDefinitionsBuilder(G_ATOMIC_CMPXCHG)
      .legalFor(!A_UseOutlineAtomics, {{A_s32, A_p0}, {A_s64, A_p0}})
      .customFor(!A_UseOutlineAtomics, {{A_s128, A_p0}})
      .libcallFor(A_UseOutlineAtomics, {{A_s8, A_p0}, {A_s16, A_p0}, {A_s32, A_p0}, {A_s64, A_p0}, {A_s128, A_p0}})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64);
  getActionDefinitionsBuilder({G_ATOMICRMW_XCHG, G_ATOMICRMW_ADD, G_ATOMICRMW_SUB, G_ATOMICRMW_AND, G_ATOMICRMW_OR, G_ATOMICRMW_XOR})
      .legalFor(!A_UseOutlineAtomics, {{A_s32, A_p0}, {A_s64, A_p0}})
      .libcallFor(A_UseOutlineAtomics, {{A_s8, A_p0}, {A_s16, A_p0}, {A_s32, A_p0}, {A_s64, A_p0}})
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64);
  getActionDefinitionsBuilder({G_ATOMICRMW_MIN, G_ATOMICRMW_MAX, G_ATOMICRMW_UMIN, G_ATOMICRMW_UMAX})
      .coreLegalIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_s32 || Query.Types[0] == A_s64)) && ((Query.Types[1] == A_p0)); })
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64);
  getActionDefinitionsBuilder(G_BLOCK_ADDR)
      .legalFor({A_p0});
  getActionDefinitionsBuilder(G_MERGE_VALUES)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 8)
      .coreLowerIf([=](const LegalityQuery &Query) { return (Query.Types[0].isScalar()) && (!(llvm::isPowerOf2_32((Query.Types[0]).getSizeInBits().getKnownMinValue()))) && (((Query.Types[0]).getSizeInBits().getKnownMinValue() < 64)); })
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 32)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, A_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, A_s64)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s128)
      .coreLegalIf([=](const LegalityQuery &Query) { return ((((Query.Types[0]).getSizeInBits().getKnownMinValue() == 32)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 64)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 128))) && ((((Query.Types[1]).getSizeInBits().getKnownMinValue() == 8)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 16)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 32)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 64))); });
  getActionDefinitionsBuilder(G_UNMERGE_VALUES)
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 8)
      .coreLowerIf([=](const LegalityQuery &Query) { return (Query.Types[1].isScalar()) && (!(llvm::isPowerOf2_32((Query.Types[1]).getSizeInBits().getKnownMinValue()))) && (((Query.Types[1]).getSizeInBits().getKnownMinValue() < 64)); })
      .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 1, 32)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 0, A_s8)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, A_s64)
      .coreMinScalar([](const LegalityQuery &) { return true; }, 1, A_s32)
      .coreMaxScalar([](const LegalityQuery &) { return true; }, 1, A_s128)
      .coreLegalIf([=](const LegalityQuery &Query) { return ((((Query.Types[1]).getSizeInBits().getKnownMinValue() == 32)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 64)) || (((Query.Types[1]).getSizeInBits().getKnownMinValue() == 128))) && ((((Query.Types[0]).getSizeInBits().getKnownMinValue() == 8)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 16)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 32)) || (((Query.Types[0]).getSizeInBits().getKnownMinValue() == 64))); });
  if (A_HasSVE) {
    getActionDefinitionsBuilder(G_EXTRACT_VECTOR_ELT)
        .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typeTupleInSet(0, 1, 2, {{A_s16, A_nxv16s8, A_s64}, {A_s16, A_nxv8s16, A_s64}, {A_s32, A_nxv4s32, A_s64}, {A_s64, A_nxv2s64, A_s64}})(Query); });
  }
  getActionDefinitionsBuilder(G_EXTRACT_VECTOR_ELT)
      .coreUnsupportedIf([=](const LegalityQuery &Query) { return (!(Query.Types[1].isScalableVector())) && ((Query.Types[0] != (Query.Types[1]).getElementType())); })
      .coreMinScalar([](const LegalityQuery &) { return true; }, 2, A_s64)
      .coreCustomIf([=](const LegalityQuery &Query) { return ((Query.Types[1] == A_v8s8)) || ((Query.Types[1] == A_v16s8)) || ((Query.Types[1] == A_v2s16)) || ((Query.Types[1] == A_v4s16)) || ((Query.Types[1] == A_v8s16)) || ((Query.Types[1] == A_v2s32)) || ((Query.Types[1] == A_v4s32)) || ((Query.Types[1] == A_v2s64)) || ((Query.Types[1] == A_v2p0)); })
      .coreMinScalarOrElt([=](const LegalityQuery &Query) { return ((Query.Types[1].isVector()) && (!(Query.Types[1].isScalableVector()))) && ((((Query.Types[1]).isVector() ? (Query.Types[1]).getElementCount().getKnownMinValue() : 0) <= 2)); }, 0, A_s64)
      .coreMinScalarOrElt([=](const LegalityQuery &Query) { return ((Query.Types[1].isVector()) && (!(Query.Types[1].isScalableVector()))) && ((((Query.Types[1]).isVector() ? (Query.Types[1]).getElementCount().getKnownMinValue() : 0) <= 4)); }, 0, A_s32)
      .coreMinScalarOrElt([=](const LegalityQuery &Query) { return ((Query.Types[1].isVector()) && (!(Query.Types[1].isScalableVector()))) && ((((Query.Types[1]).isVector() ? (Query.Types[1]).getElementCount().getKnownMinValue() : 0) <= 8)); }, 0, A_s16)
      .coreMinScalarOrElt([=](const LegalityQuery &Query) { return ((Query.Types[1].isVector()) && (!(Query.Types[1].isScalableVector()))) && ((((Query.Types[1]).isVector() ? (Query.Types[1]).getElementCount().getKnownMinValue() : 0) <= 16)); }, 0, A_s8)
      .coreMinScalarOrElt([](const LegalityQuery &) { return true; }, 0, A_s8)
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .clampMaxNumElements(1, A_s64, 2)
      .clampMaxNumElements(1, A_s32, 4)
      .clampMaxNumElements(1, A_s16, 8)
      .clampMaxNumElements(1, A_s8, 16)
      .clampMaxNumElements(1, A_p0, 2)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[1]).getScalarSizeInBits() > 64); }, 1);
  getActionDefinitionsBuilder(G_INSERT_VECTOR_ELT)
      .coreLegalIf([=](const LegalityQuery &Query) { return (Query.Types[0] == A_v8s8 || Query.Types[0] == A_v16s8 || Query.Types[0] == A_v4s16 || Query.Types[0] == A_v8s16 || Query.Types[0] == A_v2s32 || Query.Types[0] == A_v4s32 || Query.Types[0] == A_v2s64 || Query.Types[0] == A_v2p0); })
      .legalFor(A_HasSVE, {{A_nxv16s8, A_s32, A_s64}, {A_nxv8s16, A_s32, A_s64}, {A_nxv4s32, A_s32, A_s64}, {A_nxv2s64, A_s64, A_s64}})
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreWidenVectorEltsToVectorMinSize([](const LegalityQuery &) { return true; }, 0, 64)
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(0, A_s64, 2)
      .clampMaxNumElements(0, A_p0, 2)
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0);
  getActionDefinitionsBuilder(G_BUILD_VECTOR)
      .legalFor({{A_v8s8, A_s8}, {A_v16s8, A_s8}, {A_v4s16, A_s16}, {A_v8s16, A_s16}, {A_v2s32, A_s32}, {A_v4s32, A_s32}, {A_v2s64, A_s64}, {A_v2p0, A_p0}})
      .clampMinNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(0, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(0, A_s64, (A_v2s64).getNumElements())
      .coreMinScalarOrElt([](const LegalityQuery &) { return true; }, 0, A_s8)
      .coreWidenVectorEltsToVectorMinSize([](const LegalityQuery &) { return true; }, 0, 64)
      .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .minScalarSameAs(1, 0);
  getActionDefinitionsBuilder(G_BUILD_VECTOR_TRUNC)
      .lower();
  getActionDefinitionsBuilder(G_SPLAT_VECTOR)
      .legalFor({{A_v8s8, A_s8}, {A_v8s8, A_s32}, {A_v16s8, A_s8}, {A_v16s8, A_s32}, {A_v4s16, A_s16}, {A_v4s16, A_s32}, {A_v8s16, A_s16}, {A_v8s16, A_s32}, {A_v2s32, A_s32}, {A_v4s32, A_s32}, {A_v2s64, A_s64}, {A_v2p0, A_p0}});
  getActionDefinitionsBuilder(G_SHUFFLE_VECTOR)
      .coreLegalIf([=](const LegalityQuery &Query) { return (!((Query.Types[0] != Query.Types[1]))) && ((Query.Types[0] == A_v8s8 || Query.Types[0] == A_v16s8 || Query.Types[0] == A_v4s16 || Query.Types[0] == A_v8s16 || Query.Types[0] == A_v2s32 || Query.Types[0] == A_v4s32 || Query.Types[0] == A_v2s64)); })
      .coreMoreTo([=](const LegalityQuery &Query) { return (((Query.Types[0]).isVector() ? (Query.Types[0]).getElementCount().getKnownMinValue() : 0) > ((Query.Types[1]).isVector() ? (Query.Types[1]).getElementCount().getKnownMinValue() : 0)); }, 1, [=](const LegalityQuery &Query) { return std::make_pair(1, Query.Types[0]); })
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .coreMoreTo([=](const LegalityQuery &Query) { return (((Query.Types[0]).isVector() ? (Query.Types[0]).getElementCount().getKnownMinValue() : 0) < ((Query.Types[1]).isVector() ? (Query.Types[1]).getElementCount().getKnownMinValue() : 0)); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, Query.Types[1]); })
      .coreWidenScalarOrEltToNextPow2OrMinSize([](const LegalityQuery &) { return true; }, 0, 8)
      .clampMinNumElements(0, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(0, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(0, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(0, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMaxNumElements(0, A_s32, (A_v4s32).getNumElements())
      .clampMinNumElements(0, A_s64, (A_v2s64).getNumElements())
      .clampMaxNumElements(0, A_s64, (A_v2s64).getNumElements())
      .coreScalarizeIf([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 64); }, 0)
      .coreBitcastToType([=](const LegalityQuery &Query) { return Query.Types[0].isPointerVector(); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementType(A_i64)); });
  getActionDefinitionsBuilder(G_CONCAT_VECTORS)
      .legalFor({{A_v16s8, A_v8s8}, {A_v8s16, A_v4s16}, {A_v4s32, A_v2s32}})
      .coreCustomIf([=](const LegalityQuery &Query) { return ((Query.Types[0].isVector()) && (!(Query.Types[0].isScalableVector()))) && (((Query.Types[0]).getScalarSizeInBits() < 8)); })
      .coreBitcastToType([=](const LegalityQuery &Query) { return ((Query.Types[0].isVector()) && (!(Query.Types[0].isScalableVector()))) && ((Query.Types[1].isVector()) && (!(Query.Types[1].isScalableVector()))) && (((Query.Types[0]).getScalarSizeInBits() >= 8)) && (llvm::isPowerOf2_32((Query.Types[0]).getScalarSizeInBits())) && (((Query.Types[0]).getSizeInBits().getKnownMinValue() <= 128)) && (((Query.Types[1]).getSizeInBits().getKnownMinValue() <= 64)); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, ((Query.Types[0]).changeElementCount((Query.Types[0]).getElementCount().divideCoefficientBy(((Query.Types[1]).isVector() ? (Query.Types[1]).getElementCount().getKnownMinValue() : 0)))).changeElementSize((Query.Types[1]).getSizeInBits().getKnownMinValue())); });
  getActionDefinitionsBuilder(G_EXTRACT_SUBVECTOR)
      .coreLegalIf([=](const LegalityQuery &Query) { return LegalityPredicates::typePairInSet(0, 1, {{A_v8s8, A_v16s8}, {A_v4s16, A_v8s16}, {A_v2s32, A_v4s32}})(Query); })
      .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 0)
      .clampMaxNumElements(0, A_s8, 16)
      .clampMaxNumElements(0, A_s16, 8)
      .clampMaxNumElements(0, A_s32, 4)
      .clampMinNumElements(1, A_s8, (A_v8s8).getNumElements())
      .clampMaxNumElements(1, A_s8, (A_v16s8).getNumElements())
      .clampMinNumElements(1, A_s16, (A_v4s16).getNumElements())
      .clampMaxNumElements(1, A_s16, (A_v8s16).getNumElements())
      .clampMinNumElements(1, A_s32, (A_v2s32).getNumElements())
      .clampMaxNumElements(1, A_s32, (A_v4s32).getNumElements())
      .lower()
      .immIdx(0);
  if (A_HasSVE) {
    getActionDefinitionsBuilder(G_SPLAT_VECTOR)
        .legalFor({{A_nxv4s32, A_s32}, {A_nxv2s64, A_s64}});
  }
  getActionDefinitionsBuilder(G_JUMP_TABLE)
      .legalFor({A_p0});
  getActionDefinitionsBuilder(G_BRJT)
      .legalFor({{A_p0, A_s64}});
  getActionDefinitionsBuilder({G_TRAP, G_DEBUGTRAP, G_UBSANTRAP})
      .alwaysLegal();
  getActionDefinitionsBuilder(G_DYN_STACKALLOC)
      .custom();
  getActionDefinitionsBuilder({G_STACKSAVE, G_STACKRESTORE})
      .lower();
  if (A_hasMOPS) {
    getActionDefinitionsBuilder(G_BZERO)
        .unsupported();
  }
  if (A_hasMOPS) {
    getActionDefinitionsBuilder(G_MEMSET)
        .coreLegalIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_p0)) && ((Query.Types[1] == A_s64)) && ((Query.Types[2] == A_s64)); })
        .coreCustomIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_p0)) && ((Query.Types[1] == A_s8)) && ((Query.Types[2] == A_s64)); })
        .immIdx(0);
  }
  if (A_hasMOPS) {
    getActionDefinitionsBuilder({G_MEMCPY, G_MEMMOVE})
        .coreLegalIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_p0)) && ((Query.Types[1] == A_p0)) && ((Query.Types[2] == A_s64)); })
        .immIdx(0);
  }
  if (A_hasMOPS) {
    getActionDefinitionsBuilder(G_MEMCPY_INLINE)
        .coreLegalIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_p0)) && ((Query.Types[1] == A_p0)) && ((Query.Types[2] == A_s64)); });
  }
  if (A_hasMOPS) {
    getActionDefinitionsBuilder(G_MEMSET_INLINE)
        .coreLegalIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_p0)) && ((Query.Types[1] == A_s64)) && ((Query.Types[2] == A_s64)); })
        .coreCustomIf([=](const LegalityQuery &Query) { return ((Query.Types[0] == A_p0)) && ((Query.Types[1] == A_s8)) && ((Query.Types[2] == A_s64)); });
  }
  if (!A_hasMOPS) {
    getActionDefinitionsBuilder({G_BZERO, G_MEMCPY, G_MEMMOVE, G_MEMSET})
        .libcall();
  }
  getActionDefinitionsBuilder(G_VECREDUCE_FADD)
      .legalFor({{A_f32, A_v2f32}, {A_f32, A_v4f32}, {A_f64, A_v2f64}})
      .legalFor(A_HasFP16, {{A_f16, A_v4f16}, {A_f16, A_v8f16}})
      .coreWidenTo([=](const LegalityQuery &Query) { return ((!(A_HasFP16)) && (((Query.Types[0]).getScalarType()).isFloat16())) || (((Query.Types[0]).getScalarType()).isBFloat16()); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementType(A_f32)); })
      .clampMaxNumElements(1, A_s64, 2)
      .clampMaxNumElements(1, A_s32, 4)
      .clampMaxNumElements(1, A_s16, 8)
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .scalarize(1)
      .lower();
  getActionDefinitionsBuilder(G_VECREDUCE_FMUL)
      .coreWidenTo([=](const LegalityQuery &Query) { return ((!(A_HasFP16)) && (((Query.Types[0]).getScalarType()).isFloat16())) || (((Query.Types[0]).getScalarType()).isBFloat16()); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementType(A_f32)); })
      .clampMaxNumElements(1, A_s64, 2)
      .clampMaxNumElements(1, A_s32, 4)
      .clampMaxNumElements(1, A_s16, 8)
      .clampMaxNumElements(1, A_s32, 2)
      .clampMaxNumElements(1, A_s16, 4)
      .scalarize(1)
      .lower();
  getActionDefinitionsBuilder({G_VECREDUCE_SEQ_FADD, G_VECREDUCE_SEQ_FMUL})
      .scalarize(2)
      .lower();
  getActionDefinitionsBuilder(G_VECREDUCE_ADD)
      .legalFor({{A_i8, A_v8i8}, {A_i8, A_v16i8}, {A_i16, A_v4i16}, {A_i16, A_v8i16}, {A_i32, A_v2i32}, {A_i32, A_v4i32}, {A_i64, A_v2i64}})
      .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 1)
      .clampMaxNumElements(1, A_s64, 2)
      .clampMaxNumElements(1, A_s32, 4)
      .clampMaxNumElements(1, A_s16, 8)
      .clampMaxNumElements(1, A_s8, 16)
      .coreWidenVectorEltsToVectorMinSize([](const LegalityQuery &) { return true; }, 1, 64)
      .scalarize(1);
  getActionDefinitionsBuilder({G_VECREDUCE_FMIN, G_VECREDUCE_FMAX, G_VECREDUCE_FMINIMUM, G_VECREDUCE_FMAXIMUM})
      .legalFor({{A_f32, A_v2f32}, {A_f32, A_v4f32}, {A_f64, A_v2f64}})
      .legalFor(A_HasFP16, {{A_f16, A_v4f16}, {A_f16, A_v8f16}})
      .coreWidenTo([=](const LegalityQuery &Query) { return ((!(A_HasFP16)) && (((Query.Types[0]).getScalarType()).isFloat16())) || (((Query.Types[0]).getScalarType()).isBFloat16()); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementType(A_f32)); })
      .clampMaxNumElements(1, A_s64, 2)
      .clampMaxNumElements(1, A_s32, 4)
      .clampMaxNumElements(1, A_s16, 8)
      .scalarize(1)
      .lower();
  getActionDefinitionsBuilder(G_VECREDUCE_MUL)
      .clampMaxNumElements(1, A_s32, 2)
      .clampMaxNumElements(1, A_s16, 4)
      .clampMaxNumElements(1, A_s8, 8)
      .scalarize(1)
      .lower();
  getActionDefinitionsBuilder({G_VECREDUCE_SMIN, G_VECREDUCE_SMAX, G_VECREDUCE_UMIN, G_VECREDUCE_UMAX})
      .legalFor({{A_i8, A_v8i8}, {A_i8, A_v16i8}, {A_i16, A_v4i16}, {A_i16, A_v8i16}, {A_i32, A_v2i32}, {A_i32, A_v4i32}})
      .coreMoreElementsToNextPow2([=](const LegalityQuery &Query) { return (Query.Types[1].isVector()) && (((Query.Types[1]).getElementType() != A_s8)) && (((((Query.Types[1]).isVector() ? (Query.Types[1]).getElementCount().getKnownMinValue() : 0) % 2) != 0)); }, 1)
      .clampMaxNumElements(1, A_s64, 2)
      .clampMaxNumElements(1, A_s32, 4)
      .clampMaxNumElements(1, A_s16, 8)
      .clampMaxNumElements(1, A_s8, 16)
      .scalarize(1)
      .lower();
  getActionDefinitionsBuilder({G_VECREDUCE_OR, G_VECREDUCE_AND, G_VECREDUCE_XOR})
      .coreFewerTo([=](const LegalityQuery &Query) { return (!(Query.Types[1].isScalar())) && ((!(!(llvm::isPowerOf2_32(((Query.Types[1]).isVector() ? (Query.Types[1]).getElementCount().getKnownMinValue() : 0))))) && (((Query.Types[1]).getSizeInBits().getKnownMinValue() > 64))); }, 1, [=](const LegalityQuery &Query) { return std::make_pair(1, (Query.Types[1]).changeElementCount((Query.Types[1]).getElementCount().divideCoefficientBy(2))); })
      .scalarize(1)
      .lower();
  getActionDefinitionsBuilder(G_VECTOR_COMPRESS)
      .lower();
  getActionDefinitionsBuilder({G_GET_FPENV, G_SET_FPENV, G_RESET_FPENV, G_GET_FPMODE, G_SET_FPMODE, G_RESET_FPMODE})
      .libcall();
  getActionDefinitionsBuilder({G_GET_ROUNDING, G_SET_ROUNDING})
      .customFor({A_s32});
  getActionDefinitionsBuilder(G_IS_FPCLASS)
      .lower();
  getActionDefinitionsBuilder(G_PREFETCH)
      .custom();
  getActionDefinitionsBuilder({G_SCMP, G_UCMP})
      .lower();
  getActionDefinitionsBuilder({G_INTRINSIC, G_INTRINSIC_W_SIDE_EFFECTS})
      .alwaysLegal();
  getActionDefinitionsBuilder(G_FENCE)
      .alwaysLegal();
  getActionDefinitionsBuilder(G_INVOKE_REGION_START)
      .alwaysLegal();
