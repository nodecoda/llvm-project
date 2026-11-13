//===-- MiniRVISelLowering.cpp - MiniRV DAG Lowering Interface ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lowering is intentionally narrow: the corpus covers 64-bit integer
// arithmetic, and there is no memory, branch or call instruction, so anything
// that would need one is reported instead of being miscompiled.
//
//===----------------------------------------------------------------------===//

#include "MiniRVISelLowering.h"
#include "MiniRV.h"
#include "MiniRVSelectionDAGInfo.h"
#include "MiniRVSubtarget.h"
#include "MiniRVTargetMachine.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_CALLING_CONV_IMPL
#include "MiniRVGenCallingConv.inc"

MiniRVTargetLowering::MiniRVTargetLowering(const TargetMachine &TM,
                                           const MiniRVSubtarget &STI)
    : TargetLowering(TM, STI) {
  addRegisterClass(MVT::i64, &MiniRV::GPRRegClass);

  // There is no multiply or divide instruction.
  for (MVT VT : {MVT::i64}) {
    setOperationAction(ISD::MUL, VT, Expand);
    setOperationAction(ISD::SDIV, VT, Expand);
    setOperationAction(ISD::UDIV, VT, Expand);
    setOperationAction(ISD::SREM, VT, Expand);
    setOperationAction(ISD::UREM, VT, Expand);
  }

  setStackPointerRegisterToSaveRestore(MiniRV::X2);
  setBooleanContents(ZeroOrOneBooleanContent);
  setMaxAtomicSizeInBitsSupported(0);

  computeRegisterProperties(STI.getRegisterInfo());
}

SDValue MiniRVTargetLowering::LowerOperation(SDValue Op,
                                             SelectionDAG &DAG) const {
  llvm_unreachable("MiniRV does not custom-lower this operation");
}

SDValue MiniRVTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();

  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeFormalArguments(Ins, CC_MiniRV);

  MachineRegisterInfo &RegInfo = MF.getRegInfo();
  for (CCValAssign &VA : ArgLocs) {
    if (!VA.isRegLoc())
      report_fatal_error("MiniRV has no load instruction to read a stack "
                         "argument");

    // Go through a virtual register so that the physical argument register is
    // recorded as live-in to the function.
    Register VReg = RegInfo.createVirtualRegister(&MiniRV::GPRRegClass);
    RegInfo.addLiveIn(VA.getLocReg(), VReg);
    SDValue Arg = DAG.getCopyFromReg(Chain, DL, VReg, VA.getLocVT());
    Chain = Arg.getValue(1);
    InVals.push_back(Arg);
  }

  return Chain;
}

bool MiniRVTargetLowering::CanLowerReturn(
    CallingConv::ID CallConv, MachineFunction &MF, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs, LLVMContext &Context,
    const Type *RetTy) const {
  SmallVector<CCValAssign, 4> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, Context);
  return CCInfo.CheckReturn(Outs, RetCC_MiniRV);
}

SDValue
MiniRVTargetLowering::LowerReturn(SDValue Chain, CallingConv::ID CallConv,
                                  bool IsVarArg,
                                  const SmallVectorImpl<ISD::OutputArg> &Outs,
                                  const SmallVectorImpl<SDValue> &OutVals,
                                  const SDLoc &DL, SelectionDAG &DAG) const {
  SmallVector<CCValAssign, 4> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeReturn(Outs, RetCC_MiniRV);

  SDValue Glue;
  SmallVector<SDValue, 4> RetOps(1, Chain);
  for (unsigned I = 0; I != RVLocs.size(); ++I) {
    CCValAssign &VA = RVLocs[I];
    assert(VA.isRegLoc() && "MiniRV returns in registers only");
    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), OutVals[I], Glue);
    Glue = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  RetOps[0] = Chain;
  if (Glue.getNode())
    RetOps.push_back(Glue);

  return DAG.getNode(MiniRVISD::RET, DL, MVT::Other, RetOps);
}

SDValue
MiniRVTargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                                SmallVectorImpl<SDValue> &InVals) const {
  report_fatal_error("MiniRV has no call instruction");
}

const char *MiniRVTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  case MiniRVISD::RET:
    return "MiniRVISD::RET";
  default:
    return nullptr;
  }
}
