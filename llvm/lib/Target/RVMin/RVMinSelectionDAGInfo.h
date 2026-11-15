//===-- RVMinSelectionDAGInfo.h - RVMin DAG target info ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Chapter 3: every target that defines its own SDNodes must also give the
// SelectionDAG a way to describe them.  Without this object the DAG verifier
// dereferences a null target-info pointer as soon as the first target node
// (RET_FLAG) is inserted.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RVMIN_RVMINSELECTIONDAGINFO_H
#define LLVM_LIB_TARGET_RVMIN_RVMINSELECTIONDAGINFO_H

#include "llvm/CodeGen/SelectionDAGTargetInfo.h"

#define GET_SDNODE_ENUM
#include "RVMinGenSDNodeInfo.inc"

namespace llvm {

class RVMinSelectionDAGInfo : public SelectionDAGGenTargetInfo {
public:
  RVMinSelectionDAGInfo();
  ~RVMinSelectionDAGInfo() override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_RVMIN_RVMINSELECTIONDAGINFO_H
