/*===- TableGen'erated file -------------------------------------*- C++ -*-===*\
|*                                                                            *|
|* GlobalISel legalizer rule fragments                                        *|
|*                                                                            *|
|* Automatically generated file, do not edit!                                 *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

  bool Is64Bit = Subtarget.is64Bit();
  bool HasCMOV = Subtarget.canUseCMOV();
  bool HasSSE1 = Subtarget.hasSSE1();
  bool HasSSE2 = Subtarget.hasSSE2();
  bool HasSSE41 = Subtarget.hasSSE41();
  bool HasAVX = Subtarget.hasAVX();
  bool HasAVX2 = Subtarget.hasAVX2();
  bool HasAVX512 = Subtarget.hasAVX512();
  bool HasVLX = Subtarget.hasVLX();
  bool HasDQI = Subtarget.hasAVX512() && Subtarget.hasDQI();
  bool HasBWI = Subtarget.hasAVX512() && Subtarget.hasBWI();
  bool UseX87 = !Subtarget.useSoftFloat() && Subtarget.hasX87();
  bool HasPOPCNT = Subtarget.hasPOPCNT();
  bool HasVPOPCNTDQ = Subtarget.hasVPOPCNTDQ();
  bool HasLZCNT = Subtarget.hasLZCNT();
  bool HasBMI = Subtarget.hasBMI();

  const LLT X86_p0 = LLT::pointer(0, Subtarget.isTarget64BitLP64() ? 64 : 32);
  const LLT X86_s1 = LLT::scalar(1);
  const LLT X86_s8 = LLT::scalar(8);
  const LLT X86_s16 = LLT::scalar(16);
  const LLT X86_s32 = LLT::scalar(32);
  const LLT X86_s64 = LLT::scalar(64);
  const LLT X86_s80 = LLT::scalar(80);
  const LLT X86_s128 = LLT::scalar(128);
  const LLT X86_sMaxScalar = Subtarget.is64Bit() ? X86_s64 : X86_s32;
  const LLT X86_v2s32 = LLT::fixed_vector(2, 32);
  const LLT X86_v4s8 = LLT::fixed_vector(4, 8);
  const LLT X86_v16s8 = LLT::fixed_vector(16, 8);
  const LLT X86_v8s16 = LLT::fixed_vector(8, 16);
  const LLT X86_v4s32 = LLT::fixed_vector(4, 32);
  const LLT X86_v2s64 = LLT::fixed_vector(2, 64);
  const LLT X86_v2p0 = LLT::fixed_vector(2, X86_p0);
  const LLT X86_v32s8 = LLT::fixed_vector(32, 8);
  const LLT X86_v16s16 = LLT::fixed_vector(16, 16);
  const LLT X86_v8s32 = LLT::fixed_vector(8, 32);
  const LLT X86_v4s64 = LLT::fixed_vector(4, 64);
  const LLT X86_v4p0 = LLT::fixed_vector(4, X86_p0);
  const LLT X86_v64s8 = LLT::fixed_vector(64, 8);
  const LLT X86_v32s16 = LLT::fixed_vector(32, 16);
  const LLT X86_v16s32 = LLT::fixed_vector(16, 32);
  const LLT X86_v8s64 = LLT::fixed_vector(8, 64);
  const LLT X86_s8MaxVector = HasAVX512 ? X86_v64s8 : HasAVX ? X86_v32s8 : X86_v16s8;
  const LLT X86_s16MaxVector = HasAVX512 ? X86_v32s16 : HasAVX ? X86_v16s16 : X86_v8s16;
  const LLT X86_s32MaxVector = HasAVX512 ? X86_v16s32 : HasAVX ? X86_v8s32 : X86_v4s32;
  const LLT X86_s64MaxVector = HasAVX512 ? X86_v8s64 : HasAVX ? X86_v4s64 : X86_v2s64;

  const std::initializer_list<LLT> X86_IntTypes32 = {X86_s8, X86_s16, X86_s32, X86_p0};
  const std::initializer_list<LLT> X86_IntTypes64 = {X86_s8, X86_s16, X86_s32, X86_s64, X86_p0};
  const std::initializer_list<LLT> X86_PtrTypes32 = {X86_s1, X86_s8, X86_s16, X86_s32};
  const std::initializer_list<LLT> X86_PtrTypes64 = {X86_s1, X86_s8, X86_s16, X86_s32, X86_s64};

  // implicit/constants
  // 32/64-bits needs support for s64/s128 to handle cases:
  // s64 = EXTEND (G_IMPLICIT_DEF s32) -> s64 = G_IMPLICIT_DEF
  // s128 = EXTEND (G_IMPLICIT_DEF s32/s64) -> s128 = G_IMPLICIT_DEF
  getActionDefinitionsBuilder({G_IMPLICIT_DEF, G_PHI, G_FREEZE, G_CONSTANT_FOLD_BARRIER})
      .legalFor({X86_p0, X86_s1, X86_s8, X86_s16, X86_s32, X86_s64})
      .legalFor(UseX87, {X86_s80})
      .legalFor(Is64Bit, {X86_s128})
      .legalFor(HasSSE2, {X86_v16s8, X86_v8s16, X86_v4s32, X86_v2s64})
      .legalFor(HasAVX, {X86_v32s8, X86_v16s16, X86_v8s32, X86_v4s64})
      .legalFor(HasAVX512, {X86_v64s8, X86_v32s16, X86_v16s32, X86_v8s64})
      .widenScalarOrEltToNextPow2(0, /*Min=*/8)
      .clampScalarOrElt(0, X86_s8, X86_sMaxScalar)
      .moreElementsToNextPow2(0)
      .clampNumElements(0, X86_v16s8, X86_s8MaxVector)
      .clampNumElements(0, X86_v8s16, X86_s16MaxVector)
      .clampNumElements(0, X86_v4s32, X86_s32MaxVector)
      .clampNumElements(0, X86_v2s64, X86_s64MaxVector)
      .clampMaxNumElements(0, X86_p0, Is64Bit ? X86_s64MaxVector.getNumElements() : X86_s32MaxVector.getNumElements())
      .scalarizeIf(scalarOrEltWiderThan(0, 64), 0);
  getActionDefinitionsBuilder(G_CONSTANT)
      .legalFor({X86_p0, X86_s8, X86_s16, X86_s32})
      .legalFor(Is64Bit, {X86_s64})
      .widenScalarToNextPow2(0, /*Min=*/8)
      .clampScalar(0, X86_s8, X86_sMaxScalar);
  getActionDefinitionsBuilder({G_LROUND, G_LLROUND})
      .minScalarIf(typeIs(1, X86_s16), 1, X86_s32)
      .libcall();
  getActionDefinitionsBuilder({G_FCOS, G_FCOSH, G_FACOS, G_FSIN, G_FSINH, G_FASIN, G_FTAN, G_FTANH, G_FATAN, G_FATAN2, G_FPOW, G_FEXP, G_FEXP2, G_FEXP10, G_FLOG, G_FLOG2, G_FLOG10, G_FPOWI, G_FSINCOS, G_FCEIL, G_FFLOOR})
      .libcall();
  getActionDefinitionsBuilder(G_FNEG)
      .legalFor(UseX87 && !(HasSSE1), {X86_s32})
      .legalFor(UseX87 && !(HasSSE2), {X86_s64})
      .legalFor(UseX87, {X86_s80})
      .lower();
  getActionDefinitionsBuilder(G_FSQRT)
      .legalFor(HasSSE1 || UseX87, {X86_s32})
      .legalFor(HasSSE2 || UseX87, {X86_s64})
      .legalFor(UseX87, {X86_s80});
  getActionDefinitionsBuilder({G_GET_ROUNDING, G_SET_ROUNDING})
      .customFor({X86_s32});
  getActionDefinitionsBuilder(G_MERGE_VALUES)
      .widenScalarToNextPow2(1, /*Min=*/8)
      .widenScalarToNextPow2(0, /*Min=*/16)
      .minScalar(1, X86_s8)
      .minScalar(0, X86_s32)
      .legalIf([=](const LegalityQuery &Query) { return (sizeIs(0, 16)(Query) || sizeIs(0, 32)(Query) || sizeIs(0, 64)(Query) || sizeIs(0, 128)(Query) || sizeIs(0, 256)(Query) || sizeIs(0, 512)(Query)) && (sizeIs(1, 8)(Query) || sizeIs(1, 16)(Query) || sizeIs(1, 32)(Query) || sizeIs(1, 64)(Query) || sizeIs(1, 128)(Query) || sizeIs(1, 256)(Query)); });
  getActionDefinitionsBuilder(G_UNMERGE_VALUES)
      .widenScalarToNextPow2(0, /*Min=*/8)
      .widenScalarToNextPow2(1, /*Min=*/16)
      .minScalar(0, X86_s8)
      .minScalar(1, X86_s32)
      .legalIf([=](const LegalityQuery &Query) { return (sizeIs(1, 16)(Query) || sizeIs(1, 32)(Query) || sizeIs(1, 64)(Query) || sizeIs(1, 128)(Query) || sizeIs(1, 256)(Query) || sizeIs(1, 512)(Query)) && (sizeIs(0, 8)(Query) || sizeIs(0, 16)(Query) || sizeIs(0, 32)(Query) || sizeIs(0, 64)(Query) || sizeIs(0, 128)(Query) || sizeIs(0, 256)(Query)); });
  getActionDefinitionsBuilder({G_UMIN, G_UMAX, G_SMIN, G_SMAX})
      .widenScalarToNextPow2(0, /*Min=*/32)
      .lower();
  // integer addition/subtraction
  getActionDefinitionsBuilder({G_ADD, G_SUB})
      .legalFor({X86_s8, X86_s16, X86_s32})
      .legalFor(Is64Bit, {X86_s64})
      .legalFor(HasSSE2, {X86_v16s8, X86_v8s16, X86_v4s32, X86_v2s64})
      .legalFor(HasAVX2, {X86_v32s8, X86_v16s16, X86_v8s32, X86_v4s64})
      .legalFor(HasAVX512, {X86_v16s32, X86_v8s64})
      .legalFor(HasBWI, {X86_v64s8, X86_v32s16})
      .clampMinNumElements(0, X86_s8, 16)
      .clampMinNumElements(0, X86_s16, 8)
      .clampMinNumElements(0, X86_s32, 4)
      .clampMinNumElements(0, X86_s64, 2)
      .clampMaxNumElements(0, X86_s8, HasBWI ? 64 : (HasAVX2 ? 32 : 16))
      .clampMaxNumElements(0, X86_s16, HasBWI ? 32 : (HasAVX2 ? 16 : 8))
      .clampMaxNumElements(0, X86_s32, HasAVX512 ? 16 : (HasAVX2 ? 8 : 4))
      .clampMaxNumElements(0, X86_s64, HasAVX512 ? 8 : (HasAVX2 ? 4 : 2))
      .widenScalarToNextPow2(0, /*Min=*/32)
      .clampScalar(0, X86_s8, X86_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder({G_UADDE, G_UADDO, G_USUBE, G_USUBO})
      .legalFor({{X86_s8, X86_s8}, {X86_s16, X86_s8}, {X86_s32, X86_s8}})
      .legalFor(Is64Bit, {{X86_s64, X86_s8}})
      .widenScalarToNextPow2(0, /*Min=*/32)
      .clampScalar(0, X86_s8, X86_sMaxScalar)
      .clampScalar(1, X86_s8, X86_s8)
      .scalarize(0);
  // integer multiply
  getActionDefinitionsBuilder(G_MUL)
      .legalFor({X86_s8, X86_s16, X86_s32})
      .legalFor(Is64Bit, {X86_s64})
      .legalFor(HasSSE2, {X86_v8s16})
      .legalFor(HasSSE41, {X86_v4s32})
      .legalFor(HasAVX2, {X86_v16s16, X86_v8s32})
      .legalFor(HasAVX512, {X86_v16s32})
      .legalFor(HasDQI, {X86_v8s64})
      .legalFor(HasDQI && HasVLX, {X86_v2s64, X86_v4s64})
      .legalFor(HasBWI, {X86_v32s16})
      .clampMinNumElements(0, X86_s16, 8)
      .clampMinNumElements(0, X86_s32, 4)
      .clampMinNumElements(0, X86_s64, HasVLX ? 2 : 8)
      .clampMaxNumElements(0, X86_s16, HasBWI ? 32 : (HasAVX2 ? 16 : 8))
      .clampMaxNumElements(0, X86_s32, HasAVX512 ? 16 : (HasAVX2 ? 8 : 4))
      .clampMaxNumElements(0, X86_s64, 8)
      .widenScalarToNextPow2(0, /*Min=*/32)
      .clampScalar(0, X86_s8, X86_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder({G_SMULH, G_UMULH})
      .legalFor({X86_s8, X86_s16, X86_s32})
      .legalFor(Is64Bit, {X86_s64})
      .widenScalarToNextPow2(0, /*Min=*/32)
      .clampScalar(0, X86_s8, X86_sMaxScalar)
      .scalarize(0);
  // integer divisions
  getActionDefinitionsBuilder({G_SDIV, G_SREM, G_UDIV, G_UREM})
      .legalFor({X86_s8, X86_s16, X86_s32})
      .legalFor(Is64Bit, {X86_s64})
      .libcallFor({X86_s64})
      .clampScalar(0, X86_s8, X86_sMaxScalar);
  // integer shifts
  getActionDefinitionsBuilder({G_SHL, G_LSHR, G_ASHR})
      .legalFor({{X86_s8, X86_s8}, {X86_s16, X86_s8}, {X86_s32, X86_s8}})
      .legalFor(Is64Bit, {{X86_s64, X86_s8}})
      .clampScalar(0, X86_s8, X86_sMaxScalar)
      .clampScalar(1, X86_s8, X86_s8);
  // integer logic
  getActionDefinitionsBuilder({G_AND, G_OR, G_XOR})
      .legalFor({X86_s8, X86_s16, X86_s32})
      .legalFor(Is64Bit, {X86_s64})
      .legalFor(HasSSE2, {X86_v16s8, X86_v8s16, X86_v4s32, X86_v2s64})
      .legalFor(HasAVX, {X86_v32s8, X86_v16s16, X86_v8s32, X86_v4s64})
      .legalFor(HasAVX512, {X86_v64s8, X86_v32s16, X86_v16s32, X86_v8s64})
      .clampNumElements(0, X86_v16s8, X86_s8MaxVector)
      .clampNumElements(0, X86_v8s16, X86_s16MaxVector)
      .clampNumElements(0, X86_v4s32, X86_s32MaxVector)
      .clampNumElements(0, X86_v2s64, X86_s64MaxVector)
      .widenScalarToNextPow2(0, /*Min=*/32)
      .clampScalar(0, X86_s8, X86_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder(G_ICMP)
      .legalForCartesianProduct({X86_s8}, Is64Bit ? X86_IntTypes64 : X86_IntTypes32)
      .clampScalar(0, X86_s8, X86_s8)
      .widenScalarToNextPow2(1, /*Min=*/8)
      .clampScalar(1, X86_s8, X86_sMaxScalar);
  // bswap
  getActionDefinitionsBuilder(G_BSWAP)
      .legalFor({X86_s32})
      .legalFor(Is64Bit, {X86_s64})
      .widenScalarToNextPow2(0, /*Min=*/32)
      .clampScalar(0, X86_s32, X86_sMaxScalar);
  // popcount
  getActionDefinitionsBuilder(G_CTPOP)
      .legalFor(HasPOPCNT, {{X86_s16, X86_s16}, {X86_s32, X86_s32}})
      .legalFor(HasPOPCNT && Is64Bit, {{X86_s64, X86_s64}})
      .legalFor(HasVPOPCNTDQ, {{X86_v16s32, X86_v16s32}, {X86_v8s64, X86_v8s64}})
      .legalFor(HasVPOPCNTDQ && HasVLX, {{X86_v4s32, X86_v4s32}, {X86_v8s32, X86_v8s32}, {X86_v2s64, X86_v2s64}, {X86_v4s64, X86_v4s64}})
      .widenScalarToNextPow2(1, /*Min=*/16)
      .clampScalar(1, X86_s16, X86_sMaxScalar)
      .scalarSameSizeAs(0, 1);
  // count leading zeros (LZCNT)
  getActionDefinitionsBuilder(G_CTLZ)
      .legalFor(HasLZCNT, {{X86_s16, X86_s16}, {X86_s32, X86_s32}})
      .legalFor(HasLZCNT && Is64Bit, {{X86_s64, X86_s64}})
      .widenScalarToNextPow2(1, /*Min=*/16)
      .clampScalar(1, X86_s16, X86_sMaxScalar)
      .scalarSameSizeAs(0, 1);
  // count trailing zeros
  getActionDefinitionsBuilder(G_CTTZ_ZERO_POISON)
      .legalFor({{X86_s16, X86_s16}, {X86_s32, X86_s32}})
      .legalFor(Is64Bit, {{X86_s64, X86_s64}})
      .widenScalarToNextPow2(1, /*Min=*/16)
      .clampScalar(1, X86_s16, X86_sMaxScalar)
      .scalarSameSizeAs(0, 1);
  getActionDefinitionsBuilder(G_CTTZ)
      .legalFor(HasBMI, {{X86_s16, X86_s16}, {X86_s32, X86_s32}})
      .legalFor(HasBMI && Is64Bit, {{X86_s64, X86_s64}})
      .widenScalarToNextPow2(1, /*Min=*/16)
      .clampScalar(1, X86_s16, X86_sMaxScalar)
      .scalarSameSizeAs(0, 1);
  getActionDefinitionsBuilder(G_BR)
      .alwaysLegal();
  getActionDefinitionsBuilder(G_BRCOND)
      .legalFor({X86_s1});
  getActionDefinitionsBuilder(G_PTRTOINT)
      .legalForCartesianProduct(Is64Bit ? X86_PtrTypes64 : X86_PtrTypes32, {X86_p0})
      .maxScalar(0, X86_sMaxScalar)
      .widenScalarToNextPow2(0, /*Min=*/8);
  getActionDefinitionsBuilder(G_INTTOPTR)
      .legalFor({{X86_p0, X86_sMaxScalar}});
  getActionDefinitionsBuilder(G_CONSTANT_POOL)
      .legalFor({X86_p0});
  getActionDefinitionsBuilder(G_PTR_ADD)
      .legalFor({{X86_p0, X86_s32}})
      .legalFor(Is64Bit, {{X86_p0, X86_s64}})
      .widenScalarToNextPow2(1, /*Min=*/32)
      .clampScalar(1, X86_s32, X86_sMaxScalar);
  getActionDefinitionsBuilder(G_FRAME_INDEX)
      .legalFor({X86_p0});
  getActionDefinitionsBuilder(G_GLOBAL_VALUE)
      .customFor({X86_p0});
  // load/store: add more corner cases
  getActionDefinitionsBuilder(G_LOAD)
      .legalForTypesWithMemDesc({{X86_s8, X86_p0, X86_s8, 1}, {X86_s16, X86_p0, X86_s16, 1}, {X86_s32, X86_p0, X86_s32, 1}, {X86_s80, X86_p0, X86_s80, 1}, {X86_p0, X86_p0, X86_p0, 1}, {X86_v4s8, X86_p0, X86_v4s8, 1}})
      .legalForTypesWithMemDesc(Is64Bit, {{X86_s64, X86_p0, X86_s64, 1}, {X86_v2s32, X86_p0, X86_v2s32, 1}})
      .legalForTypesWithMemDesc(HasSSE1, {{X86_v4s32, X86_p0, X86_v4s32, 1}})
      .legalForTypesWithMemDesc(HasSSE2, {{X86_v16s8, X86_p0, X86_v16s8, 1}, {X86_v8s16, X86_p0, X86_v8s16, 1}, {X86_v2s64, X86_p0, X86_v2s64, 1}, {X86_v2p0, X86_p0, X86_v2p0, 1}})
      .legalForTypesWithMemDesc(HasAVX, {{X86_v32s8, X86_p0, X86_v32s8, 1}, {X86_v16s16, X86_p0, X86_v16s16, 1}, {X86_v8s32, X86_p0, X86_v8s32, 1}, {X86_v4s64, X86_p0, X86_v4s64, 1}, {X86_v4p0, X86_p0, X86_v4p0, 1}})
      .legalForTypesWithMemDesc(HasAVX512, {{X86_v64s8, X86_p0, X86_v64s8, 1}, {X86_v32s16, X86_p0, X86_v32s16, 1}, {X86_v16s32, X86_p0, X86_v16s32, 1}, {X86_v8s64, X86_p0, X86_v8s64, 1}})
      .legalForTypesWithMemDesc({{X86_s8, X86_p0, X86_s1, 1}, {X86_s16, X86_p0, X86_s8, 1}, {X86_s32, X86_p0, X86_s8, 1}, {X86_s32, X86_p0, X86_s16, 1}})
      .legalForTypesWithMemDesc(Is64Bit, {{X86_s64, X86_p0, X86_s8, 1}, {X86_s64, X86_p0, X86_s16, 1}, {X86_s64, X86_p0, X86_s32, 1}})
      .widenScalarToNextPow2(0, /*Min=*/8)
      .clampScalar(0, X86_s8, X86_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder(G_STORE)
      .legalForTypesWithMemDesc({{X86_s8, X86_p0, X86_s8, 1}, {X86_s16, X86_p0, X86_s16, 1}, {X86_s32, X86_p0, X86_s32, 1}, {X86_s80, X86_p0, X86_s80, 1}, {X86_p0, X86_p0, X86_p0, 1}, {X86_v4s8, X86_p0, X86_v4s8, 1}})
      .legalForTypesWithMemDesc(Is64Bit, {{X86_s64, X86_p0, X86_s64, 1}, {X86_v2s32, X86_p0, X86_v2s32, 1}})
      .legalForTypesWithMemDesc(HasSSE1, {{X86_v4s32, X86_p0, X86_v4s32, 1}})
      .legalForTypesWithMemDesc(HasSSE2, {{X86_v16s8, X86_p0, X86_v16s8, 1}, {X86_v8s16, X86_p0, X86_v8s16, 1}, {X86_v2s64, X86_p0, X86_v2s64, 1}, {X86_v2p0, X86_p0, X86_v2p0, 1}})
      .legalForTypesWithMemDesc(HasAVX, {{X86_v32s8, X86_p0, X86_v32s8, 1}, {X86_v16s16, X86_p0, X86_v16s16, 1}, {X86_v8s32, X86_p0, X86_v8s32, 1}, {X86_v4s64, X86_p0, X86_v4s64, 1}, {X86_v4p0, X86_p0, X86_v4p0, 1}})
      .legalForTypesWithMemDesc(HasAVX512, {{X86_v64s8, X86_p0, X86_v64s8, 1}, {X86_v32s16, X86_p0, X86_v32s16, 1}, {X86_v16s32, X86_p0, X86_v16s32, 1}, {X86_v8s64, X86_p0, X86_v8s64, 1}})
      .customIf(
          [=](const LegalityQuery &Query) {
            return Query.Types[0] != Query.MMODescrs[0].MemoryTy;
          })
      .widenScalarToNextPow2(0, /*Min=*/8)
      .clampScalar(0, X86_s8, X86_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder(G_SEXTLOAD)
      .legalForTypesWithMemDesc({{X86_s16, X86_p0, X86_s8, 1}, {X86_s32, X86_p0, X86_s8, 1}, {X86_s32, X86_p0, X86_s16, 1}})
      .legalForTypesWithMemDesc(Is64Bit, {{X86_s64, X86_p0, X86_s8, 1}, {X86_s64, X86_p0, X86_s16, 1}, {X86_s64, X86_p0, X86_s32, 1}});
  getActionDefinitionsBuilder(G_ZEXTLOAD)
      .legalForTypesWithMemDesc({{X86_s16, X86_p0, X86_s8, 1}, {X86_s32, X86_p0, X86_s8, 1}, {X86_s32, X86_p0, X86_s16, 1}})
      .legalForTypesWithMemDesc(Is64Bit, {{X86_s64, X86_p0, X86_s8, 1}, {X86_s64, X86_p0, X86_s16, 1}, {X86_s64, X86_p0, X86_s32, 1}});
  getActionDefinitionsBuilder(G_FPEXTLOAD)
      .legalForTypesWithMemDesc(UseX87, {{X86_s80, X86_p0, X86_s32, 1}, {X86_s80, X86_p0, X86_s64, 1}, {X86_s64, X86_p0, X86_s32, 1}});
  getActionDefinitionsBuilder(G_FPTRUNCSTORE)
      .legalForTypesWithMemDesc(UseX87, {{X86_s80, X86_p0, X86_s32, 1}, {X86_s80, X86_p0, X86_s64, 1}, {X86_s64, X86_p0, X86_s32, 1}});
  // sext, zext, and anyext
  getActionDefinitionsBuilder(G_ANYEXT)
      .legalFor({X86_s8, X86_s16, X86_s32, X86_s128})
      .legalFor(Is64Bit, {X86_s64})
      .widenScalarToNextPow2(0, /*Min=*/8)
      .clampScalar(0, X86_s8, X86_sMaxScalar)
      .widenScalarToNextPow2(1, /*Min=*/8)
      .clampScalar(1, X86_s8, X86_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder({G_SEXT, G_ZEXT})
      .legalFor({X86_s8, X86_s16, X86_s32})
      .legalFor(Is64Bit, {X86_s64})
      .widenScalarToNextPow2(0, /*Min=*/8)
      .clampScalar(0, X86_s8, X86_sMaxScalar)
      .widenScalarToNextPow2(1, /*Min=*/8)
      .clampScalar(1, X86_s8, X86_sMaxScalar)
      .scalarize(0);
  getActionDefinitionsBuilder(G_TRUNC)
      .legalForCartesianProduct({X86_s1, X86_s8, X86_s16, X86_s32, X86_s64}, {X86_s8, X86_s16, X86_s32, X86_s64, X86_s128});
  getActionDefinitionsBuilder(G_SEXT_INREG)
      .lower();
  // fp constants
  getActionDefinitionsBuilder(G_FCONSTANT)
      .legalFor({X86_s32, X86_s64})
      .legalFor(UseX87, {X86_s80});
  // fp arithmetic
  getActionDefinitionsBuilder({G_FADD, G_FSUB, G_FMUL, G_FDIV})
      .legalFor({X86_s32, X86_s64})
      .legalFor(HasSSE1, {X86_v4s32})
      .legalFor(HasSSE2, {X86_v2s64})
      .legalFor(HasAVX, {X86_v8s32, X86_v4s64})
      .legalFor(HasAVX512, {X86_v16s32, X86_v8s64})
      .legalFor(UseX87, {X86_s80});
  getActionDefinitionsBuilder(G_FABS)
      .legalFor(UseX87, {X86_s80})
      .legalFor(UseX87 && !(Is64Bit), {X86_s64})
      .lower();
  // fp comparison
  getActionDefinitionsBuilder(G_FCMP)
      .legalFor(HasSSE1 || UseX87, {X86_s8, X86_s32})
      .legalFor(HasSSE2 || UseX87, {X86_s8, X86_s64})
      .legalFor(UseX87, {X86_s8, X86_s80})
      .clampScalar(0, X86_s8, X86_s8)
      .clampScalar(1, X86_s32, HasSSE2 ? X86_s64 : X86_s32)
      .widenScalarToNextPow2(1);
  // fp conversions
  getActionDefinitionsBuilder(G_FPEXT)
      .legalFor(HasSSE2, {{X86_s64, X86_s32}})
      .legalFor(HasAVX, {{X86_v4s64, X86_v4s32}})
      .legalFor(HasAVX512, {{X86_v8s64, X86_v8s32}})
      .lowerFor(UseX87, {{X86_s64, X86_s32}, {X86_s80, X86_s32}, {X86_s80, X86_s64}})
      .libcall();
  getActionDefinitionsBuilder(G_FPTRUNC)
      .legalFor(HasSSE2, {{X86_s32, X86_s64}})
      .legalFor(HasAVX, {{X86_v4s32, X86_v4s64}})
      .legalFor(HasAVX512, {{X86_v8s32, X86_v8s64}})
      .lowerFor(UseX87, {{X86_s32, X86_s64}, {X86_s32, X86_s80}, {X86_s64, X86_s80}});
  getActionDefinitionsBuilder(G_SITOFP)
      .legalFor(HasSSE1, {{X86_s32, X86_s32}})
      .legalFor(HasSSE1 && Is64Bit, {{X86_s32, X86_s64}})
      .legalFor(HasSSE2, {{X86_s64, X86_s32}})
      .legalFor(HasSSE2 && Is64Bit, {{X86_s64, X86_s64}})
      .clampScalar(1, (UseX87 && !HasSSE1) ? X86_s16 : X86_s32, X86_sMaxScalar)
      .widenScalarToNextPow2(1)
      .customForCartesianProduct(UseX87, {X86_s32, X86_s64, X86_s80}, {X86_s16, X86_s32, X86_s64})
      .clampScalar(0, X86_s32, HasSSE2 ? X86_s64 : X86_s32)
      .widenScalarToNextPow2(0);
  getActionDefinitionsBuilder(G_FPTOSI)
      .legalFor(HasSSE1, {{X86_s32, X86_s32}})
      .legalFor(HasSSE1 && Is64Bit, {{X86_s64, X86_s32}})
      .legalFor(HasSSE2, {{X86_s32, X86_s64}})
      .legalFor(HasSSE2 && Is64Bit, {{X86_s64, X86_s64}})
      .clampScalar(0, (UseX87 && !HasSSE1) ? X86_s16 : X86_s32, X86_sMaxScalar)
      .widenScalarToNextPow2(0)
      .customForCartesianProduct(UseX87, {X86_s16, X86_s32, X86_s64}, {X86_s32, X86_s64, X86_s80})
      .clampScalar(1, X86_s32, HasSSE2 ? X86_s64 : X86_s32)
      .widenScalarToNextPow2(1);
  // For G_UITOFP and G_FPTOUI without AVX512, we have to custom legalize
  // types
  // <= s32 manually. Otherwise, in custom handler there is no way to
  // understand whether s32 is an original type and we need to promote it to
  // s64 or s32 is obtained after widening and we shouldn't widen it to s64.
  // 
  // For AVX512 we simply widen types as there is direct mapping from opcodes
  // to asm instructions.
  getActionDefinitionsBuilder(G_UITOFP)
      .legalFor(HasAVX512, {{X86_s32, X86_s32}, {X86_s32, X86_s64}, {X86_s64, X86_s32}, {X86_s64, X86_s64}})
      .customIf([=](const LegalityQuery &Query) { return !(HasAVX512) && ((HasSSE1 && typeIs(0, X86_s32)(Query)) || (HasSSE2 && typeIs(0, X86_s64)(Query))) && scalarNarrowerThan(1, Is64Bit ? 64 : 32)(Query); })
      .lowerIf([=](const LegalityQuery &Query) { return !(HasAVX512) && ((HasSSE1 && typeIs(0, X86_s32)(Query)) || (HasSSE2 && typeIs(0, X86_s64)(Query))) && Is64Bit && typeIs(1, X86_s64)(Query); })
      .clampScalar(0, X86_s32, HasSSE2 ? X86_s64 : X86_s32)
      .widenScalarToNextPow2(0)
      .clampScalar(1, X86_s32, X86_sMaxScalar)
      .widenScalarToNextPow2(1);
  // TODO: replace with customized legalization using
  // specifics of cvttsd2si. The selection of this node requires
  // a vector type. Either G_SCALAR_TO_VECTOR is needed or more advanced
  // support of G_BUILD_VECTOR/G_INSERT_VECTOR_ELT is required beforehand.
  getActionDefinitionsBuilder(G_FPTOUI)
      .legalFor(HasAVX512, {{X86_s32, X86_s32}, {X86_s32, X86_s64}, {X86_s64, X86_s32}, {X86_s64, X86_s64}})
      .customIf([=](const LegalityQuery &Query) { return !(HasAVX512) && ((HasSSE1 && typeIs(1, X86_s32)(Query)) || (HasSSE2 && typeIs(1, X86_s64)(Query))) && scalarNarrowerThan(0, Is64Bit ? 64 : 32)(Query); })
      .lowerIf([=](const LegalityQuery &Query) { return !(HasAVX512) && ((HasSSE1 && typeIs(1, X86_s32)(Query)) || (HasSSE2 && typeIs(1, X86_s64)(Query))) && Is64Bit && typeIs(0, X86_s64)(Query); })
      .clampScalar(0, X86_s32, X86_sMaxScalar)
      .widenScalarToNextPow2(0)
      .clampScalar(1, X86_s32, HasSSE2 ? X86_s64 : X86_s32)
      .widenScalarToNextPow2(1);
  // vector ops
  getActionDefinitionsBuilder(G_BUILD_VECTOR)
      .customIf([=](const LegalityQuery &Query) { return (HasSSE1 && typeInSet(0, {X86_v4s32})(Query)) || (HasSSE2 && typeInSet(0, {X86_v2s64, X86_v8s16, X86_v16s8})(Query)) || (HasAVX && typeInSet(0, {X86_v4s64, X86_v8s32, X86_v16s16, X86_v32s8})(Query)) || (HasAVX512 && typeInSet(0, {X86_v8s64, X86_v16s32, X86_v32s16, X86_v64s8})(Query)); })
      .clampNumElements(0, X86_v16s8, X86_s8MaxVector)
      .clampNumElements(0, X86_v8s16, X86_s16MaxVector)
      .clampNumElements(0, X86_v4s32, X86_s32MaxVector)
      .clampNumElements(0, X86_v2s64, X86_s64MaxVector)
      .moreElementsToNextPow2(0);
  getActionDefinitionsBuilder(G_EXTRACT)
      .legalFor(HasAVX, {{X86_v16s8, X86_v32s8}, {X86_v8s16, X86_v16s16}, {X86_v4s32, X86_v8s32}, {X86_v2s64, X86_v4s64}})
      .legalFor(HasAVX512, {{X86_v16s8, X86_v64s8}, {X86_v32s8, X86_v64s8}, {X86_v8s16, X86_v32s16}, {X86_v16s16, X86_v32s16}, {X86_v4s32, X86_v16s32}, {X86_v8s32, X86_v16s32}, {X86_v2s64, X86_v8s64}, {X86_v4s64, X86_v8s64}})
      .unsupported();
  getActionDefinitionsBuilder(G_INSERT)
      .legalFor(HasAVX, {{X86_v32s8, X86_v16s8}, {X86_v16s16, X86_v8s16}, {X86_v8s32, X86_v4s32}, {X86_v4s64, X86_v2s64}})
      .legalFor(HasAVX512, {{X86_v64s8, X86_v16s8}, {X86_v64s8, X86_v32s8}, {X86_v32s16, X86_v8s16}, {X86_v32s16, X86_v16s16}, {X86_v16s32, X86_v4s32}, {X86_v16s32, X86_v8s32}, {X86_v8s64, X86_v2s64}, {X86_v8s64, X86_v4s64}})
      .unsupported();
  // todo: only permit dst types up to max legal vector register size?
  getActionDefinitionsBuilder(G_CONCAT_VECTORS)
      .legalFor(HasSSE1, {{X86_v32s8, X86_v16s8}, {X86_v16s16, X86_v8s16}, {X86_v8s32, X86_v4s32}, {X86_v4s64, X86_v2s64}})
      .legalFor(HasAVX, {{X86_v64s8, X86_v16s8}, {X86_v64s8, X86_v32s8}, {X86_v32s16, X86_v8s16}, {X86_v32s16, X86_v16s16}, {X86_v16s32, X86_v4s32}, {X86_v16s32, X86_v8s32}, {X86_v8s64, X86_v2s64}, {X86_v8s64, X86_v4s64}});
  // todo: vectors and address spaces
  getActionDefinitionsBuilder(G_SELECT)
      .legalFor({{X86_s16, X86_s32}, {X86_s32, X86_s32}, {X86_p0, X86_s32}})
      .legalFor(!(HasCMOV), {{X86_s8, X86_s32}})
      .legalFor(Is64Bit, {{X86_s64, X86_s32}})
      .legalFor(UseX87, {{X86_s80, X86_s32}})
      .clampScalar(1, X86_s32, X86_s32)
      .widenScalarToNextPow2(0, /*Min=*/8)
      .clampScalar(0, HasCMOV ? X86_s16 : X86_s8, X86_sMaxScalar);
  // memory intrinsics
  getActionDefinitionsBuilder({G_MEMCPY, G_MEMMOVE, G_MEMSET})
      .libcall();
  getActionDefinitionsBuilder({G_DYN_STACKALLOC, G_STACKSAVE, G_STACKRESTORE})
      .lower();
  // fp intrinsics
  // fpclass for i686 is disabled for llvm issue #171992
  getActionDefinitionsBuilder(G_IS_FPCLASS)
      .lowerFor(Is64Bit, {{X86_s1, X86_s32}, {X86_s1, X86_s64}, {X86_s1, X86_s80}});
  getActionDefinitionsBuilder({G_INTRINSIC_ROUNDEVEN, G_INTRINSIC_TRUNC})
      .scalarize(0)
      .minScalar(0, LLT::scalar(32))
      .libcall();
  getActionDefinitionsBuilder({G_INTRINSIC, G_INTRINSIC_W_SIDE_EFFECTS})
      .alwaysLegal();
  getActionDefinitionsBuilder({G_TRAP, G_DEBUGTRAP, G_UBSANTRAP})
      .alwaysLegal();
  getActionDefinitionsBuilder(G_INVOKE_REGION_START)
      .alwaysLegal();
