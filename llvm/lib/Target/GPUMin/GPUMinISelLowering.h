//===-- GPUMinISelLowering.h - GPUMin DAG lowering interface ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUMIN_GPUMINISELLOWERING_H
#define LLVM_LIB_TARGET_GPUMIN_GPUMINISELLOWERING_H

#include "GPUMinSelectionDAGInfo.h"
#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {

class GPUMinSubtarget;

/// The type and operation legalizer for GM32.
///
/// Chapter 00 legalizes exactly one type: i32 in the per-lane register file.
/// Everything else (i8/i16 promotion, i64 splitting, pointer arithmetic) is
/// handled by the generic legalizer, which is also why a function that uses
/// any other type cannot be selected yet.
class GPUMinTargetLowering : public TargetLowering {
public:
  GPUMinTargetLowering(const TargetMachine &TM, const GPUMinSubtarget &STI);

  const char *getTargetNodeName(unsigned Opcode) const override;

  bool CanLowerReturn(CallingConv::ID CallConv, MachineFunction &MF,
                      bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      LLVMContext &Context, const Type *RetTy) const override;

  SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;

  SDValue LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv,
                               bool IsVarArg,
                               const SmallVectorImpl<ISD::InputArg> &Ins,
                               const SDLoc &DL, SelectionDAG &DAG,
                               SmallVectorImpl<SDValue> &InVals) const override;

  SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
                      SelectionDAG &DAG) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_GPUMIN_GPUMINISELLOWERING_H
