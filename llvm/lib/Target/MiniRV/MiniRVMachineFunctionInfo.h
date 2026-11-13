//===-- MiniRVMachineFunctionInfo.h - MiniRV function info ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares MiniRV-specific per-machine-function information.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_MINIRV_MINIRVMACHINEFUNCTIONINFO_H
#define LLVM_LIB_TARGET_MINIRV_MINIRVMACHINEFUNCTIONINFO_H

#include "llvm/CodeGen/MachineFunction.h"

namespace llvm {

class MiniRVSubtarget;

/// Per-function state that the calling convention lowering has to hand to the
/// rest of the backend.
///
/// A variadic function spills the argument registers that the caller may still
/// use for unnamed arguments into a save area at the top of its own frame.  The
/// save area is laid out directly below the incoming stack pointer so that it
/// is contiguous with the incoming stack arguments: together they form one
/// array of XLEN-sized argument slots, which is exactly the representation a
/// `va_list` is (a pointer to the next slot).
class MiniRVMachineFunctionInfo : public MachineFunctionInfo {
  /// Frame index of the first saved argument register, or of the first
  /// incoming stack argument when no register has to be saved.
  int VarArgsFrameIndex = 0;

public:
  MiniRVMachineFunctionInfo(const Function &, const MiniRVSubtarget *) {}

  MachineFunctionInfo *
  clone(BumpPtrAllocator &Allocator, MachineFunction &DestMF,
        const DenseMap<MachineBasicBlock *, MachineBasicBlock *> &Src2DstMBB)
      const override {
    return DestMF.cloneInfo<MiniRVMachineFunctionInfo>(*this);
  }

  int getVarArgsFrameIndex() const { return VarArgsFrameIndex; }
  void setVarArgsFrameIndex(int FI) { VarArgsFrameIndex = FI; }
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_MINIRV_MINIRVMACHINEFUNCTIONINFO_H
