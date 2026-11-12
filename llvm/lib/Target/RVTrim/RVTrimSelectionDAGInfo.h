//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVTrim_RVTrimSELECTIONDAGINFO_H
#define LLVM_LIB_TARGET_RVTrim_RVTrimSELECTIONDAGINFO_H

#include "llvm/CodeGen/SDNodeInfo.h"
#include "llvm/CodeGen/SelectionDAGTargetInfo.h"

#define GET_SDNODE_ENUM
#include "RVTrimGenSDNodeInfo.inc"

namespace llvm {

namespace RVTrimISD {
// RVTrimISD Node TSFlags
enum : llvm::SDNodeTSFlags {
  HasPassthruOpMask = 1 << 0,
  HasMaskOpMask = 1 << 1,
};
} // namespace RVTrimISD

class RVTrimSelectionDAGInfo : public SelectionDAGGenTargetInfo {
public:
  RVTrimSelectionDAGInfo();

  ~RVTrimSelectionDAGInfo() override;

  void verifyTargetNode(const SelectionDAG &DAG,
                        const SDNode *N) const override;

  SDValue EmitTargetCodeForMemset(SelectionDAG &DAG, const SDLoc &dl,
                                  SDValue Chain, SDValue Dst, SDValue Src,
                                  SDValue Size, Align Alignment,
                                  bool isVolatile, bool AlwaysInline,
                                  MachinePointerInfo DstPtrInfo) const override;

  bool hasPassthruOp(unsigned Opcode) const {
    return GenNodeInfo.getDesc(Opcode).TSFlags & RVTrimISD::HasPassthruOpMask;
  }

  bool hasMaskOp(unsigned Opcode) const {
    return GenNodeInfo.getDesc(Opcode).TSFlags & RVTrimISD::HasMaskOpMask;
  }

  unsigned getMAccOpcode(unsigned MulOpcode) const {
    switch (static_cast<RVTrimISD::GenNodeType>(MulOpcode)) {
    default:
      llvm_unreachable("Unexpected opcode");
    case RVTrimISD::VWMUL_VL:
      return RVTrimISD::VWMACC_VL;
    case RVTrimISD::VWMULU_VL:
      return RVTrimISD::VWMACCU_VL;
    case RVTrimISD::VWMULSU_VL:
      return RVTrimISD::VWMACCSU_VL;
    }
  }
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_RVTrim_RVTrimSELECTIONDAGINFO_H
