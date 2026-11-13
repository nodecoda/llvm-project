//===-- MiniRVSelectionDAGInfo.h - MiniRV SelectionDAG Info -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MINIRV_MINIRVSELECTIONDAGINFO_H
#define LLVM_LIB_TARGET_MINIRV_MINIRVSELECTIONDAGINFO_H

#include "llvm/CodeGen/SelectionDAGTargetInfo.h"

#define GET_SDNODE_ENUM
#include "MiniRVGenSDNodeInfo.inc"

namespace llvm {

class MiniRVSelectionDAGInfo : public SelectionDAGGenTargetInfo {
public:
  MiniRVSelectionDAGInfo();
  ~MiniRVSelectionDAGInfo() override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_MINIRV_MINIRVSELECTIONDAGINFO_H
