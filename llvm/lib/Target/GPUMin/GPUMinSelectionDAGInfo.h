//===-- GPUMinSelectionDAGInfo.h - GPUMin DAG target info ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Every target that defines its own SDNodes must give the SelectionDAG a way to
// describe them.  Chapter 00 defines none, so the plain SelectionDAGTargetInfo
// base is enough; chapter 03 adds the first target node (RET_FLAG) and switches
// this to SelectionDAGGenTargetInfo.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUMIN_GPUMINSELECTIONDAGINFO_H
#define LLVM_LIB_TARGET_GPUMIN_GPUMINSELECTIONDAGINFO_H

#include "llvm/CodeGen/SelectionDAGTargetInfo.h"

#define GET_SDNODE_ENUM
#include "GPUMinGenSDNodeInfo.inc"

namespace llvm {

class GPUMinSelectionDAGInfo : public SelectionDAGGenTargetInfo {
public:
  GPUMinSelectionDAGInfo();
  ~GPUMinSelectionDAGInfo() override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_GPUMIN_GPUMINSELECTIONDAGINFO_H
