//===-- GPUMinISelLowering.cpp - GPUMin DAG lowering interface ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUMinISelLowering.h"
#include "GPUMinSubtarget.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_CALLING_CONV_IMPL
#include "GPUMinGenCallingConv.inc"

GPUMinTargetLowering::GPUMinTargetLowering(const TargetMachine &TM,
                                           const GPUMinSubtarget &STI)
    : TargetLowering(TM, STI) {
  // GM32 has one architectural value type: a 32-bit per-lane integer.  This
  // must happen before computeRegisterProperties(), which derives the legal
  // type set from the registered classes.
  addRegisterClass(MVT::i32, &GPUMin::GPRRegClass);

  // Chapter 04: a comparison result is one bit per lane, which GM32 keeps in
  // p0-p7.  Making i1 a legal type is what lets `setcc` produce a predicate
  // directly instead of materialising 0/1 in a general register.
  addRegisterClass(MVT::i1, &GPUMin::PREDRegClass);

  setBooleanContents(ZeroOrOneBooleanContent);
  setOperationAction(ISD::SELECT, MVT::i32, Legal);
  setOperationAction(ISD::SELECT_CC, MVT::i32, Custom);
  // A predicate-producing setcc has result type i1 and selects to SETP.
  // The common `zext (icmp ...)` shape, and the i64 carry expansion, instead
  // produce a setcc with an i32 result.  That one is rewritten by
  // PerformDAGCombine into SETP + SELP; there is no single instruction for it.
  setTargetDAGCombine(ISD::SETCC);
  setOperationAction(ISD::BRCOND, MVT::Other, Custom);
  setOperationAction(ISD::BR_CC, MVT::i32, Custom);

  // Chapter 08: i8 and i16 have no register class of their own, so the type
  // legalizer promotes every value of those types to i32.  What it cannot
  // invent is the memory side: a promoted `load i8` becomes `extloadi8`, and
  // the only correct way to implement that is the narrow M-format opcode
  // (ld.global.u8/s8/u16/s16), not a 32-bit load plus a truncate.
  for (MVT VT : {MVT::i8, MVT::i16}) {
    setLoadExtAction(ISD::EXTLOAD, MVT::i32, VT, Legal);
    setLoadExtAction(ISD::ZEXTLOAD, MVT::i32, VT, Legal);
    setLoadExtAction(ISD::SEXTLOAD, MVT::i32, VT, Legal);
    setTruncStoreAction(MVT::i32, VT, Legal);
  }

  // `store i1` names an 8-bit location with a predicate value.  Because the
  // memory type is *wider* than the value, this is a non-truncating store and
  // the generic legalizer would leave it alone (it only looks at the value
  // type).  Take it over: zext the predicate into a 0/1 word, then use the
  // ordinary 32 -> 8 truncstore.
  setOperationAction(ISD::STORE, MVT::i1, Custom);
  setTruncStoreAction(MVT::i1, MVT::i8, Custom);

  // `load i1` is a byte load followed by "take bit 0".  The generic legalizer
  // models it as zextload-from-i1, which no single opcode implements.
  setLoadExtAction(ISD::EXTLOAD, MVT::i32, MVT::i1, Custom);
  setLoadExtAction(ISD::ZEXTLOAD, MVT::i32, MVT::i1, Custom);
  setLoadExtAction(ISD::SEXTLOAD, MVT::i32, MVT::i1, Custom);


  computeRegisterProperties(STI.getRegisterInfo());
}

const char *GPUMinTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  case GPUMinISD::RET:
    return "GPUMinISD::RET";
  case GPUMinISD::BRCOND:
    return "GPUMinISD::BRCOND";
  case GPUMinISD::KRET:
    return "GPUMinISD::KRET";
  default:
    return nullptr;
  }
}

bool GPUMinTargetLowering::CanLowerReturn(
    CallingConv::ID CallConv, MachineFunction &MF, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs, LLVMContext &Context,
    const Type *RetTy) const {
  // Chapter 03 adds the integer half of the ABI: values return in r4-r5
  // (RetCC_GPUMin).  Kernels still return void, which is the Outs.empty()
  // case.
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, Context);
  return CCInfo.CheckReturn(Outs, RetCC_GPUMin);
}

