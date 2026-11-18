//===-- GPUMinISelDAGToDAG.h - GPUMin DAG-to-DAG selector ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUMIN_GPUMINISELDAGTODAG_H
#define LLVM_LIB_TARGET_GPUMIN_GPUMINISELDAGTODAG_H

#include "llvm/CodeGen/SelectionDAGISel.h"

namespace llvm {

/// New-PM wrapper around GPUMinDAGToDAGISel.  CodeGenPassBuilder adds this to
/// the machine pass pipeline in GPUMinCodeGenPassBuilder::addInstSelector().
class GPUMinDAGToDAGISelPass : public SelectionDAGISelPass {
public:
  explicit GPUMinDAGToDAGISelPass(TargetMachine &TM);
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_GPUMIN_GPUMINISELDAGTODAG_H
