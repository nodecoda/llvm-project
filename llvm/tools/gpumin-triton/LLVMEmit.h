//===-- LLVMEmit.h - TTIR-min to GPUMin LLVM IR ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_GPUMIN_TRITON_LLVMEMIT_H
#define LLVM_TOOLS_GPUMIN_TRITON_LLVMEMIT_H

#include "TTIR.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include <memory>

namespace gpumintriton {

/// Lower TTIR-min to a GPUMin LLVM IR module.
///
/// The fixed layout is one Triton program per GPUMin block: 32 threads
/// (8 lanes per warp, 4 warps).  Tensor element r*32+tid lives in thread tid,
/// so every tensor operation becomes 32/N unrolled scalar operations.
llvm::Expected<std::unique_ptr<llvm::Module>>
emitLLVM(const Module &TTIRModule, llvm::LLVMContext &Ctx);

} // namespace gpumintriton

#endif // LLVM_TOOLS_GPUMIN_TRITON_LLVMEMIT_H
