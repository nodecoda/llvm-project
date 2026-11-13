//===-- MiniRV.h - Top-level interface for MiniRV ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MINIRV_MINIRV_H
#define LLVM_LIB_TARGET_MINIRV_MINIRV_H

#include "MCTargetDesc/MiniRVMCTargetDesc.h"
#include "llvm/CodeGen/MachineFunctionAnalysisManager.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Target/TargetMachine.h"

// The target-specific DAG opcodes (MiniRVISD::BRCC, MiniRVISD::RET) are
// generated from the SDNode records in MiniRVInstrInfo.td.
#define GET_SDNODE_ENUM
#include "MiniRVGenSDNodeInfo.inc"

namespace llvm {

class MiniRVTargetMachine;

class MiniRVISelDAGToDAGPass : public SelectionDAGISelPass {
public:
  MiniRVISelDAGToDAGPass(MiniRVTargetMachine &TM, CodeGenOptLevel OptLevel);
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_MINIRV_MINIRV_H
