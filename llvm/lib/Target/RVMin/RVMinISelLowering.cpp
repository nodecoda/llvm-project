//===-- RVMinISelLowering.cpp - RVMin DAG lowering implementation --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 3: just enough lowering to compile
//
//   int f() { return 42; }
//   int f(int a, int b) { return a + b; }
//
// That means formal arguments in a0-a7, returns in a0, and the RET_FLAG node.
// Calls, stack arguments and everything else arrive in later chapters.
//
//===----------------------------------------------------------------------===//

#include "RVMinISelLowering.h"
#include "RVMin.h"
#include "RVMinSubtarget.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define GET_CALLING_CONV_IMPL
#include "RVMinGenCallingConv.inc"

RVMinTargetLowering::RVMinTargetLowering(const TargetMachine &TM,
                                         const RVMinSubtarget &STI)
    : TargetLowering(TM, STI) {
  addRegisterClass(MVT::i32, &RVMin::GPRRegClass);
  computeRegisterProperties(STI.getRegisterInfo());

  setStackPointerRegisterToSaveRestore(RVMin::X2);
  setBooleanContents(ZeroOrOneBooleanContent);
  setBooleanVectorContents(ZeroOrOneBooleanContent);
}

const char *RVMinTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch (Opcode) {
  case RVMinISD::RET_FLAG:
    return "RVMinISD::RET_FLAG";
  default:
    return nullptr;
  }
}

SDValue RVMinTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  if (IsVarArg)
    report_fatal_error("RVMin: varargs are not supported yet");

  MachineFunction &MF = DAG.getMachineFunction();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();

  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeFormalArguments(Ins, CC_RVMin);

  for (unsigned I = 0, E = Ins.size(); I != E; ++I) {
    CCValAssign &VA = ArgLocs[I];
    if (!VA.isRegLoc())
      report_fatal_error("RVMin: stack arguments are not supported yet");

    MVT LocVT = VA.getLocVT();
    Register VReg = RegInfo.createVirtualRegister(&RVMin::GPRRegClass);
    RegInfo.addLiveIn(VA.getLocReg(), VReg);
    SDValue ArgValue = DAG.getCopyFromReg(Chain, DL, VReg, LocVT);
    InVals.push_back(ArgValue);
  }

  return Chain;
}

bool RVMinTargetLowering::CanLowerReturn(
    CallingConv::ID CallConv, MachineFunction &MF, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs, LLVMContext &Context,
    const Type *RetTy) const {
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, Context);
  return CCInfo.CheckReturn(Outs, RetCC_RVMin);
}

SDValue RVMinTargetLowering::LowerReturn(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs,
    const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
    SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();

  SmallVector<CCValAssign, 4> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
  if (!CCInfo.CheckReturn(Outs, RetCC_RVMin))
    return DAG.getNode(ISD::UNDEF, DL, MVT::Other);

  // Move the return values into their ABI registers and terminate with
  // RET_FLAG, which TableGen maps to PseudoRET / `jalr zero, 0(ra)`.
  SmallVector<SDValue, 4> RetOps(1, Chain);
  SDValue Glue;
  for (unsigned I = 0, E = RVLocs.size(); I != E; ++I) {
    CCValAssign &VA = RVLocs[I];
    assert(VA.isRegLoc() && "can only return in registers");
    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), OutVals[I], Glue);
    Glue = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  RetOps[0] = Chain;
  if (Glue.getNode())
    RetOps.push_back(Glue);

  return DAG.getNode(RVMinISD::RET_FLAG, DL, MVT::Other, RetOps);
}