SDValue GPUMinTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  if (IsVarArg)
    report_fatal_error("GPUMin: varargs are not supported");

  MachineFunction &MF = DAG.getMachineFunction();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();

  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeFormalArguments(Ins, CC_GPUMin);

  for (CCValAssign &VA : ArgLocs) {
    if (!VA.isRegLoc())
      report_fatal_error("GPUMin: stack arguments arrive with frame lowering");

    // Arguments are already in physical registers (r4-r11 by CC_GPUMin); turn
    // each into a virtual register so the rest of the pipeline can rename it.
    Register VReg = RegInfo.createVirtualRegister(&GPUMin::GPRRegClass);
    RegInfo.addLiveIn(VA.getLocReg(), VReg);
    InVals.push_back(DAG.getCopyFromReg(Chain, DL, VReg, VA.getLocVT()));
    Chain = InVals.back().getValue(1);
  }
  return Chain;
}

SDValue GPUMinTargetLowering::LowerReturn(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs,
    const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
    SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();

  SmallVector<CCValAssign, 4> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
  CCInfo.AnalyzeReturn(Outs, RetCC_GPUMin);

  // The RET node carries both the chain/glue and one register operand per
  // value.  The register operands are what keeps the copies above alive: a
  // node with only a chain would let dead-machine-instruction elimination
  // delete `$r4 = COPY %v` because nothing in the machine function reads r4.
  SmallVector<SDValue, 4> RetOps(1, Chain);
  SDValue Glue;
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    CCValAssign &VA = RVLocs[i];
    if (!VA.isRegLoc())
      report_fatal_error("GPUMin: stack returns are not supported");

    // Move the value into its ABI register.  The copy becomes a `mov` in the
    // machine layer (copyPhysReg), which is the first instruction the target
    // emits that is not a constant or a terminator.
    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), OutVals[i], Glue);
    Glue = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  RetOps[0] = Chain;
  if (Glue)
    RetOps.push_back(Glue);

  // Chapter 07 splits the two meanings of "return": a kernel terminates the
  // launch (`exit`), a device function goes back to lr (`ret`).  The
  // distinction is carried by the "gpumin_kernel" attribute, which is also
  // what the launcher in chapter 13 will look for.
  unsigned RetOpcode = MF.getFunction().hasFnAttribute("gpumin_kernel")
                           ? GPUMinISD::KRET
                           : GPUMinISD::RET;
  return DAG.getNode(RetOpcode, DL, MVT::Other, RetOps);
}

SDValue GPUMinTargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                                        SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG = CLI.DAG;
  SDLoc DL = CLI.DL;
  MachineFunction &MF = DAG.getMachineFunction();

  SmallVector<CCValAssign, 8> ArgLocs;
  CCState CCInfo(CLI.CallConv, CLI.IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeCallOperands(CLI.Outs, CC_GPUMin);

  // Stack arguments need an outgoing-argument area, which is the frame
  // chapter's job; chapter 07 covers the register half of the ABI only.
  if (CCInfo.getStackSize() != 0)
    report_fatal_error("GPUMin: stack arguments are not supported yet");

  // A function with a call must save lr: the callee is free to overwrite it.
  MF.getFrameInfo().setHasCalls(true);

  SDValue Chain = CLI.Chain;
  SDValue Glue;
  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Chain);

  // Direct calls only: the J format has no register form, so the callee is a
  // symbol whose offset is resolved by the assembler.
  SDValue Callee = CLI.Callee;
  if (auto *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee = DAG.getTargetGlobalAddress(
        G->getGlobal(), DL, getPointerTy(DAG.getDataLayout()));
  else
    report_fatal_error("GPUMin: indirect calls are not supported yet");
  Ops.push_back(Callee);

  for (unsigned I = 0; I != ArgLocs.size(); ++I) {
    CCValAssign &VA = ArgLocs[I];
    if (!VA.isRegLoc())
      report_fatal_error("GPUMin: stack arguments are not supported yet");
    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), CLI.OutVals[I], Glue);
    Glue = Chain.getValue(1);
    // The register operand makes the argument live *into* the call; without
    // it the copy could be sunk or deleted.
    Ops.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  const GPUMinSubtarget &ST = MF.getSubtarget<GPUMinSubtarget>();
  const uint32_t *Mask =
      ST.getRegisterInfo()->getCallPreservedMask(MF, CLI.CallConv);
  Ops.push_back(DAG.getRegisterMask(Mask));

  if (Glue.getNode())
    Ops.push_back(Glue);

  // A target node, not a machine node: selection happens later, and the CALL
  // instruction itself declares the caller-saved registers it destroys.
  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  SDValue Call = DAG.getNode(GPUMinISD::CALL, DL, NodeTys, Ops);
  Chain = Call.getValue(0);
  Glue = Call.getValue(1);

  if (CLI.RetTy->isVoidTy())
    return Chain;

  EVT RetVT = getValueType(DAG.getDataLayout(), CLI.RetTy);
  SDValue Ret = DAG.getCopyFromReg(Chain, DL, GPUMin::R4, RetVT, Glue);
  InVals.push_back(Ret);
  return Ret.getValue(1);
}

