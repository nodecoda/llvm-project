//===-- GPUMinISelLowering.cpp - GPUMin DAG lowering interface ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUMinISelLowering.h"
#include "GPUMinSubtarget.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

GPUMinTargetLowering::GPUMinTargetLowering(const TargetMachine &TM,
                                           const GPUMinSubtarget &STI)
    : TargetLowering(TM, STI) {
  // GM32 has one architectural value type: a 32-bit per-lane integer.  This
  // must happen before computeRegisterProperties(), which derives the legal
  // type set from the registered classes.
  addRegisterClass(MVT::i32, &GPUMin::GPRRegClass);
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
  // Chapter 00 only lowers `ret void`: a kernel ends with EXIT and has no
  // return value.  Value returns arrive with the calling convention in
  // chapter 07.
  return Outs.empty();
}

SDValue GPUMinTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  // A chapter-00 function takes no arguments; kernels receive theirs through
  // the launch descriptor, which is chapter 13's subject.
  assert(Ins.empty() && "GPUMin: arguments arrive in chapter 07");
  return Chain;
}

SDValue GPUMinTargetLowering::LowerReturn(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs,
    const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
    SelectionDAG &DAG) const {
  assert(Outs.empty() && "GPUMin: value returns arrive in chapter 07");
  return DAG.getNode(GPUMinISD::RET, DL, MVT::Other, Chain);
}

SDValue GPUMinTargetLowering::LowerOperation(SDValue Op,
                                             SelectionDAG &DAG) const {
  llvm_unreachable("GPUMin: no custom lowering in chapter 00");
}
