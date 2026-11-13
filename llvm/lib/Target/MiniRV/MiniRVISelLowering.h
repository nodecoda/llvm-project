//===-- MiniRVISelLowering.h - MiniRV DAG Lowering Interface ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MINIRV_MINIRVISELLOWERING_H
#define LLVM_LIB_TARGET_MINIRV_MINIRVISELLOWERING_H

#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {

class MiniRVSubtarget;

class MiniRVTargetLowering : public TargetLowering {
public:
  explicit MiniRVTargetLowering(const TargetMachine &TM,
                                const MiniRVSubtarget &STI);

  SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;

  SDValue LowerBRCOND(SDValue Op, SelectionDAG &DAG) const;

  SDValue LowerSELECT_CC(SDValue Op, SelectionDAG &DAG) const;

  SDValue LowerGlobalAddress(SDValue Op, SelectionDAG &DAG) const;

  SDValue LowerJumpTable(SDValue Op, SelectionDAG &DAG) const;

  SDValue LowerFRAMEADDR(SDValue Op, SelectionDAG &DAG) const;

  SDValue LowerRETURNADDR(SDValue Op, SelectionDAG &DAG) const;

  SDValue LowerVASTART(SDValue Op, SelectionDAG &DAG) const;

  SDValue LowerVAARG(SDValue Op, SelectionDAG &DAG) const;

  SDValue LowerBranchCC(SDValue Chain, SDValue LHS, SDValue RHS,
                        ISD::CondCode CC, SDValue Dest, const SDLoc &DL,
                        SelectionDAG &DAG) const;

  EVT getSetCCResultType(const DataLayout &DL, LLVMContext &Context,
                         EVT VT) const override;

  SDValue LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv,
                               bool IsVarArg,
                               const SmallVectorImpl<ISD::InputArg> &Ins,
                               const SDLoc &DL, SelectionDAG &DAG,
                               SmallVectorImpl<SDValue> &InVals) const override;

  SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
                      SelectionDAG &DAG) const override;

  SDValue LowerCall(TargetLowering::CallLoweringInfo &CLI,
                    SmallVectorImpl<SDValue> &InVals) const override;

  void ReplaceNodeResults(SDNode *N, SmallVectorImpl<SDValue> &Results,
                          SelectionDAG &DAG) const override;

  bool CanLowerReturn(CallingConv::ID CallConv, MachineFunction &MF,
                      bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      LLVMContext &Context, const Type *RetTy) const override;

  const char *getTargetNodeName(unsigned Opcode) const override;

  unsigned ComputeNumSignBitsForTargetNode(SDValue Op,
                                           const APInt &DemandedElts,
                                           const SelectionDAG &DAG,
                                           unsigned Depth) const override;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_MINIRV_MINIRVISELLOWERING_H