SDValue GPUMinTargetLowering::PerformDAGCombine(SDNode *N,
                                                DAGCombinerInfo &DCI) const {
  SelectionDAG &DAG = DCI.DAG;
  SDLoc DL(N);

  // 64-bit comparisons.  i64 is expanded into two i32 halves, and the generic
  // type legalizer cannot expand a setcc whose result type is the legal i1
  // (it asserts instead).  Rewrite the comparison in terms of i32 compares
  // before type legalization ever sees it.
  if (N->getOpcode() == ISD::SETCC &&
      N->getOperand(0).getValueType() == MVT::i64) {
    SDValue L = N->getOperand(0), R = N->getOperand(1);
    ISD::CondCode CC = cast<CondCodeSDNode>(N->getOperand(2))->get();

    auto Half = [&](SDValue V, unsigned Idx) {
      return DAG.getNode(ISD::EXTRACT_ELEMENT, DL, MVT::i32, V,
                         DAG.getConstant(Idx, DL, MVT::i64));
    };
    auto Cmp = [&](SDValue A, SDValue B, ISD::CondCode C) {
      return DAG.getSetCC(DL, MVT::i1, A, B, C);
    };
    SDValue Zero = DAG.getConstant(0, DL, MVT::i32);
    SDValue One = DAG.getConstant(1, DL, MVT::i32);
    // Materialise through SELECT rather than ZERO_EXTEND: the combiner folds
    // `(and (zext a), (zext b))` back into an i1 AND, and the predicate file
    // has no boolean ALU.
    auto ToInt = [&](SDValue P) {
      return DAG.getSelect(DL, MVT::i32, P, One, Zero);
    };
    auto BoolOp = [&](SDValue A, SDValue B, unsigned Opc) {
      SDValue V = DAG.getNode(Opc, DL, MVT::i32, ToInt(A), ToInt(B));
      return Cmp(V, Zero, ISD::SETNE);
    };
    auto And = [&](SDValue A, SDValue B) { return BoolOp(A, B, ISD::AND); };
    auto Or = [&](SDValue A, SDValue B) { return BoolOp(A, B, ISD::OR); };
    auto Eq = [&](SDValue A, SDValue B) {
      return Cmp(A, B, ISD::SETEQ);
    };
    auto Not = [&](SDValue A) { return Eq(ToInt(A), Zero); };

    // Fold the "greater" forms into the "less" forms by swapping operands;
    // doing the swap before the halves are extracted keeps the two in sync.
    bool Swap = false;
    switch (CC) {
    case ISD::SETUGT: CC = ISD::SETULT; Swap = true; break;
    case ISD::SETUGE: CC = ISD::SETULE; Swap = true; break;
    case ISD::SETGT:  CC = ISD::SETLT;  Swap = true; break;
    case ISD::SETGE:  CC = ISD::SETLE;  Swap = true; break;
    default: break;
    }
    if (Swap)
      std::swap(L, R);

    SDValue LLo = Half(L, 0), LHi = Half(L, 1);
    SDValue RLo = Half(R, 0), RHi = Half(R, 1);

    // Unsigned/signed less-than: the high word decides unless it is equal,
    // in which case the low words are compared unsigned (two's complement).
    auto LessThan = [&](bool IsSigned) {
      SDValue HiCmp = Cmp(LHi, RHi, IsSigned ? ISD::SETLT : ISD::SETULT);
      return Or(HiCmp, And(Eq(LHi, RHi), Cmp(LLo, RLo, ISD::SETULT)));
    };
    auto LessEqual = [&](bool IsSigned) {
      SDValue HiCmp = Cmp(LHi, RHi, IsSigned ? ISD::SETLE : ISD::SETULE);
      return Or(HiCmp, And(Eq(LHi, RHi), Cmp(LLo, RLo, ISD::SETULE)));
    };

    SDValue Pred;
    switch (CC) {
    case ISD::SETEQ:
      Pred = And(Eq(LLo, RLo), Eq(LHi, RHi));
      break;
    case ISD::SETNE:
      Pred = Not(And(Eq(LLo, RLo), Eq(LHi, RHi)));
      break;
    case ISD::SETULT:
      Pred = LessThan(/*IsSigned=*/false);
      break;
    case ISD::SETULE:
      Pred = LessEqual(/*IsSigned=*/false);
      break;
    case ISD::SETLT:
      Pred = LessThan(/*IsSigned=*/true);
      break;
    case ISD::SETLE:
      Pred = LessEqual(/*IsSigned=*/true);
      break;
    default:
      return SDValue();
    }

    if (N->getValueType(0) == MVT::i32)
      return DAG.getSelect(DL, MVT::i32, Pred, One, Zero);
    return Pred;
  }

  // `setcc` with an i32 result: the predicate file has the answer, but the
  // consumer wants a general register.  Materialising 1/0 through SELP keeps
  // the compare in SETP and reuses the predicate rather than branching.
  if (N->getOpcode() == ISD::SETCC && N->getValueType(0) == MVT::i32) {
    SDValue Cmp = DAG.getSetCC(DL, MVT::i1, N->getOperand(0), N->getOperand(1),
                               cast<CondCodeSDNode>(N->getOperand(2))->get());
    return DAG.getSelect(DL, MVT::i32, Cmp,
                         DAG.getConstant(1, DL, MVT::i32),
                         DAG.getConstant(0, DL, MVT::i32));
  }

  return SDValue();
}

