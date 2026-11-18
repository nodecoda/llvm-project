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
  setOperationAction(ISD::SETCC, MVT::i32, Legal);

  computeRegisterProperties(STI.getRegisterInfo());
}

const char *GPUMinTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  case GPUMinISD::RET:
    return "GPUMinISD::RET";
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
  return DAG.getNode(GPUMinISD::RET, DL, MVT::Other, RetOps);
}

SDValue GPUMinTargetLowering::LowerOperation(SDValue Op,
                                             SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  default:
    llvm_unreachable("GPUMin: unhandled custom lowering");

  // `select_cc` is what `icmp` + `select` becomes after type legalization.
  // Splitting it into a predicate compare and a predicate select is the whole
  // point of having p0-p7: the compare writes a predicate, SELP consumes it.
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
