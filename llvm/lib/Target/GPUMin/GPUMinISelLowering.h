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
/// i32 is the only register-file type.  i1 lives in the predicate file;
/// i8/i16 are promoted to i32; i64 is expanded into two i32 halves.  Chapter
/// 08 makes the memory side of those promotions precise: narrow loads and
/// stores keep their width instead of reading a full word.
class GPUMinTargetLowering : public TargetLowering {
public:
  GPUMinTargetLowering(const TargetMachine &TM, const GPUMinSubtarget &STI);

  const char *getTargetNodeName(unsigned Opcode) const override;

  /// f32 literals stay in the DAG; selectImmediate() materialises the bit
  /// pattern with movi/shift instead of a constant-pool load.
  FPImmAction getFPImmAction(const APFloat &Imm, EVT VT,
                             bool ForCodeSize) const override;

  /// GM32 has sdiv/udiv/srem/urem.  Telling the DAG combiner that division is
  /// cheap keeps `x / constant` from being rewritten into a 64-bit magic
  /// multiply, which this target cannot select.
  bool isIntDivCheap(EVT VT, AttributeList Attr) const override;

  bool CanLowerReturn(CallingConv::ID CallConv, MachineFunction &MF,
                      bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      LLVMContext &Context, const Type *RetTy) const override;

  SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;

  SDValue PerformDAGCombine(SDNode *N, DAGCombinerInfo &DCI) const override;

  SDValue LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv,
                               bool IsVarArg,
                               const SmallVectorImpl<ISD::InputArg> &Ins,
                               const SDLoc &DL, SelectionDAG &DAG,
                               SmallVectorImpl<SDValue> &InVals) const override;

  SDValue LowerCall(TargetLowering::CallLoweringInfo &CLI,
                    SmallVectorImpl<SDValue> &InVals) const override;

  SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
                      SelectionDAG &DAG) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_GPUMIN_GPUMINISELLOWERING_H