SDValue GPUMinTargetLowering::LowerOperation(SDValue Op,
                                             SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  default:
    llvm_unreachable("GPUMin: unhandled custom lowering");

  // `select_cc` is what `icmp` + `select` becomes after type legalization.
  // Splitting it into a predicate compare and a predicate select is the whole
  // point of having p0-p7: the compare writes a predicate, SELP consumes it.
  // Both forms end up as GPUMinISD::BRCOND, whose only condition source is a
  // predicate register.  `br_cc` therefore materialises the comparison with
  // setcc first: same split as SELECT_CC, one node earlier.
  case ISD::BRCOND:
    return DAG.getNode(GPUMinISD::BRCOND, SDLoc(Op), MVT::Other,
                       Op.getOperand(0), Op.getOperand(1), Op.getOperand(2));

  case ISD::BR_CC: {
    SDValue Chain = Op.getOperand(0);
    ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(1))->get();
    SDValue Cmp = DAG.getSetCC(SDLoc(Op), MVT::i1, Op.getOperand(2),
                               Op.getOperand(3), CC);
    return DAG.getNode(GPUMinISD::BRCOND, SDLoc(Op), MVT::Other, Chain, Cmp,
                       Op.getOperand(4));
  }

  // `load i1`: read the containing byte, then keep bit 0.  A sign-extending
  // load (sext i1) is the same value negated, which the SUB against r0-like
  // zero constant expresses without another opcode.
  case ISD::LOAD: {
    auto *LD = cast<LoadSDNode>(Op.getNode());
    if (LD->getMemoryVT() != MVT::i1)
      llvm_unreachable("GPUMin: unexpected custom load");
    SDLoc DL(Op);
    SDValue Byte = DAG.getExtLoad(ISD::ZEXTLOAD, DL, MVT::i32,
                                  LD->getChain(), LD->getBasePtr(), MVT::i8,
                                  LD->getMemOperand());
    SDValue Bit = DAG.getNode(ISD::AND, DL, MVT::i32, Byte,
                              DAG.getConstant(1, DL, MVT::i32));
    if (LD->getExtensionType() == ISD::SEXTLOAD)
      Bit = DAG.getNode(ISD::SUB, DL, MVT::i32,
                        DAG.getConstant(0, DL, MVT::i32), Bit);
    return DAG.getMergeValues({Bit, LD->getChain()}, DL);
  }

  case ISD::STORE: {
    auto *ST = cast<StoreSDNode>(Op.getNode());
    if (ST->getValue().getValueType() != MVT::i1)
      llvm_unreachable("GPUMin: unexpected custom store");
    SDLoc DL(Op);
    SDValue Z = DAG.getNode(ISD::ZERO_EXTEND, DL, MVT::i32, ST->getValue());
    return DAG.getTruncStore(ST->getChain(), DL, Z, ST->getBasePtr(),
                             ST->getPointerInfo(), MVT::i8,
                             ST->getBaseAlign(),
                             ST->getMemOperand()->getFlags());
  }

  case ISD::SELECT_CC: {
    SDValue LHS = Op.getOperand(0);
    SDValue RHS = Op.getOperand(1);
    SDValue TrueV = Op.getOperand(2);
    SDValue FalseV = Op.getOperand(3);
    ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(4))->get();
    SDLoc DL(Op);
    SDValue Cmp = DAG.getSetCC(DL, MVT::i1, LHS, RHS, CC);
    return DAG.getSelect(DL, Op.getValueType(), Cmp, TrueV, FalseV);
  }
  }
}
